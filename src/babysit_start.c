/*
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Matthew G. McGovern
 *
 * This file is part of babysit, which is distributed under the MIT
 * license; see the file LICENSE for details.
 */
/*
 * babysit: "babysit start": the parent, which creates the instance, forks the
 * babysitter and waits to be told why it should return.
 */
#include "babysit_internal.h"

static int read_msg(int fd, struct msg *m) {
  char *p = (char *)m;
  size_t got = 0;

  while (got < sizeof *m) {
    ssize_t r = read(fd, p + got, sizeof *m - got);
    if (r < 0 && errno == EINTR)
      continue;
    if (r <= 0)
      return -1;
    got += (size_t)r;
  }
  return 0;
}

/*
 * --follow: copy to our stderr whatever has been added to the log since the
 * last time. It is this process that copies, never the babysitter: a
 * terminal that is slow to take the output delays our return and nothing
 * else, neither the command nor the timers.
 */
static void follow_log(int fd) {
  static char buf[1 << 16];
  ssize_t r;

  if (fd < 0)
    return;
  while ((r = read(fd, buf, sizeof buf)) > 0)
    if (write_all(2, buf, (size_t)r) < 0)
      break;
}

/* babysit start [options] [--] command [args...] */
int cmd_start(int argc, char **argv) {
  const char *idle_s = NULL, *timeout_s = NULL, *dir = NULL, *v;
  const char *config = NULL;
  char logdir[PATH_BUF], cfg[PATH_BUF], child_id[ID_MAX];
  char minute[ID_MAX];
  char path[PATH_BUF + ID_MAX + 8];
  char *cmdline, **cmd;
  long long idle_ms = -1, timeout_ms = -1;
  int detach = 0, follow = 0, follow_fd = -1, utc = 0, i, n, st;
  int status_pipe[2];
  struct pollfd waiting;
  struct msg m;
  pid_t pid, mid;

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    }
    if (argv[i][0] != '-' || argv[i][1] == '\0')
      break;
    if (opt_match("babysit", argc, argv, &i, "-e", "--expect", &v)) {
      expect_pat = v;
    } else if (opt_match("babysit", argc, argv, &i, "-i", "--idle", &v)) {
      idle_s = v;
    } else if (opt_match("babysit", argc, argv, &i, "-t", "--timeout", &v)) {
      timeout_s = v;
    } else if (opt_match("babysit", argc, argv, &i, "-l", "--log-dir", &v)) {
      dir = v;
    } else if (opt_match("babysit", argc, argv, &i, "-c", "--config", &v)) {
      config = v;
    } else if (opt_match("babysit", argc, argv, &i, "-F", "--fixed", NULL)) {
      expect_fixed = 1;
    } else if (opt_match("babysit", argc, argv, &i, "-d", "--detach", NULL)) {
      detach = 1;
    } else if (opt_match("babysit", argc, argv, &i, "-f", "--follow", NULL)) {
      follow = 1;
    } else {
      fprintf(stderr, "babysit: unknown option '%s'\n\n", argv[i]);
      usage(stderr);
      return EXIT_USAGE;
    }
  }
  cmd = argv + i;
  if (!*cmd) {
    fprintf(stderr, "babysit: no command given\n\n");
    usage(stderr);
    return EXIT_USAGE;
  }

  if (idle_s && parse_interval(idle_s, &idle_ms) < 0) {
    fprintf(stderr, "babysit: invalid --idle interval '%s'\n", idle_s);
    return EXIT_USAGE;
  }
  if (timeout_s && parse_interval(timeout_s, &timeout_ms) < 0) {
    fprintf(stderr, "babysit: invalid --timeout interval '%s'\n", timeout_s);
    return EXIT_USAGE;
  }
  if (expect_pat && !expect_fixed) {
    int rc = regcomp(&expect_re, expect_pat, REG_EXTENDED | REG_NOSUB);
    if (rc != 0) {
      char msg[256];
      regerror(rc, &expect_re, msg, sizeof msg);
      fprintf(stderr, "babysit: bad --expect pattern: %s\n", msg);
      return EXIT_USAGE;
    }
  }

  if (resolve_logdir(dir, logdir, sizeof logdir) < 0)
    return EXIT_USAGE;
  if (config) {
    if (load_timezone(config, 1, &utc) < 0)
      return EXIT_USAGE;
  } else if (default_config_path(cfg, sizeof cfg) == 0) {
    load_timezone(cfg, 0, &utc);
  }
  if (minute_path(time(NULL), utc, minute, sizeof minute) < 0) {
    fprintf(stderr, "babysit: cannot tell the time\n");
    return EXIT_USAGE;
  }
  cmdline = join_cmdline(cmd);
  if (!cmdline) {
    perror("babysit: malloc");
    return EXIT_USAGE;
  }
  if (new_instance(logdir, minute, child_id, sizeof child_id) < 0) {
    fprintf(stderr, "babysit: cannot create a log directory under %s: %s\n",
            logdir, strerror(errno));
    free(cmdline);
    return EXIT_USAGE;
  }
  snprintf(path, sizeof path, "%s/%s", logdir, child_id);

  if (pipe(status_pipe) < 0) {
    perror("babysit: pipe");
    free(cmdline);
    drop_instance(logdir, child_id);
    return EXIT_USAGE;
  }
  fflush(NULL);
  /* A caller that has closed our stderr gets no messages; it still gets
   * the exit status, which a SIGPIPE would take away. */
  signal(SIGPIPE, SIG_IGN);

  /* Double fork + setsid: the babysitter is reparented to init and has no
   * controlling terminal, so it outlives us and our caller. */
  mid = fork();
  if (mid < 0) {
    perror("babysit: fork");
    free(cmdline);
    drop_instance(logdir, child_id);
    return EXIT_USAGE;
  }
  if (mid == 0) {
    pid_t bp;

    setsid();
    bp = fork();
    if (bp != 0)
      _exit(bp < 0 ? 1 : 0);
    close(status_pipe[0]);
    status_fd = status_pipe[1];
    babysitter(path, cmd, cmdline, detach, idle_ms, timeout_ms);
    _exit(0);
  }
  close(status_pipe[1]);
  free(cmdline);
  if (expect_pat && !expect_fixed)
    regfree(&expect_re);
  while (waitpid(mid, &st, 0) < 0 && errno == EINTR)
    ;

  if (read_msg(status_pipe[0], &m) < 0) {
    fprintf(stderr, "babysit: the babysitter failed to start\n");
    drop_instance(logdir, child_id);
    return EXIT_USAGE;
  }
  if (m.type == MSG_ERR_EXEC) {
    /* Nothing ran, but the instance is there to say so. */
    puts(child_id);
    fflush(stdout);
    fprintf(stderr, "babysit: %s: cannot execute '%s': %s\n", child_id, cmd[0],
            strerror(m.value));
    return m.value == ENOENT ? 127 : 126;
  }
  if (m.type != MSG_START) {
    fprintf(stderr, "babysit: cannot create %s/log: %s\n", path,
            strerror(m.value));
    drop_instance(logdir, child_id);
    return EXIT_USAGE;
  }
  pid = (pid_t)m.value;

  /* The command is running: from here on stdout says which instance it
   * is, and says nothing else. */
  puts(child_id);
  fflush(stdout);

  if (follow) {
    snprintf(path, sizeof path, "%s/%s/log", logdir, child_id);
    follow_fd = open(path, O_RDONLY);
  }

  /* Now just wait to be told why to return, showing the log as it grows
   * if that was asked for. A warning about the log may come first; it is
   * passed on, and the wait goes on. */
  for (;;) {
    follow_log(follow_fd);
    if (follow_fd >= 0) {
      waiting.fd = status_pipe[0];
      waiting.events = POLLIN;
      waiting.revents = 0;
      if (poll(&waiting, 1, FOLLOW_TICK_MS) == 0)
        continue;
    }
    n = read_msg(status_pipe[0], &m);
    if (n < 0 || m.type != MSG_WARN_LOG)
      break;
    fprintf(stderr,
            "babysit: %s: warning: cannot write to the log (%s); output is "
            "missing from it from here on\n",
            child_id, strerror(m.value));
  }
  follow_log(follow_fd); /* the babysitter wrote the log before it told us */
  if (n < 0) {
    fprintf(stderr,
            "babysit: %s: lost contact with the babysitter; see the "
            "log\n",
            child_id);
    return EXIT_USAGE;
  }
  switch (m.type) {
  case MSG_DETACHED:
    return 0;
  case MSG_MATCH:
    fprintf(stderr,
            "babysit: %s: expected output seen; pid %ld continues in the "
            "background\n",
            child_id, (long)pid);
    return 0;
  case MSG_STALL:
    fprintf(stderr,
            "babysit: %s: stalled: no output for %s; pid %ld is still "
            "running\n",
            child_id, idle_s, (long)pid);
    return EXIT_STALLED;
  case MSG_TIMEOUT:
    fprintf(stderr,
            "babysit: %s: timed out after %s; pid %ld is still running\n",
            child_id, timeout_s, (long)pid);
    return EXIT_TIMEOUT;
  case MSG_SIGNAL:
    fprintf(stderr, "babysit: %s: killed by signal %d%s\n", child_id, m.value,
            expect_pat ? " before the expected output" : "");
    return 128 + m.value;
  case MSG_EXIT:
    fprintf(stderr, "babysit: %s: exited with code %d%s\n", child_id, m.value,
            expect_pat ? " before the expected output" : "");
    return (expect_pat && m.value == 0) ? 1 : m.value;
  default:
    break;
  }
  return EXIT_USAGE;
}
