/*
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Matthew G. McGovern
 *
 * This file is part of babysit, which is distributed under the MIT
 * license; see the file LICENSE for details.
 */
/*
 * babysit - run a command under a babysitter that logs it and tells you
 * early when it stalls, prints an expected line, or runs too long.
 *
 * Three processes are involved:
 *
 *   parent      the babysit you invoked. It creates the instance, prints
 *               its identifier on stdout, then only waits for the
 *               babysitter to say why it should return, and turns that
 *               into an exit code.
 *   babysitter  detached (own session). Owns the command's stdout/stderr
 *               pipes, writes the log, echoes it to the caller's stderr,
 *               and runs the timers. When it decides the parent should
 *               return it closes its copy of the caller's stderr and keeps
 *               logging.
 *   child       the command. Its stdio never touches the caller's, so the
 *               parent and babysitter can let go without disturbing it.
 *
 * Instance: <YYYYmmdd>/<HH>/<MM>/<salt>, which is also where its log is:
 * <log-dir>/<instance>/log. See babysit.h.
 *
 * Stdout carries the identifier and nothing else, so that
 * id=$(babysit start ...) is all a caller needs to find the instance again.
 *
 * The babysitter records the command's exit status in <instance>/exit_code
 * and takes signals for it from <instance>/ctl. The other subcommands
 * (watch, check, info, signal) only ever look at an instance from outside,
 * through those files, the log and its locks.
 *
 * POSIX.1-2008, C99, no extensions.
 */
#include "babysit.h"
#include <regex.h>

#define EXIT_TIMEOUT 124 /* same as timeout(1) */
#define EXIT_STALLED 125

#define PREFIX_LEN 5
#define LINE_MAX_ 4096
#define PARTIAL_FLUSH_MS 1000 /* log an unterminated line after this long */
#define ECHO_FD 2 /* the caller's stderr; its stdout is for the identifier */

enum {
  MSG_START = 1, /* value: child pid */
  MSG_ERR_LOG,   /* value: errno */
  MSG_ERR_EXEC,  /* value: errno */
  MSG_EXIT,      /* value: exit code */
  MSG_SIGNAL,    /* value: signal number */
  MSG_MATCH,
  MSG_STALL,
  MSG_TIMEOUT,
  MSG_DETACHED
};
struct msg {
  int type;
  int value;
};

struct lstream {
  int fd;
  size_t len;
  char buf[PREFIX_LEN + LINE_MAX_ + 1]; /* prefix, data, newline */
};

static const char *expect_pat;
static int fixed;
static regex_t expect_re;

/* babysitter state */
static int log_fd = -1, status_fd = -1;
static int watching; /* still echoing to the caller and running timers */
static long long last_read;
static long long timeout_at = -1; /* when --timeout expires, in now_ms() */
static int chld_pipe[2] = {-1, -1};
static char ctl_path[PATH_BUF + ID_MAX + 16]; /* our control FIFO, if any */
static int ctl_fd = -1, ctl_wfd = -1;

static void usage(FILE *f) {
  fprintf(
      f,
      "Usage: babysit start [options] [--] command [args...]\n"
      "       babysit watch ID [options]\n"
      "       babysit check ID\n"
      "       babysit info ID [--pid | --log | --exit-code]\n"
      "       babysit signal SIGNAL ID\n"
      "\n"
      "start runs COMMAND detached from this terminal/pipe and logs its "
      "output to\n"
      "LOGDIR/ID/log, where ID identifies this run: the minute it started "
      "and four\n"
      "random characters, for example 20261116/06/15/k3f9. ID is printed on "
      "stdout,\n"
      "alone; the command's output is echoed on stderr, each line prefixed "
      "with\n"
      "'OUT: ' (stdout) or 'ERR: ' (stderr). When the command exits, its "
      "status is\n"
      "recorded in LOGDIR/ID/exit_code. The other commands take that ID.\n"
      "\n"
      "By default start waits for the command and returns its exit status. "
      "The\n"
      "options below make it return early. When it does, the command is NOT\n"
      "stopped: it keeps running and its output keeps going to the log.\n"
      "\n"
      "Options for start and watch:\n"
      "  -e, --expect REGEX   return 0 once an output line matches the POSIX\n"
      "                       extended REGEX (matched without the OUT:/ERR: "
      "prefix)\n"
      "  -F, --fixed          treat the --expect pattern as a plain substring\n"
      "  -i, --idle SECS      return 125 if the command writes nothing for "
      "SECS\n"
      "  -t, --timeout SECS   return 124 after SECS in total\n"
      "Options for start:\n"
      "  -d, --detach         return 0 as soon as the command has started\n"
      "  -c, --config FILE    configuration file (default:\n"
      "                       $XDG_CONFIG_HOME/babysit/babysit.conf, or\n"
      "                       $HOME/.config/babysit/babysit.conf)\n"
      "Options for watch:\n"
      "      --onfail SIGNAL  on returning 124 or 125, send SIGNAL to the "
      "command\n"
      "Options for every command:\n"
      "  -l, --log-dir DIR    base log directory\n"
      "                       (default: $HOME/.local/var/log/babysit)\n"
      "  -V, --version        show the version\n"
      "  -h, --help           show this help\n"
      "\n"
      "watch follows the log of a run that was started earlier, from its "
      "beginning,\n"
      "and returns as start would; with no options it waits for the command "
      "to exit.\n"
      "check says whether the command is running (exit 0) or not (exit 1).\n"
      "info prints the run's log path, state and, while it is running, pid; "
      "or just\n"
      "the one asked for (--pid fails once the command has exited, "
      "--exit-code\n"
      "until it has). signal has the babysitter send SIGNAL (INT, TERM, 9, "
      "...) to\n"
      "the command's process group.\n"
      "\n"
      "SECS may be fractional and may carry an s, m or h suffix (0.5, 30s, "
      "2m).\n"
      "The command gets /dev/null as stdin and its own process group.\n"
      "\n"
      "Exit status of start and watch:\n"
      "  N        the command exited with status N (128+signal if killed)\n"
      "  0        --expect matched, or --detach\n"
      "  1        --expect was given but the command exited 0 without a match\n"
      "  124      --timeout expired; command still running\n"
      "  125      stalled for --idle seconds; command still running\n"
      "  126/127  the command could not be executed / was not found\n"
      "  75       the clock has gone backwards; nothing was started; try "
      "again\n"
      "           once babysitd has packed the records dated later than "
      "now\n"
      "  2        usage or internal error, or no such run\n");
}

/* ---------- helpers ---------- */

/* Monotonic milliseconds where available, wall clock otherwise. */
static long long now_ms(void) {
  struct timeval tv;
#ifdef CLOCK_MONOTONIC
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
    return ((long long)ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
#endif
  gettimeofday(&tv, NULL);
  return ((long long)tv.tv_sec * 1000) + (tv.tv_usec / 1000);
}

/* Parse "5", "0.5", "30s", "2m", "1h" into milliseconds (> 0). */
static int parse_interval(const char *s, long long *ms) {
  char *end;
  double d, mult = 1;

  errno = 0;
  d = strtod(s, &end);
  if (end == s || errno)
    return -1;
  if (*end == 's') {
    end++;
  } else if (*end == 'm') {
    mult = 60;
    end++;
  } else if (*end == 'h') {
    mult = 3600;
    end++;
  }
  if (*end || !(d > 0) || d * mult > 1e9)
    return -1;
  *ms = (long long)((d * mult * 1000) + 0.5);
  if (*ms < 1)
    *ms = 1;
  return 0;
}

static int write_all(int fd, const void *buf, size_t n) {
  const char *p = buf;

  while (n > 0) {
    ssize_t w = write(fd, p, n);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    p += w;
    n -= (size_t)w;
  }
  return 0;
}

static void set_nonblock(int fd) {
  int fl = fcntl(fd, F_GETFL);
  if (fl >= 0)
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void set_cloexec(int fd) {
  int fl = fcntl(fd, F_GETFD);
  if (fl >= 0)
    fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
}

/* ---------- babysitter ---------- */

static void on_sigchld(int sig) {
  int saved = errno;
  ssize_t w = write(chld_pipe[1], "", 1);
  (void)w;
  (void)sig;
  errno = saved;
}

static void send_msg(int type, int value) {
  struct msg m;

  m.type = type;
  m.value = value;
  if (status_fd >= 0 && write_all(status_fd, &m, sizeof m) < 0) {
    close(status_fd);
    status_fd = -1;
  }
}

/*
 * Stop watching: close our copy of the caller's stderr, then tell the
 * parent why so it can return. Logging carries on regardless.
 */
static void release(int type, int value) {
  int devnull;

  if (!watching)
    return;
  watching = 0;
  devnull = open("/dev/null", O_WRONLY);
  if (devnull >= 0) {
    dup2(devnull, ECHO_FD);
    close(devnull);
  } else {
    close(ECHO_FD);
  }
  send_msg(type, value);
  if (status_fd >= 0) {
    close(status_fd);
    status_fd = -1;
  }
}

/*
 * Copy output to the caller's stderr. The caller may read slowly or not at
 * all, and a plain write() would then block for as long as it likes, with
 * --timeout unable to fire and a killed parent going unnoticed. So wait for
 * room with poll(), keeping an eye on both, and write no more at a time
 * than a pipe that reports room is sure to take. The descriptor is shared
 * with the caller, so it cannot simply be made non-blocking.
 *
 * Time spent waiting here is the caller's silence, not the command's, and
 * does not count towards --idle.
 */
static void echo(const char *buf, size_t n) {
  struct pollfd p[2];
  int waited = 0;

  while (watching && n > 0) {
    long long rem = -1;
    ssize_t w;
    int r;

    if (timeout_at >= 0) {
      rem = timeout_at - now_ms();
      if (rem <= 0) {
        release(MSG_TIMEOUT, 0);
        break;
      }
    }
    p[0].fd = ECHO_FD;
    p[0].events = POLLOUT;
    p[1].fd = status_fd; /* errors out if the parent dies */
    p[1].events = 0;
    p[0].revents = p[1].revents = 0;
    r = poll(p, 2, 0);
    if (r == 0) {
      if (rem > INT_MAX)
        rem = INT_MAX;
      waited = 1;
      r = poll(p, 2, (int)rem); /* -1 without --timeout: no limit */
    }
    if (r < 0 && errno != EINTR) {
      release(MSG_DETACHED, 0);
      break;
    }
    if (r <= 0)
      continue; /* the deadline is looked at again above */
    if (p[1].revents) {
      release(MSG_DETACHED, 0); /* parent was killed: let go too */
      break;
    }
    if (!p[0].revents)
      continue;
    w = write(ECHO_FD, buf, n > PIPE_BUF ? PIPE_BUF : n);
    if (w < 0) {
      if (errno == EINTR || errno == EAGAIN)
        continue;
      release(MSG_DETACHED, 0); /* caller stopped reading */
      break;
    }
    buf += w;
    n -= (size_t)w;
  }
  if (waited)
    last_read = now_ms();
}

/*
 * Write to the log. The disk can fill up, or a file size limit be reached,
 * while the command is running; that is no reason to stop it, but it must
 * not pass unnoticed either. The caller is told once, on its stderr, and
 * what the log missed is counted. Each later write tries again, and as soon
 * as one gets through the log first gets a line saying how much is missing
 * at that point.
 */
static unsigned long long lost; /* bytes the log did not take */
static int lost_err, lost_told;

static void log_write(const char *buf, size_t n) {
  char note[256];
  int k;

  if (lost) {
    k = snprintf(note, sizeof note,
                 "babysit: %llu bytes of output are missing here: %s\n", lost,
                 strerror(lost_err));
    if (write_all(log_fd, note, (size_t)k) == 0)
      lost = 0;
  }
  if (!lost && write_all(log_fd, buf, n) == 0)
    return;
  if (!lost)
    lost_err = errno;
  lost += n;
  if (!lost_told) {
    lost_told = 1;
    k = snprintf(note, sizeof note,
                 "babysit: warning: cannot write to the log (%s); output is "
                 "missing from it from here on\n",
                 strerror(lost_err));
    echo(note, (size_t)k);
  }
}

static void emit(const char *buf, size_t n) {
  log_write(buf, n);
  echo(buf, n);
}

static void flush_line(struct lstream *s) {
  char *data = s->buf + PREFIX_LEN;
  int hit = 0;

  if (watching && expect_pat) {
    data[s->len] = '\0';
    hit = fixed ? strstr(data, expect_pat) != NULL
                : regexec(&expect_re, data, 0, NULL, 0) == 0;
  }
  data[s->len] = '\n';
  emit(s->buf, PREFIX_LEN + s->len + 1);
  s->len = 0;
  if (hit)
    release(MSG_MATCH, 0);
}

static void flush_partial(struct lstream *s) {
  if (s->len)
    flush_line(s);
}

/* Returns 1 if data was read, 0 on EOF (stream closed), -1 if nothing yet. */
static int read_stream(struct lstream *s) {
  char tmp[4096];
  ssize_t r = read(s->fd, tmp, sizeof tmp), i;

  if (r > 0) {
    last_read = now_ms(); /* any byte counts as progress */
    for (i = 0; i < r; i++) {
      if (tmp[i] == '\n') {
        flush_line(s);
        continue;
      }
      s->buf[PREFIX_LEN + s->len++] = tmp[i];
      if (s->len == LINE_MAX_)
        flush_line(s);
    }
    return 1;
  }
  if (r < 0 && (errno == EAGAIN || errno == EINTR))
    return -1;
  flush_partial(s);
  close(s->fd);
  s->fd = -1;
  return 0;
}

static void drain(struct lstream *s) {
  while (s->fd >= 0 && read_stream(s) == 1)
    ;
  flush_partial(s);
}

/*
 * The control FIFO, <instance>/ctl: "babysit signal" writes a signal number
 * and a newline to it, and we deliver the signal. We are the command's
 * parent, so until we have reaped it its pid cannot have passed to anyone
 * else, which nobody looking the pid up from outside can be sure of.
 * Holding a write end ourselves keeps the read end from reporting hang-up.
 */
static void ctl_open(const char *dir) {
  snprintf(ctl_path, sizeof ctl_path, "%s/ctl", dir);
  if (mkfifo(ctl_path, 0600) < 0)
    return;
  ctl_fd = open(ctl_path, O_RDONLY | O_NONBLOCK);
  ctl_wfd = ctl_fd >= 0 ? open(ctl_path, O_WRONLY | O_NONBLOCK) : -1;
}

/* No more signals: the command is gone, or we are. */
static void ctl_close(void) {
  if (ctl_fd >= 0)
    close(ctl_fd);
  if (ctl_wfd >= 0)
    close(ctl_wfd);
  ctl_fd = ctl_wfd = -1;
  if (ctl_path[0])
    unlink(ctl_path);
  ctl_path[0] = '\0';
}

/* Deliver every signal asked for so far to the command's process group. */
static void ctl_read(pid_t child) {
  static char buf[32];
  static size_t len;
  char line[96], c;
  int sig, n;

  while (read(ctl_fd, &c, 1) == 1) {
    if (c != '\n') {
      if (len < sizeof buf - 1)
        buf[len++] = c;
      continue;
    }
    buf[len] = '\0';
    len = 0;
    sig = (int)strtol(buf, NULL, 10);
    if (sig <= 0)
      continue;
    if (kill(-child, sig) < 0 && kill(child, sig) < 0)
      continue;
    n = snprintf(line, sizeof line,
                 "babysit: signal %d sent to the command on request\n", sig);
    emit(line, (size_t)n);
  }
}

/*
 * Record how the command ended in <instance>/exit_code: one number, as a
 * shell would report it (128 + signal if it was killed). Written under
 * another name and renamed, so a reader sees all of it or none.
 */
static void record_exit(const char *dir, int code) {
  char path[PATH_BUF + ID_MAX + 32], tmp[PATH_BUF + ID_MAX + 40], text[16];
  int fd, n;

  snprintf(path, sizeof path, "%s/exit_code", dir);
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    return;
  n = snprintf(text, sizeof text, "%d\n", code);
  if (write_all(fd, text, (size_t)n) < 0 || close(fd) < 0 ||
      rename(tmp, path) < 0)
    unlink(tmp);
}

/* The babysitter's way out. */
static void leave(int code) {
  ctl_close();
  COVERAGE_FLUSH();
  _exit(code);
}

static void babysitter(const char *dir, char **cmd, const char *cmdline,
                       int detach, long long idle_ms, long long timeout_ms) {
  static struct lstream out, err;
  struct sigaction sa;
  struct pollfd p[5];
  char path[PATH_BUF + ID_MAX + 16], line[256];
  int op[2], ep[2], go[2], xp[2];
  int devnull, e, n, status = 0, reaped = 0, lock_err = 0;
  long long start;
  pid_t child;

  signal(SIGPIPE, SIG_IGN);
  signal(SIGXFSZ, SIG_IGN); /* a log at its size limit: EFBIG, not death */
  watching = 1;

  /* Keep only the caller's stderr (for echoing); drop stdin and stdout,
   * so that a caller reading the identifier from stdout sees end-of-file
   * as soon as the parent has printed it and gone. */
  devnull = open("/dev/null", O_RDWR);
  if (devnull >= 0) {
    dup2(devnull, 0);
    dup2(devnull, 1);
    if (devnull > 2)
      close(devnull);
  }

  if (pipe(op) < 0 || pipe(ep) < 0 || pipe(go) < 0 || pipe(xp) < 0 ||
      pipe(chld_pipe) < 0) {
    send_msg(MSG_ERR_LOG, errno);
    leave(1);
  }
  set_cloexec(xp[1]);
  set_nonblock(chld_pipe[0]);
  set_nonblock(chld_pipe[1]);
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_sigchld;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGCHLD, &sa, NULL);

  child = fork();
  if (child < 0) {
    send_msg(MSG_ERR_LOG, errno);
    leave(1);
  }
  if (child == 0) {
    char c;
    ssize_t w;

    setpgid(0, 0);
    dup2(op[1], 1);
    dup2(ep[1], 2);
    close(op[0]);
    close(op[1]);
    close(ep[0]);
    close(ep[1]);
    close(go[1]);
    close(xp[0]);
    close(chld_pipe[0]);
    close(chld_pipe[1]);
    close(status_fd);
    signal(SIGPIPE, SIG_DFL);
    signal(SIGXFSZ, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
    /* Wait until the babysitter has the log file open. */
    if (read(go[0], &c, 1) != 1)
      _exit(127);
    close(go[0]);
    COVERAGE_FLUSH(); /* exec replaces us; record what ran so far */
    /* Running what the caller named is the whole point. */
    /* NOLINTNEXTLINE(clang-analyzer-optin.taint.GenericTaint) */
    execvp(cmd[0], cmd);
    e = errno;
    w = write(xp[1], &e, sizeof e);
    (void)w;
    _exit(127);
  }
  close(op[1]);
  close(ep[1]);
  close(go[0]);
  close(xp[1]);

  /* The parent made the instance's directory; the log in it is ours. The
   * locks mark it as being written and the command as not yet reaped. */
  snprintf(path, sizeof path, "%s/log", dir);
  log_fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_APPEND, 0644);
  if (log_fd >= 0 && (lock_byte(log_fd, LOCK_WRITER, F_WRLCK) < 0 ||
                      lock_byte(log_fd, LOCK_CHILD, F_WRLCK) < 0))
    lock_err = errno;
  if (log_fd < 0) {
    send_msg(MSG_ERR_LOG, errno);
    close(go[1]); /* child sees EOF and exits without exec */
    leave(1);
  }

  if (write(go[1], "", 1) != 1) {
    send_msg(MSG_ERR_LOG, errno);
    leave(1);
  }
  close(go[1]);
  if (read(xp[0], &e, sizeof e) == (ssize_t)sizeof e) {
    /* Nothing ran. The instance stays, saying why and with what status. */
    log_write(cmdline, strlen(cmdline));
    n = snprintf(line, sizeof line, "\nbabysit: cannot execute: %s\n",
                 strerror(e));
    log_write(line, (size_t)n);
    record_exit(dir, e == ENOENT ? 127 : 126);
    send_msg(MSG_ERR_EXEC, e);
    leave(1);
  }
  close(xp[0]);

  emit(cmdline, strlen(cmdline));
  n = snprintf(line, sizeof line, "\nProcess created with pid %ld\n",
               (long)child);
  emit(line, (size_t)n);
  if (lock_err) {
    n = snprintf(line, sizeof line,
                 "babysit: warning: cannot lock this log (%s); babysitd "
                 "cannot tell that it is still being written\n",
                 strerror(lock_err));
    emit(line, (size_t)n);
  }
  ctl_open(dir);
  /* Only now may the parent print the identifier: sharing a terminal or
   * a file with our echo, it then lands after the header, not inside it. */
  send_msg(MSG_START, (int)child);
  if (detach)
    release(MSG_DETACHED, 0);

  out.fd = op[0];
  err.fd = ep[0];
  memcpy(out.buf, "OUT: ", PREFIX_LEN);
  memcpy(err.buf, "ERR: ", PREFIX_LEN);
  set_nonblock(out.fd);
  set_nonblock(err.fd);

  /* Keep logging until the command is gone and both pipes are closed
   * (grandchildren may hold them open after the command itself exits). */
  start = last_read = now_ms();
  if (timeout_ms > 0)
    timeout_at = start + timeout_ms;
  while (out.fd >= 0 || err.fd >= 0 || !reaped) {
    long long now = now_ms(), wait = -1, rem;
    int r, timeout;

    if (out.len || err.len) { /* e.g. a prompt with no newline */
      rem = last_read + PARTIAL_FLUSH_MS - now;
      if (rem <= 0) {
        flush_partial(&out);
        flush_partial(&err);
      } else {
        wait = rem;
      }
    }
    if (watching && idle_ms > 0) {
      rem = last_read + idle_ms - now;
      if (rem <= 0) {
        flush_partial(&out); /* show what it stalled on */
        flush_partial(&err);
        release(MSG_STALL, 0);
      } else if (wait < 0 || rem < wait) {
        wait = rem;
      }
    }
    if (watching && timeout_ms > 0) {
      rem = timeout_at - now;
      if (rem <= 0)
        release(MSG_TIMEOUT, 0);
      else if (wait < 0 || rem < wait)
        wait = rem;
    }

    p[0].fd = out.fd;
    p[1].fd = err.fd;
    p[2].fd = reaped ? -1 : chld_pipe[0];
    p[3].fd = watching ? status_fd : -1; /* errors out if parent dies */
    p[4].fd = ctl_fd;
    p[0].events = p[1].events = p[2].events = p[4].events = POLLIN;
    p[3].events = 0;
    p[0].revents = p[1].revents = p[2].revents = p[3].revents = 0;
    p[4].revents = 0;

    if (wait < 0)
      timeout = -1;
    else
      timeout = wait > INT_MAX ? INT_MAX : (int)wait;
    r = poll(p, 5, timeout);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (r == 0)
      continue;
    if (p[3].revents)
      release(MSG_DETACHED, 0); /* parent was killed: let go too */
    if (p[4].revents && !reaped)
      ctl_read(child);
    if (p[2].revents) {
      char junk[16];
      while (read(chld_pipe[0], junk, sizeof junk) > 0)
        ;
      if (waitpid(child, &status, WNOHANG) == child) {
        int code =
            WIFSIGNALED(status) ? 128 + WTERMSIG(status) : WEXITSTATUS(status);

        reaped = 1;
        ctl_close();
        drain(&out);
        drain(&err);
        if (WIFSIGNALED(status))
          n = snprintf(line, sizeof line, "Process killed by signal %d\n",
                       WTERMSIG(status));
        else
          n = snprintf(line, sizeof line, "Process exited with code %d\n",
                       WEXITSTATUS(status));
        emit(line, (size_t)n);
        /* The status first, then the lock: someone who finds the lock gone
         * must find the status there. From here on the pid may belong to
         * something else. */
        record_exit(dir, code);
        lock_byte(log_fd, LOCK_CHILD, F_UNLCK);
        if (WIFSIGNALED(status))
          release(MSG_SIGNAL, WTERMSIG(status));
        else
          release(MSG_EXIT, WEXITSTATUS(status));
        continue;
      }
    }
    if (p[0].revents && out.fd >= 0)
      read_stream(&out);
    if (p[1].revents && err.fd >= 0)
      read_stream(&err);
  }
  close(log_fd);
  leave(0);
}

/* ---------- parent ---------- */

/* Join argv into one line, shell-quoting anything that needs it. */
static char *join_cmdline(char **cmd) {
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

/* ---------- instances ---------- */

static void random_salt(char *salt) {
  unsigned char raw[SALT_LEN];
  static unsigned long state;
  int fd, i, got = 0;

  fd = open("/dev/urandom", O_RDONLY);
  if (fd >= 0) {
    got = read(fd, raw, sizeof raw) == (ssize_t)sizeof raw;
    close(fd);
  }
  if (!got) { /* uniqueness does not rest on this: mkdir() decides */
    if (!state)
      state = (unsigned long)time(NULL) + ((unsigned long)getpid() * 65537UL);
    for (i = 0; i < SALT_LEN; i++) {
      state = (state * 1103515245UL) + 12345UL;
      raw[i] = (unsigned char)((state / 65536UL) % 256UL);
    }
  }
  for (i = 0; i < SALT_LEN; i++)
    salt[i] = salt_chars[raw[i] % (sizeof salt_chars - 1)];
  salt[SALT_LEN] = '\0';
}

/*
 * Create a new instance under LOGDIR, in the directory of MINUTE
 * ("YYYYmmdd/HH/MM"), and return its identifier in ID. The
 * mkdir() of the last component is what makes it unique: if it is taken,
 * another salt is tried.
 */
static int new_instance(const char *logdir, const char *minute, char *id,
                        size_t idn) {
  char salt[SALT_LEN + 1], dir[PATH_BUF + ID_MAX + 8];
  int tries, n;

  n = snprintf(dir, sizeof dir, "%s/%s", logdir, minute);
  if (n < 0 || (size_t)n >= sizeof dir) {
    errno = ENAMETOOLONG;
    return -1;
  }
  if (mkdirs(dir) < 0)
    return -1;
  for (tries = 0; tries < 1000; tries++) {
    random_salt(salt);
    snprintf(dir, sizeof dir, "%s/%s/%s", logdir, minute, salt);
    if (mkdir(dir, 0777) == 0) {
      snprintf(id, idn, "%s/%s", minute, salt);
      return 0;
    }
    if (errno != EEXIST)
      return -1;
  }
  errno = EEXIST;
  return -1;
}

/* Remove an instance that never got as far as a log, and whatever of the
 * directories above it that leaves empty. */
static void drop_instance(const char *logdir, const char *id) {
  char path[PATH_BUF + ID_MAX + 8];
  int i;

  snprintf(path, sizeof path, "%s/%s", logdir, id);
  remove_tree(path);
  for (i = 0; i < 3; i++) {
    *strrchr(path, '/') = '\0';
    if (rmdir(path) < 0)
      break;
  }
}

/* The pid on the log's second line, "Process created with pid N"; -1 if
 * the log does not get that far. */
static long logged_pid(const char *path) {
  static const char intro[] = "Process created with pid ";
  char line[128], *end;
  FILE *f = fopen(path, "r");
  long pid = -1;
  int c;

  if (!f)
    return -1;
  do
    c = getc(f); /* the command line, however long it is */
  while (c != EOF && c != '\n');
  if (c != EOF && fgets(line, sizeof line, f) &&
      strncmp(line, intro, sizeof intro - 1) == 0) {
    pid = strtol(line + sizeof intro - 1, &end, 10);
    if (end == line + sizeof intro - 1 || *end != '\n' || pid <= 0)
      pid = -1;
  }
  fclose(f);
  return pid;
}

/* ---------- looking at an instance from outside ---------- */

enum { ST_RUNNING, ST_EXITED, ST_LOST };

struct instance {
  char dir[PATH_BUF + ID_MAX + 8];
  char log[PATH_BUF + ID_MAX + 16];
};

/* Work out the log directory from --log-dir, or its default. */
static int resolve_logdir(const char *dir, char *logdir, size_t size) {
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

/* Find instance ID under --log-dir DIR. Complains and returns -1 if ID is
 * not an identifier or there is no such instance. */
static int find_instance(const char *dir, const char *id, struct instance *in) {
  char logdir[PATH_BUF];
  struct stat sb;

  if (!id) {
    fprintf(stderr, "babysit: no instance identifier given\n");
    return -1;
  }
  if (!is_instance_id(id)) {
    fprintf(stderr,
            "babysit: '%s' is not an instance identifier "
            "(YYYYmmdd/HH/MM/xxxx)\n",
            id);
    return -1;
  }
  if (resolve_logdir(dir, logdir, sizeof logdir) < 0)
    return -1;
  snprintf(in->dir, sizeof in->dir, "%s/%s", logdir, id);
  snprintf(in->log, sizeof in->log, "%s/log", in->dir);
  if (stat(in->log, &sb) < 0) {
    fprintf(stderr,
            "babysit: no instance %s under %s (babysitd may have packed "
            "it)\n",
            id, logdir);
    return -1;
  }
  return 0;
}

/* The recorded exit status, or -1 if there is none (yet). */
static int recorded_exit(const struct instance *in) {
  char path[PATH_BUF + ID_MAX + 32], text[32], *end;
  long code;

  snprintf(path, sizeof path, "%s/exit_code", in->dir);
  if (read_first_line(path, text, sizeof text) < 0)
    return -1;
  code = strtol(text, &end, 10);
  return (end == text || *end || code < 0 || code > 255) ? -1 : (int)code;
}

/*
 * What has become of the command: it has exited (*code says how), it is
 * running, or neither can be shown, which means the babysitter died without
 * recording a status. The status is looked for first and again last: the
 * babysitter records it just before it lets go of the lock.
 */
static int instance_state(const struct instance *in, int *code) {
  *code = recorded_exit(in);
  if (*code >= 0)
    return ST_EXITED;
  if (child_is_running(in->log))
    return ST_RUNNING;
  *code = recorded_exit(in);
  return *code >= 0 ? ST_EXITED : ST_LOST;
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
static int parse_signal(const char *s) {
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

/* Ask the instance's babysitter to send SIG to the command. 0 if asked,
 * -1 if there is nobody to ask: the command is not running. */
static int request_signal(const struct instance *in, int sig) {
  char path[PATH_BUF + ID_MAX + 16], text[16];
  int fd, n, rc;

  if (!child_is_running(in->log))
    return -1;
  snprintf(path, sizeof path, "%s/ctl", in->dir);
  fd = open(path, O_WRONLY | O_NONBLOCK); /* fails if nobody listens */
  if (fd < 0)
    return -1;
  n = snprintf(text, sizeof text, "%d\n", sig);
  rc = write(fd, text, (size_t)n) == (ssize_t)n ? 0 : -1;
  close(fd);
  return rc;
}

/*
 * Options and operands of the commands that look at an existing instance:
 * every option may come before or after the operands. Returns the number
 * of operands stored in OPS, or -1 after complaining.
 */
struct look {
  const char *dir, *idle_s, *timeout_s, *onfail;
  int pid, log, code;
};

static int look_args(const char *name, int argc, char **argv, int watch,
                     int info, struct look *o, const char **ops, int max) {
  const char *v;
  int i, n = 0;

  memset(o, 0, sizeof *o);
  for (i = 1; i < argc; i++) {
    if (opt_match("babysit", argc, argv, &i, "-l", "--log-dir", &v)) {
      o->dir = v;
    } else if (watch &&
               opt_match("babysit", argc, argv, &i, "-e", "--expect", &v)) {
      expect_pat = v;
    } else if (watch &&
               opt_match("babysit", argc, argv, &i, "-i", "--idle", &v)) {
      o->idle_s = v;
    } else if (watch &&
               opt_match("babysit", argc, argv, &i, "-t", "--timeout", &v)) {
      o->timeout_s = v;
    } else if (watch && opt_match("babysit", argc, argv, &i, "--onfail",
                                  "--onfail", &v)) {
      o->onfail = v;
    } else if (watch &&
               opt_match("babysit", argc, argv, &i, "-F", "--fixed", NULL)) {
      fixed = 1;
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

/* babysit check ID: 0 if the command is running, 1 if it is not. */
static int cmd_check(int argc, char **argv) {
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
static int cmd_info(int argc, char **argv) {
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
static int cmd_signal(int argc, char **argv) {
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
 * babysit watch ID: follow the log of an instance that is already there and
 * return as "start" would have. Whoever started it has gone, and its
 * babysitter is no longer telling anyone anything, so this reads the log
 * file itself, from the beginning: a line that appeared before we got here
 * still counts for --expect. Lines that appear from now on are echoed on
 * stderr.
 */
#define WATCH_TICK_MS 100

static int watch_fail(const struct instance *in, const char *id,
                      const char *onfail, int sig, int status) {
  if (onfail) {
    if (request_signal(in, sig) == 0)
      fprintf(stderr, "babysit: %s: sent %s to the command\n", id, onfail);
    else
      fprintf(stderr, "babysit: %s: could not send %s: not running\n", id,
              onfail);
  }
  return status;
}

static int cmd_watch(int argc, char **argv) {
  static char buf[4 * (PREFIX_LEN + LINE_MAX_ + 1)];
  const char *ops[1] = {NULL}, *id;
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
  id = ops[0];
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
  if (expect_pat && !fixed) {
    int rc = regcomp(&expect_re, expect_pat, REG_EXTENDED | REG_NOSUB);
    if (rc != 0) {
      char msg[256];
      regerror(rc, &expect_re, msg, sizeof msg);
      fprintf(stderr, "babysit: bad --expect pattern: %s\n", msg);
      return EXIT_USAGE;
    }
  }
  if (find_instance(o.dir, id, &in) < 0)
    return EXIT_USAGE;
  fd = open(in.log, O_RDONLY);
  if (fd < 0 || fstat(fd, &sb) < 0) {
    fprintf(stderr, "babysit: cannot read %s: %s\n", in.log, strerror(errno));
    return EXIT_USAGE;
  }
  from = sb.st_size; /* what is there already is not echoed */

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
          ssize_t w = write(2, line, n);
          (void)w;
        }
        if (expect_pat && n > PREFIX_LEN &&
            (memcmp(line, "OUT: ", PREFIX_LEN) == 0 ||
             memcmp(line, "ERR: ", PREFIX_LEN) == 0)) {
          *nl = '\0';
          hit = fixed ? strstr(line + PREFIX_LEN, expect_pat) != NULL
                      : regexec(&expect_re, line + PREFIX_LEN, 0, NULL, 0) == 0;
        }
        if (hit) {
          fprintf(stderr, "babysit: %s: expected output seen\n", id);
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
      fprintf(stderr, "babysit: %s: exited with status %d%s\n", id, code,
              expect_pat ? " before the expected output" : "");
      return (expect_pat && code == 0) ? 1 : code;
    }
    if (state == ST_LOST) {
      fprintf(stderr,
              "babysit: %s: the babysitter is gone and left no exit "
              "status; see the log\n",
              id);
      return EXIT_USAGE;
    }
    now = now_ms();
    if (timeout_ms > 0 && now - start >= timeout_ms) {
      fprintf(stderr, "babysit: %s: timed out after %s; still running\n", id,
              o.timeout_s);
      return watch_fail(&in, id, o.onfail, sig, EXIT_TIMEOUT);
    }
    if (idle_ms > 0 && now - last >= idle_ms) {
      fprintf(stderr, "babysit: %s: stalled: no output for %s; still running\n",
              id, o.idle_s);
      return watch_fail(&in, id, o.onfail, sig, EXIT_STALLED);
    }
    nanosleep(&tick, NULL);
  }
}

/* The one setting babysit takes from babysit.conf: timezone. */
static int load_timezone(const char *path, int required, int *utc) {
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

/* babysit start [options] [--] command [args...] */
static int cmd_start(int argc, char **argv) {
  const char *idle_s = NULL, *timeout_s = NULL, *dir = NULL, *v;
  const char *config = NULL;
  char logdir[PATH_BUF], cfg[PATH_BUF], id[ID_MAX];
  char minute[ID_MAX], later[ID_MAX];
  char path[PATH_BUF + ID_MAX + 8];
  char *cmdline, **cmd;
  long long idle_ms = -1, timeout_ms = -1;
  int detach = 0, utc = 0, i, st, status_pipe[2];
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
      fixed = 1;
    } else if (opt_match("babysit", argc, argv, &i, "-d", "--detach", NULL)) {
      detach = 1;
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
  if (expect_pat && !fixed) {
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
  /* Records dated later than now: the clock has been set back. Filing new
   * ones among them would make the directory lie about what happened when,
   * so nothing is started until babysitd has packed them away. That is its
   * job, not ours; all we do is ask, and tell the caller to come back. */
  if (find_later(logdir, minute, later, sizeof later)) {
    request_pass(logdir);
    fprintf(stderr,
            "babysit: the clock has gone backwards: %s/%s is dated later "
            "than now (%s).\n"
            "babysit: nothing was started. Once babysitd has packed it "
            "(\"babysitd --once\"), try again.\n",
            logdir, later, minute);
    return EXIT_AGAIN;
  }
  cmdline = join_cmdline(cmd);
  if (!cmdline) {
    perror("babysit: malloc");
    return EXIT_USAGE;
  }
  if (new_instance(logdir, minute, id, sizeof id) < 0) {
    fprintf(stderr, "babysit: cannot create a log directory under %s: %s\n",
            logdir, strerror(errno));
    free(cmdline);
    return EXIT_USAGE;
  }
  snprintf(path, sizeof path, "%s/%s", logdir, id);

  if (pipe(status_pipe) < 0) {
    perror("babysit: pipe");
    free(cmdline);
    drop_instance(logdir, id);
    return EXIT_USAGE;
  }
  fflush(NULL);

  /* Double fork + setsid: the babysitter is reparented to init and has no
   * controlling terminal, so it outlives us and our caller. */
  mid = fork();
  if (mid < 0) {
    perror("babysit: fork");
    free(cmdline);
    drop_instance(logdir, id);
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
  if (expect_pat && !fixed)
    regfree(&expect_re);
  while (waitpid(mid, &st, 0) < 0 && errno == EINTR)
    ;

  if (read_msg(status_pipe[0], &m) < 0) {
    fprintf(stderr, "babysit: the babysitter failed to start\n");
    drop_instance(logdir, id);
    return EXIT_USAGE;
  }
  if (m.type == MSG_ERR_EXEC) {
    /* Nothing ran, but the instance is there to say so. */
    puts(id);
    fflush(stdout);
    fprintf(stderr, "babysit: %s: cannot execute '%s': %s\n", id, cmd[0],
            strerror(m.value));
    return m.value == ENOENT ? 127 : 126;
  }
  if (m.type != MSG_START) {
    fprintf(stderr, "babysit: cannot create %s/log: %s\n", path,
            strerror(m.value));
    drop_instance(logdir, id);
    return EXIT_USAGE;
  }
  pid = (pid_t)m.value;

  /* The command is running: from here on stdout says which instance it
   * is, and says nothing else. */
  puts(id);
  fflush(stdout);

  /* Now just wait to be told why to return. */
  if (read_msg(status_pipe[0], &m) < 0) {
    fprintf(stderr,
            "babysit: %s: lost contact with the babysitter; see the "
            "log\n",
            id);
    return EXIT_USAGE;
  }
  switch (m.type) {
  case MSG_DETACHED:
    return 0;
  case MSG_MATCH:
    fprintf(stderr,
            "babysit: %s: expected output seen; pid %ld continues in the "
            "background\n",
            id, (long)pid);
    return 0;
  case MSG_STALL:
    fprintf(stderr,
            "babysit: %s: stalled: no output for %s; pid %ld is still "
            "running\n",
            id, idle_s, (long)pid);
    return EXIT_STALLED;
  case MSG_TIMEOUT:
    fprintf(stderr,
            "babysit: %s: timed out after %s; pid %ld is still running\n", id,
            timeout_s, (long)pid);
    return EXIT_TIMEOUT;
  case MSG_SIGNAL:
    fprintf(stderr, "babysit: %s: killed by signal %d%s\n", id, m.value,
            expect_pat ? " before the expected output" : "");
    return 128 + m.value;
  case MSG_EXIT:
    fprintf(stderr, "babysit: %s: exited with code %d%s\n", id, m.value,
            expect_pat ? " before the expected output" : "");
    return (expect_pat && m.value == 0) ? 1 : m.value;
  default:
    break;
  }
  return EXIT_USAGE;
}

int main(int argc, char **argv) {
  const char *c = argc > 1 ? argv[1] : "";

  if (strcmp(c, "-h") == 0 || strcmp(c, "--help") == 0) {
    usage(stdout);
    return 0;
  }
  if (strcmp(c, "-V") == 0 || strcmp(c, "--version") == 0) {
    puts("babysit " BABYSIT_VERSION);
    return 0;
  }
  if (strcmp(c, "start") == 0)
    return cmd_start(argc - 1, argv + 1);
  if (strcmp(c, "watch") == 0)
    return cmd_watch(argc - 1, argv + 1);
  if (strcmp(c, "check") == 0)
    return cmd_check(argc - 1, argv + 1);
  if (strcmp(c, "info") == 0)
    return cmd_info(argc - 1, argv + 1);
  if (strcmp(c, "signal") == 0)
    return cmd_signal(argc - 1, argv + 1);
  if (*c)
    fprintf(stderr, "babysit: unknown command '%s'\n\n", c);
  else
    fprintf(stderr, "babysit: no command given\n\n");
  usage(stderr);
  return EXIT_USAGE;
}
