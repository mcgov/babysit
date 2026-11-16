/*
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Matthew G. McGovern
 *
 * This file is part of babysit, which is distributed under the MIT
 * license; see the file LICENSE for details.
 */
/*
 * babysit: what the babysitter does for others while the command runs and when
 * it ends: signals asked for through the control FIFO, and the exit_code file.
 */
#include "babysit_internal.h"

char ctl_path[PATH_BUF + ID_MAX + 16]; /* our control FIFO, if any */
int ctl_fd = -1, ctl_wfd = -1;

/*
 * The control FIFO, <instance>/ctl: "babysit signal" writes a signal number
 * and a newline to it, and we deliver the signal. We are the command's
 * parent, so until we have reaped it its pid cannot have passed to anyone
 * else, which nobody looking the pid up from outside can be sure of.
 * Holding a write end ourselves keeps the read end from reporting hang-up.
 */
void ctl_open(const char *dir) {
  snprintf(ctl_path, sizeof ctl_path, "%s/ctl", dir);
  if (mkfifo(ctl_path, 0600) < 0)
    return;
  ctl_fd = open(ctl_path, O_RDONLY | O_NONBLOCK);
  ctl_wfd = ctl_fd >= 0 ? open(ctl_path, O_WRONLY | O_NONBLOCK) : -1;
}

/* No more signals: the command is gone, or we are. */
void ctl_close(void) {
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
void ctl_read(pid_t child) {
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
void record_exit(const char *dir, int code) {
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
