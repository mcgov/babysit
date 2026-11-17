/*
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Matthew G. McGovern
 *
 * This file is part of babysit, which is distributed under the MIT
 * license; see the file LICENSE for details.
 */
/*
 * babysit: what the command line and the configuration file say: the --expect
 * pattern, the log directory, signal names, the options of the commands
 * that look at an instance, and the timezone setting.
 */
#include "babysit_internal.h"

const char *expect_pat;

int expect_fixed;

regex_t expect_re;

/* ---------- parent ---------- */

/* Join argv into one line, shell-quoting anything that needs it. */
char *join_cmdline(char **cmd) {
  static const char safe[] =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
      "_-./=:,@%+";
  size_t cap = 1, i;
  char *buf, *o;
  const char *c;

  for (i = 0; cmd[i]; i++)
    cap += (strlen(cmd[i]) * 4) + 3;
  buf = o = malloc(cap);
  if (!buf)
    return NULL;
  for (i = 0; cmd[i]; i++) {
    if (i)
      *o++ = ' ';
    if (cmd[i][0] && strspn(cmd[i], safe) == strlen(cmd[i])) {
      o += sprintf(o, "%s", cmd[i]);
      continue;
    }
    *o++ = '\'';
    for (c = cmd[i]; *c; c++) {
      if (*c == '\'')
        o += sprintf(o, "'\\''");
      else if (*c == '\n') /* keep the header on one line */
        *o++ = ' ';
      else
        *o++ = *c;
    }
    *o++ = '\'';
  }
  *o = '\0';
  return buf;
}

/* Work out the log directory from --log-dir, or its default. */
int resolve_logdir(const char *dir, char *logdir, size_t size) {
  char cwd[PATH_BUF];
  int n;

  if (!dir) {
    if (default_log_dir(logdir, size) < 0) {
      fprintf(stderr, "babysit: HOME is not set; use --log-dir\n");
      return -1;
    }
    return 0;
  }
  if (dir[0] == '/') {
    n = snprintf(logdir, size, "%s", dir);
  } else {
    if (!getcwd(cwd, sizeof cwd)) {
      perror("babysit: getcwd");
      return -1;
    }
    n = snprintf(logdir, size, "%s/%s", cwd, dir);
  }
  if (n < 0 || (size_t)n >= size - 64) {
    fprintf(stderr, "babysit: log path too long\n");
    return -1;
  }
  return 0;
}

static const struct {
  const char *name;
  int sig;
} signals[] = {
    {"HUP", SIGHUP},   {"INT", SIGINT},   {"QUIT", SIGQUIT},
    {"KILL", SIGKILL}, {"TERM", SIGTERM}, {"USR1", SIGUSR1},
    {"USR2", SIGUSR2}, {"STOP", SIGSTOP}, {"CONT", SIGCONT},
};

/* "INT", "SIGINT", "int" or "2" -> 2; -1 if it is none of ours. */
int parse_signal(const char *s) {
  char name[8], *end;
  size_t i;
  long n = strtol(s, &end, 10);

  if (end != s && !*end)
    return (n > 0 && n < 65) ? (int)n : -1;
  if ((s[0] == 'S' || s[0] == 's') && (s[1] == 'I' || s[1] == 'i') &&
      (s[2] == 'G' || s[2] == 'g'))
    s += 3;
  for (i = 0; i < sizeof name - 1 && s[i]; i++)
    name[i] = (char)(s[i] >= 'a' && s[i] <= 'z' ? s[i] - 32 : s[i]);
  name[i] = '\0';
  if (s[i])
    return -1;
  for (i = 0; i < sizeof signals / sizeof signals[0]; i++)
    if (strcmp(name, signals[i].name) == 0)
      return signals[i].sig;
  return -1;
}

int look_args(const char *name, int argc, char **argv, int watch, int info,
              struct look *o, const char **ops, int max) {
  const char *v;
  int i, n = 0;

  memset(o, 0, sizeof *o);
  for (i = 1; i < argc; i++) {
    if (opt_match("babysit", argc, argv, &i, "-l", "--log-dir", &v)) {
      o->dir = v;
    } else if (watch == LOOK_WATCH &&
               opt_match("babysit", argc, argv, &i, "-e", "--expect", &v)) {
      expect_pat = v;
    } else if (watch == LOOK_WATCH &&
               opt_match("babysit", argc, argv, &i, "-i", "--idle", &v)) {
      o->idle_s = v;
    } else if (watch &&
               opt_match("babysit", argc, argv, &i, "-t", "--timeout", &v)) {
      o->timeout_s = v;
    } else if (watch == LOOK_WATCH && opt_match("babysit", argc, argv, &i,
                                                "--onfail", "--onfail", &v)) {
      o->onfail = v;
    } else if (watch == LOOK_WATCH &&
               opt_match("babysit", argc, argv, &i, "-F", "--fixed", NULL)) {
      expect_fixed = 1;
    } else if (info &&
               opt_match("babysit", argc, argv, &i, "--pid", "--pid", NULL)) {
      o->pid = 1;
    } else if (info &&
               opt_match("babysit", argc, argv, &i, "--log", "--log", NULL)) {
      o->log = 1;
    } else if (info && opt_match("babysit", argc, argv, &i, "--exit-code",
                                 "--exit-code", NULL)) {
      o->code = 1;
    } else if (argv[i][0] == '-' && argv[i][1] != '\0' &&
               !(argv[i][1] >= '0' && argv[i][1] <= '9')) {
      fprintf(stderr, "babysit %s: unknown option '%s'\n", name, argv[i]);
      return -1;
    } else if (n < max) {
      ops[n++] = argv[i];
    } else {
      fprintf(stderr, "babysit %s: too many arguments\n", name);
      return -1;
    }
  }
  return n;
}

/* The one setting babysit takes from babysit.conf: timezone. */
int load_timezone(const char *path, int required, int *utc) {
  char line[512], *key, *val;
  FILE *f = fopen(path, "r");
  int lineno = 0;

  if (!f) {
    if (!required)
      return 0;
    fprintf(stderr, "babysit: cannot read %s: %s\n", path, strerror(errno));
    return -1;
  }
  while (fgets(line, sizeof line, f)) {
    lineno++;
    if (config_split(line, &key, &val) == 1 && strcmp(key, "timezone") == 0 &&
        parse_timezone(val, utc) < 0)
      fprintf(stderr, "babysit: %s:%d: timezone must be local or utc\n", path,
              lineno);
  }
  fclose(f);
  return 0;
}
