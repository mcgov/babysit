/*
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Matthew G. McGovern
 *
 * This file is part of babysit, which is distributed under the MIT
 * license; see the file LICENSE for details.
 */
/*
 * babysit: "babysit check", "babysit info", "babysit signal" and "babysit cat".
 */
#include "babysit_internal.h"

/* babysit check ID: 0 if the command is running, 1 if it is not. */
int cmd_check(int argc, char **argv) {
  const char *ops[1] = {NULL};
  struct instance in;
  struct look o;
  int code;

  if (look_args("check", argc, argv, 0, 0, &o, ops, 1) < 0 ||
      find_instance(o.dir, ops[0], &in) < 0)
    return EXIT_USAGE;
  switch (instance_state(&in, &code)) {
  case ST_RUNNING:
    puts("running");
    return 0;
  case ST_EXITED:
    printf("exited %d\n", code);
    return 1;
  default:
    puts("lost");
    return 1;
  }
}

/* babysit info ID [--pid | --log | --exit-code] */
int cmd_info(int argc, char **argv) {
  const char *ops[1] = {NULL};
  struct instance in;
  struct look o;
  int code, state;
  long pid;

  if (look_args("info", argc, argv, 0, 1, &o, ops, 1) < 0 ||
      o.pid + o.log + o.code > 1 || find_instance(o.dir, ops[0], &in) < 0) {
    if (o.pid + o.log + o.code > 1)
      fprintf(stderr, "babysit info: give one of --pid, --log and "
                      "--exit-code\n");
    return EXIT_USAGE;
  }
  state = instance_state(&in, &code);
  /* A pid is given out only while the babysitter vouches for it, that is
   * until it has reaped the command: after that the number may be
   * anyone's. */
  pid = state == ST_RUNNING ? logged_pid(in.log) : -1;
  if (o.log) {
    puts(in.log);
    return 0;
  }
  if (o.pid) {
    if (pid < 0) {
      fprintf(stderr, "babysit: the command of %s is not running\n", ops[0]);
      return 1;
    }
    printf("%ld\n", pid);
    return 0;
  }
  if (o.code) {
    if (state != ST_EXITED) {
      fprintf(stderr, "babysit: the command of %s has no exit status%s\n",
              ops[0], state == ST_RUNNING ? " yet: it is running" : "");
      return 1;
    }
    printf("%d\n", code);
    return 0;
  }
  printf("log: %s\n", in.log);
  if (state == ST_RUNNING) {
    puts("state: running");
    if (pid > 0)
      printf("pid: %ld\n", pid);
  } else if (state == ST_EXITED) {
    printf("state: exited\nexit_code: %d\n", code);
  } else {
    puts("state: lost");
  }
  return 0;
}

/* babysit signal SIGNAL ID */
int cmd_signal(int argc, char **argv) {
  const char *ops[2] = {NULL, NULL};
  struct instance in;
  struct look o;
  int sig;

  if (look_args("signal", argc, argv, 0, 0, &o, ops, 2) != 2) {
    fprintf(stderr, "babysit signal: expected a signal and an instance "
                    "identifier\n");
    return EXIT_USAGE;
  }
  sig = parse_signal(ops[0]);
  if (sig < 0) {
    fprintf(stderr, "babysit signal: unknown signal '%s'\n", ops[0]);
    return EXIT_USAGE;
  }
  if (find_instance(o.dir, ops[1], &in) < 0)
    return EXIT_USAGE;
  if (request_signal(&in, sig) < 0) {
    fprintf(stderr, "babysit: the command of %s is not running\n", ops[1]);
    return 1;
  }
  return 0;
}

/*
 * babysit cat [stdout] [stderr] CHILD_ID: print what the command has
 * written so far, as the command wrote it, that is without the "OUT: " and
 * "ERR: " labels the log gives each line. Naming one stream prints that
 * one; naming neither, or both, prints both, in the order they are in the
 * log. Everything else in the log (the command line, the lines about the
 * process, babysit's own notes) is left out.
 */
int cmd_cat(int argc, char **argv) {
  const char *ops[3] = {NULL, NULL, NULL}, *child_id = NULL;
  struct instance in;
  struct look o;
  char *line = NULL;
  size_t cap = 0;
  ssize_t len;
  FILE *f;
  int n, i, out = 0, err = 0, first = 1;

  n = look_args("cat", argc, argv, 0, 0, &o, ops, 3);
  if (n < 0)
    return EXIT_USAGE;
  for (i = 0; i < n; i++) {
    if (strcmp(ops[i], "stdout") == 0) {
      out = 1;
    } else if (strcmp(ops[i], "stderr") == 0) {
      err = 1;
    } else if (!child_id) {
      child_id = ops[i];
    } else {
      fprintf(stderr, "babysit cat: too many arguments\n");
      return EXIT_USAGE;
    }
  }
  if (!out && !err)
    out = err = 1;
  if (find_instance(o.dir, child_id, &in) < 0)
    return EXIT_USAGE;
  f = fopen(in.log, "r");
  if (!f) {
    fprintf(stderr, "babysit: cannot read %s: %s\n", in.log, strerror(errno));
    return EXIT_USAGE;
  }
  /* getline(), not fgets(): output may contain NUL bytes. The first line
   * is the command line, which could itself begin with a label. */
  while ((len = getline(&line, &cap, f)) > 0) {
    if (first) {
      first = 0;
      continue;
    }
    if (len > PREFIX_LEN && ((out && memcmp(line, "OUT: ", PREFIX_LEN) == 0) ||
                             (err && memcmp(line, "ERR: ", PREFIX_LEN) == 0)))
      fwrite(line + PREFIX_LEN, 1, (size_t)len - PREFIX_LEN, stdout);
  }
  free(line);
  fclose(f);
  return fflush(stdout) == 0 ? 0 : EXIT_USAGE;
}
