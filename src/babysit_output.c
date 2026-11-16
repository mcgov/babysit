/*
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Matthew G. McGovern
 *
 * This file is part of babysit, which is distributed under the MIT
 * license; see the file LICENSE for details.
 */
/*
 * babysit: the babysitter's output: the log, the echo on the caller's stderr,
 * the messages to the waiting parent, and the line buffering of the command's
 * two streams.
 */
#include "babysit_internal.h"

/* babysitter state */
int log_fd = -1, status_fd = -1;

int watching; /* still echoing to the caller and running timers */
long long last_read;

long long timeout_at = -1; /* when --timeout expires, in now_ms() */

int write_all(int fd, const void *buf, size_t n) {
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

void set_nonblock(int fd) {
  int fl = fcntl(fd, F_GETFL);
  if (fl >= 0)
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

void set_cloexec(int fd) {
  int fl = fcntl(fd, F_GETFD);
  if (fl >= 0)
    fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
}

void send_msg(int type, int value) {
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
void release(int type, int value) {
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
void echo(const char *buf, size_t n) {
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

void log_write(const char *buf, size_t n) {
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

void emit(const char *buf, size_t n) {
  log_write(buf, n);
  echo(buf, n);
}

static void flush_line(struct lstream *s) {
  char *data = s->buf + PREFIX_LEN;
  int hit = 0;

  if (watching && expect_pat) {
    data[s->len] = '\0';
    hit = expect_fixed ? strstr(data, expect_pat) != NULL
                       : regexec(&expect_re, data, 0, NULL, 0) == 0;
  }
  data[s->len] = '\n';
  emit(s->buf, PREFIX_LEN + s->len + 1);
  s->len = 0;
  if (hit)
    release(MSG_MATCH, 0);
}

void flush_partial(struct lstream *s) {
  if (s->len)
    flush_line(s);
}

/* Returns 1 if data was read, 0 on EOF (stream closed), -1 if nothing yet. */
int read_stream(struct lstream *s) {
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

void drain(struct lstream *s) {
  while (s->fd >= 0 && read_stream(s) == 1)
    ;
  flush_partial(s);
}
