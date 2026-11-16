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
 *   parent      the babysit you invoked. It only waits for the babysitter to
 *               say why it should return, and turns that into an exit code.
 *   babysitter  detached (own session). Owns the command's stdout/stderr
 *               pipes, writes the log, echoes it to the caller's stdout, and
 *               runs the timers. When it decides the parent should return it
 *               closes its copy of the caller's stdout and keeps logging.
 *   child       the command. Its stdio never touches the caller's, so the
 *               parent and babysitter can let go without disturbing it.
 *
 * Log: <log-dir>/<boot timestamp>/<generation>/<parent pid>/<child pid>/log
 *   (so "ls <log-dir>/<boot>/<gen>/<parent pid>" resolves a babysit pid to
 *   the pid of the command it started; pids are only unique within one boot)
 *
 * Pid reuse: logs go to the highest-numbered generation of the current
 * boot. If either pid of a new log is already filed there, the pid range
 * has wrapped around; the old log is left alone and a new generation is
 * started, and babysitd is woken through <log-dir>/.request (if it is
 * running). It packs the older generation once nothing writes to it
 * (the babysitter holds a write lock on its log for as long as it logs).
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

static void usage(FILE *f) {
  fprintf(
      f,
      "Usage: babysit [options] [--] command [args...]\n"
      "\n"
      "Run COMMAND detached from this terminal/pipe, echo its output, and log "
      "it\n"
      "to LOGDIR/<boot time>/<gen>/<babysit pid>/<child pid>/log, so the pid "
      "of\n"
      "this babysit is enough to find the command's pid and log later: see\n"
      "--get-child-pid and --get-child-log (the path is also printed as the\n"
      "first line of output). Each output line is\n"
      "prefixed with 'OUT: ' (stdout) or 'ERR: ' (stderr); the log starts "
      "with\n"
      "the command line and 'Process created with pid N' and ends with\n"
      "'Process exited with code N'.\n"
      "\n"
      "By default babysit waits for the command and returns its exit status.\n"
      "The options below make it return early. When it does, the command is "
      "NOT\n"
      "stopped: it keeps running and its output keeps going to the log.\n"
      "\n"
      "Options:\n"
      "  -e, --expect REGEX   return 0 once an output line matches the POSIX\n"
      "                       extended REGEX (matched without the OUT:/ERR: "
      "prefix)\n"
      "  -F, --fixed          treat the --expect pattern as a plain substring\n"
      "  -i, --idle SECS      return 125 if the command writes nothing for "
      "SECS\n"
      "  -t, --timeout SECS   return 124 after SECS in total\n"
      "  -d, --detach         return 0 as soon as the command has started\n"
      "  -l, --log-dir DIR    base log directory\n"
      "                       (default: $HOME/.local/var/log/babysit)\n"
      "      --get-child-pid PID\n"
      "                       run nothing: print the pid of the command "
      "that the\n"
      "                       babysit with pid PID started during this boot\n"
      "      --get-child-log PID\n"
      "                       run nothing: print the path of that command's "
      "log\n"
      "  -V, --version        show the version\n"
      "  -h, --help           show this help\n"
      "\n"
      "SECS may be fractional and may carry an s, m or h suffix (0.5, 30s, "
      "2m).\n"
      "The command gets /dev/null as stdin and its own process group, so\n"
      "'kill -- -PID' stops it together with its children.\n"
      "\n"
      "Exit status:\n"
      "  N        the command exited with status N (128+signal if killed)\n"
      "  0        --expect matched, or --detach\n"
      "  1        --expect was given but the command exited 0 without a match\n"
      "  124      --timeout expired; command still running\n"
      "  125      stalled for --idle seconds; command still running\n"
      "  126/127  the command could not be executed / was not found\n"
      "  2        usage or internal error; nothing was started, unless "
      "stderr\n"
      "           says that contact with the babysitter was lost\n"
      "With --get-child-pid or --get-child-log: 0 if found, 1 if PID has no "
      "log.\n");
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
 * Stop watching: close our copy of the caller's stdout, then tell the
 * parent why so it can return. Logging carries on regardless.
 */
static void release(int type, int value) {
  int devnull;

  if (!watching)
    return;
  watching = 0;
  devnull = open("/dev/null", O_WRONLY);
  if (devnull >= 0) {
    dup2(devnull, 1);
    close(devnull);
  } else {
    close(1);
  }
  send_msg(type, value);
  if (status_fd >= 0) {
    close(status_fd);
    status_fd = -1;
  }
}

/*
 * Copy output to the caller's stdout. The caller may read slowly or not at
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
    p[0].fd = 1;
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
    w = write(1, buf, n > PIPE_BUF ? PIPE_BUF : n);
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
 * not pass unnoticed either. The caller is told once, on its stdout, and
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
 * Would a log for (parent, child) land on pids already filed in this
 * generation? True if this babysit's pid has a directory there, or the
 * child's pid appears under any babysit pid, or the generation has
 * already been packed.
 */
static int gen_collides(const char *gendir, long parent, long child) {
  char path[PATH_BUF + 64];
  struct dirent *de;
  struct stat sb;
  DIR *d;
  int hit = 0, n;

  n = snprintf(path, sizeof path, "%s.tar.gz", gendir);
  if (n > 0 && (size_t)n < sizeof path && stat(path, &sb) == 0)
    return 1; /* already packed: its number is spent */
  n = snprintf(path, sizeof path, "%s/%ld", gendir, parent);
  if (n > 0 && (size_t)n < sizeof path && stat(path, &sb) == 0)
    return 1;
  d = opendir(gendir);
  if (!d)
    return 0;
  while (!hit && (de = readdir(d)) != NULL) {
    if (!is_pid_name(de->d_name))
      continue;
    n = snprintf(path, sizeof path, "%s/%s/%ld", gendir, de->d_name, child);
    hit = n > 0 && (size_t)n < sizeof path && stat(path, &sb) == 0;
  }
  closedir(d);
  return hit;
}

/* The babysitter's way out. */
static void leave(int code) {
  COVERAGE_FLUSH();
  _exit(code);
}

static void babysitter(const char *logdir, const char *base, long parent_pid,
                       char **cmd, const char *cmdline, int detach,
                       long long idle_ms, long long timeout_ms) {
  static struct lstream out, err;
  struct sigaction sa;
  struct pollfd p[4];
  char path[PATH_BUF], line[256];
  int op[2], ep[2], go[2], xp[2];
  int devnull, e, n, status = 0, reaped = 0, lock_err = 0;
  long long start;
  long gen;
  int bumped = 0;
  pid_t child;

  signal(SIGPIPE, SIG_IGN);
  signal(SIGXFSZ, SIG_IGN); /* a log at its size limit: EFBIG, not death */
  watching = 1;

  /* Keep only the caller's stdout (for echoing); drop stdin and stderr. */
  devnull = open("/dev/null", O_RDWR);
  if (devnull >= 0) {
    dup2(devnull, 0);
    dup2(devnull, 2);
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

  /* The log path contains the child's pid, so it is created only now. */
  /* Log into the newest generation. If our pids are already filed
   * there, they have been reused: leave that log alone and open the next
   * generation, which also tells babysitd to pack the older one. */
  gen = current_gen(base);
  if (gen < 0)
    gen = 0;
  for (;;) {
    n = snprintf(path, sizeof path, "%s/%ld", base, gen);
    if (n < 0 || (size_t)n >= sizeof path - 64 ||
        !gen_collides(path, parent_pid, (long)child))
      break;
    gen++;
    bumped = 1;
  }
  n = snprintf(path, sizeof path, "%s/%ld/%ld/%ld", base, gen, parent_pid,
               (long)child);
  if (n < 0 || (size_t)n >= sizeof path - 4) {
    errno = ENAMETOOLONG;
  } else if (mkdirs(path) == 0) {
    memcpy(path + n, "/log", 5); /* room was checked just above */
    log_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
  }
  if (log_fd >= 0) {
    /* The lock marks the log as having an active writer. */
    if (lock_log(log_fd) < 0)
      lock_err = errno;
    if (bumped)
      request_packing(logdir); /* don't wait for its next check */
  }
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
    /* Nothing ran: leave no log, and no directories either, which a
     * later babysit would take for pids already used in this generation. */
    unlink(path);
    for (n = 0; n < 2; n++) {
      *strrchr(path, '/') = '\0';
      rmdir(path);
    }
    send_msg(MSG_ERR_EXEC, e);
    leave(1);
  }
  close(xp[0]);

  send_msg(MSG_START, (int)child);
  echo("Log file: ", 10);
  echo(path, strlen(path));
  echo("\n", 1);
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
    p[0].events = p[1].events = p[2].events = POLLIN;
    p[3].events = 0;
    p[0].revents = p[1].revents = p[2].revents = p[3].revents = 0;

    if (wait < 0)
      timeout = -1;
    else
      timeout = wait > INT_MAX ? INT_MAX : (int)wait;
    r = poll(p, 4, timeout);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (r == 0)
      continue;
    if (p[3].revents)
      release(MSG_DETACHED, 0); /* parent was killed: let go too */
    if (p[2].revents) {
      char junk[16];
      while (read(chld_pipe[0], junk, sizeof junk) > 0)
        ;
      if (waitpid(child, &status, WNOHANG) == child) {
        reaped = 1;
        drain(&out);
        drain(&err);
        if (WIFSIGNALED(status)) {
          n = snprintf(line, sizeof line, "Process killed by signal %d\n",
                       WTERMSIG(status));
          emit(line, (size_t)n);
          release(MSG_SIGNAL, WTERMSIG(status));
        } else {
          n = snprintf(line, sizeof line, "Process exited with code %d\n",
                       WEXITSTATUS(status));
          emit(line, (size_t)n);
          release(MSG_EXIT, WEXITSTATUS(status));
        }
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

/*
 * --get-child-pid / --get-child-log: find what the babysit with pid PPID
 * filed during this boot and print the command's pid or the path of its
 * log. Generations are searched newest first, so if that pid has come
 * round again the most recent run wins. Packed generations and
 * earlier boots are not searched.
 */
static int lookup(const char *base, const char *ppid, int want_log) {
  char path[PATH_BUF + 512];
  struct dirent *de;
  struct stat sb;
  long gen;
  DIR *d;
  int n;

  if (!is_pid_name(ppid)) {
    fprintf(stderr, "babysit: '%s' is not a pid\n", ppid);
    return EXIT_USAGE;
  }
  for (gen = current_gen(base); gen >= 0; gen--) {
    n = snprintf(path, sizeof path, "%s/%ld/%s", base, gen, ppid);
    if (n < 0 || (size_t)n >= sizeof path)
      continue;
    d = opendir(path);
    if (!d)
      continue;
    while ((de = readdir(d)) != NULL) {
      if (!is_pid_name(de->d_name))
        continue;
      n = snprintf(path, sizeof path, "%s/%ld/%s/%s/log", base, gen, ppid,
                   de->d_name);
      if (n < 0 || (size_t)n >= sizeof path || stat(path, &sb) < 0)
        continue;
      puts(want_log ? path : de->d_name);
      closedir(d);
      return 0;
    }
    closedir(d);
  }
  fprintf(stderr,
          "babysit: no log for babysit pid %s under %s (nothing was "
          "started, or babysitd has packed it)\n",
          ppid, base);
  return 1;
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

int main(int argc, char **argv) {
  const char *idle_s = NULL, *timeout_s = NULL, *dir = NULL, *v;
  const char *get_pid = NULL, *get_log = NULL;
  char logdir[PATH_BUF], cwd[PATH_BUF], boot[BOOT_STAMP_MAX];
  char base[PATH_BUF + 64];
  char *cmdline, **cmd;
  long long idle_ms = -1, timeout_ms = -1;
  int detach = 0, i, n, st, status_pipe[2];
  struct msg m;
  pid_t pid, mid;
  long self;

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
    } else if (opt_match("babysit", argc, argv, &i, "--get-child-pid",
                         "--get-child-pid", &v)) {
      get_pid = v;
    } else if (opt_match("babysit", argc, argv, &i, "--get-child-log",
                         "--get-child-log", &v)) {
      get_log = v;
    } else if (opt_match("babysit", argc, argv, &i, "-F", "--fixed", NULL)) {
      fixed = 1;
    } else if (opt_match("babysit", argc, argv, &i, "-d", "--detach", NULL)) {
      detach = 1;
    } else if (opt_match("babysit", argc, argv, &i, "-V", "--version", NULL)) {
      puts("babysit " BABYSIT_VERSION);
      return 0;
    } else if (opt_match("babysit", argc, argv, &i, "-h", "--help", NULL)) {
      usage(stdout);
      return 0;
    } else {
      fprintf(stderr, "babysit: unknown option '%s'\n\n", argv[i]);
      usage(stderr);
      return EXIT_USAGE;
    }
  }
  cmd = argv + i;
  if ((get_pid || get_log) && *cmd) {
    fprintf(stderr, "babysit: --get-child-pid and --get-child-log take no "
                    "command\n");
    return EXIT_USAGE;
  }
  if (!get_pid && !get_log && !*cmd) {
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

  if (!dir) {
    if (default_log_dir(logdir, sizeof logdir) < 0) {
      fprintf(stderr, "babysit: HOME is not set; use --log-dir\n");
      return EXIT_USAGE;
    }
  } else {
    if (dir[0] == '/') {
      n = snprintf(logdir, sizeof logdir, "%s", dir);
    } else {
      if (!getcwd(cwd, sizeof cwd)) {
        perror("babysit: getcwd");
        return EXIT_USAGE;
      }
      n = snprintf(logdir, sizeof logdir, "%s/%s", cwd, dir);
    }
    if (n < 0 || (size_t)n >= sizeof logdir - 64) {
      fprintf(stderr, "babysit: log path too long\n");
      return EXIT_USAGE;
    }
  }

  current_boot(logdir, boot, sizeof boot);
  snprintf(base, sizeof base, "%s/%s", logdir, boot);
  if (get_pid || get_log) {
    int rc = get_pid ? lookup(base, get_pid, 0) : 0;

    if (rc == 0 && get_log)
      rc = lookup(base, get_log, 1);
    return rc;
  }
  cmdline = join_cmdline(cmd);
  if (!cmdline) {
    perror("babysit: malloc");
    return EXIT_USAGE;
  }

  if (pipe(status_pipe) < 0) {
    perror("babysit: pipe");
    free(cmdline);
    return EXIT_USAGE;
  }
  fflush(NULL);

  /* Double fork + setsid: the babysitter is reparented to init and has no
   * controlling terminal, so it outlives us and our caller. */
  self = (long)getpid(); /* not getppid() later: we may be gone by then */
  mid = fork();
  if (mid < 0) {
    perror("babysit: fork");
    free(cmdline);
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
    babysitter(logdir, base, self, cmd, cmdline, detach, idle_ms, timeout_ms);
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
    return EXIT_USAGE;
  }
  if (m.type == MSG_ERR_EXEC) {
    fprintf(stderr, "babysit: cannot execute '%s': %s\n", cmd[0],
            strerror(m.value));
    return m.value == ENOENT ? 127 : 126;
  }
  if (m.type != MSG_START) {
    fprintf(stderr, "babysit: cannot create log under %s: %s\n", base,
            strerror(m.value));
    return EXIT_USAGE;
  }
  pid = (pid_t)m.value;

  /* Now just wait to be told why to return. */
  if (read_msg(status_pipe[0], &m) < 0) {
    fprintf(stderr, "babysit: lost contact with the babysitter; see the "
                    "log file\n");
    return EXIT_USAGE;
  }
  switch (m.type) {
  case MSG_DETACHED:
    return 0;
  case MSG_MATCH:
    fprintf(stderr,
            "babysit: expected output seen; pid %ld continues in "
            "the background\n",
            (long)pid);
    return 0;
  case MSG_STALL:
    fprintf(stderr,
            "babysit: stalled: no output for %s; pid %ld is "
            "still running\n",
            idle_s, (long)pid);
    return EXIT_STALLED;
  case MSG_TIMEOUT:
    fprintf(stderr,
            "babysit: timed out after %s; pid %ld is still "
            "running\n",
            timeout_s, (long)pid);
    return EXIT_TIMEOUT;
  case MSG_SIGNAL:
    fprintf(stderr, "babysit: pid %ld was killed by signal %d%s\n", (long)pid,
            m.value, expect_pat ? " before the expected output" : "");
    return 128 + m.value;
  case MSG_EXIT:
    fprintf(stderr, "babysit: pid %ld exited with code %d%s\n", (long)pid,
            m.value, expect_pat ? " before the expected output" : "");
    return (expect_pat && m.value == 0) ? 1 : m.value;
  default:
    break;
  }
  return EXIT_USAGE;
}
