/*
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Matthew G. McGovern
 *
 * This file is part of babysit, which is distributed under the MIT
 * license; see the file LICENSE for details.
 */
/*
 * babysit: the babysitter's output: the log, the messages to the waiting
 * parent, and the line buffering of the command's two streams.
 *
 * Nothing is copied to the caller. The babysitter holds none of the
 * caller's descriptors, so a caller that reads slowly, or not at all, can
 * neither hold the command back nor keep a timer from firing. Whoever
 * wants to see the output reads the log: "babysit cat", "babysit watch",
 * or tail -f.
 */
#include "babysit_internal.h"

/* babysitter state */
int log_fd = -1, status_fd = -1;

int watching; /* the parent is still waiting, and the timers are running */
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
 * Write to the log. The disk can fill up, or a file size limit be reached,
 * while the command is running; that is no reason to stop it, but it must
 * not pass unnoticed either. The parent is told once, if it is still
 * waiting, and what the log missed is counted. Each later write tries
 * again, and as soon as one gets through the log first gets a line saying
 * how much is missing at that point.
 */
static unsigned long long lost; /* bytes the log did not take */
static int lost_err, lost_told;

static void log_now(const char *buf, size_t n) {
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
    send_msg(MSG_WARN_LOG, lost_err);
  }
}

/*
 * Output is collected here and written out in one go: the log gets one
 * write() for everything that one read of the command's pipe produced,
 * where a write per line would cost a system call a line and hold a busy
 * command back many times over. Only whole lines are put in, so whoever
 * reads the log never sees half of one.
 *
 * Nothing waits in here: the main loop empties it before it sleeps, and so
 * does everything that tells the parent something or ends the process.
 */
static char pending[1 << 17];
static size_t npending;

void out_flush(void) {
  size_t n = npending;

  npending = 0;
  if (n > 0)
    log_now(pending, n);
}

/* To the log, by way of the buffer. */
void emit(const char *buf, size_t n) {
  if (n > sizeof pending - npending)
    out_flush();
  if (n > sizeof pending) { /* a command line of rare length */
    log_now(buf, n);
    return;
  }
  memcpy(pending + npending, buf, n);
  npending += n;
}

/* To the log at once, after whatever is waiting. */
void log_write(const char *buf, size_t n) {
  out_flush();
  log_now(buf, n);
}

/*
 * Stop watching: tell the parent why, so that it can return. The log is
 * brought up to date first: what the parent is told about is in it by the
 * time the parent has gone. Logging carries on regardless.
 */
void release(int type, int value) {
  if (!watching)
    return;
  out_flush();
  watching = 0;
  send_msg(type, value);
  if (status_fd >= 0) {
    close(status_fd);
    status_fd = -1;
  }
}

static void flush_line(struct lstream *s) {
  char *data = s->buf + PREFIX_LEN;
  int hit = 0, seen = expect_seen;

  if (watching && expect_n) {
    data[s->len] = '\0';
    hit = expect_line(data);
  }
  data[s->len] = '\n';
  emit(s->buf, PREFIX_LEN + s->len + 1);
  s->len = 0;
  if (hit)
    release(MSG_MATCH, 0);
  else if (expect_seen != seen)
    send_msg(MSG_SEEN, expect_seen); /* the parent reports it if we fail */
}

void flush_partial(struct lstream *s) {
  if (s->len)
    flush_line(s);
}

/* Returns 1 if data was read, 0 on EOF (stream closed), -1 if nothing yet. */
int read_stream(struct lstream *s) {
  static char tmp[1 << 16]; /* as much as a pipe holds */
  ssize_t r = read(s->fd, tmp, sizeof tmp);
  const char *p = tmp, *end = tmp + (r > 0 ? r : 0);

  if (r > 0) {
    last_read = now_ms(); /* any byte counts as progress */
    while (p < end) {
      /* Up to the next newline, in pieces no longer than a line may be. */
      const char *nl = memchr(p, '\n', (size_t)(end - p));
      size_t seg = (size_t)((nl ? nl : end) - p);

      while (seg > 0) {
        size_t room = LINE_MAX_ - s->len, take = seg < room ? seg : room;

        memcpy(s->buf + PREFIX_LEN + s->len, p, take);
        s->len += take;
        p += take;
        seg -= take;
        if (s->len == LINE_MAX_)
          flush_line(s);
      }
      if (nl) {
        flush_line(s);
        p = nl + 1;
      }
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
