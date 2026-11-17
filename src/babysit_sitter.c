/*
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Matthew G. McGovern
 *
 * This file is part of babysit, which is distributed under the MIT
 * license; see the file LICENSE for details.
 */
/*
 * babysit: the babysitter process itself: it starts the command, owns its
 * output and runs the timers until the command and its pipes are gone.
 */
#include "babysit_internal.h"

/* The SIGCHLD handler writes here; the main loop polls the other end. */
static int chld_pipe[2] = {-1, -1};

/* ---------- babysitter ---------- */

static void on_sigchld(int sig) {
  int saved = errno;
  ssize_t w = write(chld_pipe[1], "", 1);
  (void)w;
  (void)sig;
  errno = saved;
}

/* The babysitter's way out. */
static void leave(int code) {
  out_flush();
  ctl_close();
  COVERAGE_FLUSH();
  _exit(code);
}

void babysitter(const char *dir, char **cmd, const char *cmdline, int detach,
                long long idle_ms, long long timeout_ms) {
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

  /* Keep nothing of the caller's: whoever reads its stdout or stderr sees
   * end-of-file as soon as the parent has gone, and nothing the caller
   * does with them can reach us or the command. */
  devnull = open("/dev/null", O_RDWR);
  if (devnull >= 0) {
    dup2(devnull, 0);
    dup2(devnull, 1);
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
  /* The log has its header by the time the parent prints the identifier. */
  out_flush();
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

    out_flush(); /* nothing stays in the buffer while we sleep */
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
        out_flush(); /* the log is complete before the status appears */
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
