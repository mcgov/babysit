/*
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Matthew G. McGovern
 *
 * This file is part of babysit, which is distributed under the MIT
 * license; see the file LICENSE for details.
 */
/*
 * babysit: "babysit watch".
 */
#include "babysit_internal.h"

/*
 * babysit watch ID: follow the log of an instance that is already there and
 * return as "start" would have. Whoever started it has gone, and its
 * babysitter is no longer telling anyone anything, so this reads the log
 * file itself, from the beginning: a line that appeared before we got here
 * still counts for --expect. Lines that appear from now on are printed on
 * stdout.
 */
#define WATCH_TICK_MS 100

static int watch_fail(const struct instance *in, const char *child_id,
                      const char *onfail, int sig, int status) {
  if (onfail) {
    if (request_signal(in, sig) == 0)
      fprintf(stderr, "babysit: %s: sent %s to the command\n", child_id,
              onfail);
    else
      fprintf(stderr, "babysit: %s: could not send %s: not running\n", child_id,
              onfail);
  }
  return status;
}

int cmd_watch(int argc, char **argv) {
  static char buf[4 * (PREFIX_LEN + LINE_MAX_ + 1)];
  const char *ops[1] = {NULL}, *child_id;
  long long idle_ms = -1, timeout_ms = -1, start, last, now;
  struct timespec tick = {0, WATCH_TICK_MS * 1000000L};
  struct instance in;
  struct look o;
  struct stat sb;
  size_t len = 0;
  off_t pos = 0, from;
  int fd, sig = 0, code, state, ending = 0;

  if (look_args("watch", argc, argv, 1, 0, &o, ops, 1) < 0)
    return EXIT_USAGE;
  child_id = ops[0];
  if (o.idle_s && parse_interval(o.idle_s, &idle_ms) < 0) {
    fprintf(stderr, "babysit: invalid --idle interval '%s'\n", o.idle_s);
    return EXIT_USAGE;
  }
  if (o.timeout_s && parse_interval(o.timeout_s, &timeout_ms) < 0) {
    fprintf(stderr, "babysit: invalid --timeout interval '%s'\n", o.timeout_s);
    return EXIT_USAGE;
  }
  if (o.onfail)
    sig = parse_signal(o.onfail);
  if (sig < 0) {
    fprintf(stderr, "babysit: unknown signal '%s'\n", o.onfail);
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
  if (find_instance(o.dir, child_id, &in) < 0)
    return EXIT_USAGE;
  fd = open(in.log, O_RDONLY);
  if (fd < 0 || fstat(fd, &sb) < 0) {
    fprintf(stderr, "babysit: cannot read %s: %s\n", in.log, strerror(errno));
    return EXIT_USAGE;
  }
  from = sb.st_size; /* what is there already is not printed */

  start = last = now_ms();
  for (;;) {
    ssize_t r = read(fd, buf + len, sizeof buf - len);
    char *line = buf, *nl;

    if (r > 0) {
      last = now_ms();
      len += (size_t)r;
      while ((nl = memchr(line, '\n', len - (size_t)(line - buf))) != NULL) {
        size_t n = (size_t)(nl - line) + 1;
        int hit = 0;

        if (pos >= from) {
          ssize_t w = write(1, line, n);
          (void)w;
        }
        if (expect_pat && n > PREFIX_LEN &&
            (memcmp(line, "OUT: ", PREFIX_LEN) == 0 ||
             memcmp(line, "ERR: ", PREFIX_LEN) == 0)) {
          *nl = '\0';
          hit = expect_fixed
                    ? strstr(line + PREFIX_LEN, expect_pat) != NULL
                    : regexec(&expect_re, line + PREFIX_LEN, 0, NULL, 0) == 0;
        }
        if (hit) {
          fprintf(stderr, "babysit: %s: expected output seen\n", child_id);
          return 0;
        }
        pos += (off_t)n;
        line = nl + 1;
      }
      len -= (size_t)(line - buf);
      memmove(buf, line, len);
      if (len == sizeof buf) /* not a log of ours: do not get stuck on it */
        len = 0;
      continue;
    }
    /* Nothing more to read for now. Has the command ended? If it has, the
     * rest of the log is read once more before its status is returned. */
    state = instance_state(&in, &code);
    if (state == ST_EXITED) {
      if (!ending) {
        ending = 1;
        continue;
      }
      fprintf(stderr, "babysit: %s: exited with status %d%s\n", child_id, code,
              expect_pat ? " before the expected output" : "");
      return (expect_pat && code == 0) ? 1 : code;
    }
    if (state == ST_LOST) {
      fprintf(stderr,
              "babysit: %s: the babysitter is gone and left no exit "
              "status; see the log\n",
              child_id);
      return EXIT_USAGE;
    }
    now = now_ms();
    if (timeout_ms > 0 && now - start >= timeout_ms) {
      fprintf(stderr, "babysit: %s: timed out after %s; still running\n",
              child_id, o.timeout_s);
      return watch_fail(&in, child_id, o.onfail, sig, EXIT_TIMEOUT);
    }
    if (idle_ms > 0 && now - last >= idle_ms) {
      fprintf(stderr, "babysit: %s: stalled: no output for %s; still running\n",
              child_id, o.idle_s);
      return watch_fail(&in, child_id, o.onfail, sig, EXIT_STALLED);
    }
    nanosleep(&tick, NULL);
  }
}
