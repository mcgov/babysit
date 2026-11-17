/*
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Matthew G. McGovern
 *
 * This file is part of babysit, which is distributed under the MIT
 * license; see the file LICENSE for details.
 */
/*
 * babysit: "babysit watch" and "babysit wait".
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

  if (look_args("watch", argc, argv, LOOK_WATCH, 0, &o, ops, 1) < 0)
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
  if (expect_compile() < 0)
    return EXIT_USAGE;
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
        if (expect_n && n > PREFIX_LEN &&
            (memcmp(line, "OUT: ", PREFIX_LEN) == 0 ||
             memcmp(line, "ERR: ", PREFIX_LEN) == 0)) {
          *nl = '\0';
          hit = expect_line(line + PREFIX_LEN);
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
              expect_unmet());
      return (expect_n && code == 0) ? 1 : code;
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
      fprintf(stderr, "babysit: %s: timed out after %s; still running%s\n",
              child_id, o.timeout_s, expect_sofar());
      return watch_fail(&in, child_id, o.onfail, sig, EXIT_TIMEOUT);
    }
    if (idle_ms > 0 && now - last >= idle_ms) {
      fprintf(stderr,
              "babysit: %s: stalled: no output for %s; still running%s\n",
              child_id, o.idle_s, expect_sofar());
      return watch_fail(&in, child_id, o.onfail, sig, EXIT_STALLED);
    }
    nanosleep(&tick, NULL);
  }
}

/*
 * babysit wait ID [--timeout SECS]: block, in silence, until the command
 * has exited, and return its exit status; 124 if SECS pass first, with the
 * command still running.
 *
 * Nothing is read and nothing is polled. The babysitter holds a write lock
 * on one byte of the log for exactly as long as it has not reaped the
 * command, so asking for a read lock on that byte and waiting for it is
 * waiting for the command. The kernel wakes us when it is over. For
 * --timeout a timer interrupts the wait, and goes on interrupting it every
 * tenth of a second after that; each time, the caller looks at the clock
 * itself. Nothing is taken from the handler having run: it only exists so
 * that the signal interrupts the wait and does not end the process. On a
 * filesystem that cannot wait for a lock, the state is looked at ten
 * times a second.
 */
static void on_alarm(int sig) { (void)sig; }

/* Wait until the lock on the log's LOCK_CHILD byte can be had, and let it
 * go again at once. 0 when that happened; -1 if a signal cut the wait
 * short, and -2 if there are no locks to wait for here. */
static int wait_for_child_lock(const char *log) {
  struct flock fl;
  int fd = open(log, O_RDONLY), rc = -1;

  if (fd < 0)
    return -1;
  memset(&fl, 0, sizeof fl);
  fl.l_type = F_RDLCK;
  fl.l_whence = SEEK_SET;
  fl.l_start = LOCK_CHILD;
  fl.l_len = 1;
  if (fcntl(fd, F_SETLKW, &fl) == 0)
    rc = 0;
  else if (errno != EINTR)
    rc = -2;
  close(fd); /* which also gives the lock back */
  return rc;
}

int cmd_wait(int argc, char **argv) {
  const char *ops[1] = {NULL}, *child_id;
  struct timespec tick = {0, WATCH_TICK_MS * 1000000L};
  long long timeout_ms = -1, start;
  struct itimerval timer;
  struct instance in;
  struct sigaction sa;
  struct look o;
  int code, state;

  if (look_args("wait", argc, argv, LOOK_WAIT, 0, &o, ops, 1) < 0)
    return EXIT_USAGE;
  child_id = ops[0];
  if (o.timeout_s && parse_interval(o.timeout_s, &timeout_ms) < 0) {
    fprintf(stderr, "babysit: invalid --timeout interval '%s'\n", o.timeout_s);
    return EXIT_USAGE;
  }
  if (find_instance(o.dir, child_id, &in) < 0)
    return EXIT_USAGE;

  start = now_ms();
  if (timeout_ms > 0) {
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm; /* no SA_RESTART: the wait must be cut short */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    memset(&timer, 0, sizeof timer);
    timer.it_value.tv_sec = (time_t)(timeout_ms / 1000);
    timer.it_value.tv_usec = (suseconds_t)((timeout_ms % 1000) * 1000);
    timer.it_interval.tv_usec = (suseconds_t)WATCH_TICK_MS * 1000;
    setitimer(ITIMER_REAL, &timer, NULL);
  }
  for (;;) {
    state = instance_state(&in, &code);
    if (state != ST_RUNNING)
      break;
    if (timeout_ms > 0 && now_ms() - start >= timeout_ms) {
      fprintf(stderr, "babysit: %s: still running after %s\n", child_id,
              o.timeout_s);
      return EXIT_TIMEOUT;
    }
    if (wait_for_child_lock(in.log) == -2)
      nanosleep(&tick, NULL);
  }
  if (state == ST_LOST) {
    fprintf(stderr,
            "babysit: %s: the babysitter is gone and left no exit status; "
            "see the log\n",
            child_id);
    return EXIT_USAGE;
  }
  return code;
}
