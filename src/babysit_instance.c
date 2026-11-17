/*
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Matthew G. McGovern
 *
 * This file is part of babysit, which is distributed under the MIT
 * license; see the file LICENSE for details.
 */
/*
 * babysit: instances on disk: creating one, removing one, and finding out from
 * outside what has become of one.
 */
#include "babysit_internal.h"

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
int new_instance(const char *logdir, const char *minute, char *child_id,
                 size_t size) {
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
      snprintf(child_id, size, "%s/%s", minute, salt);
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
void drop_instance(const char *logdir, const char *child_id) {
  char path[PATH_BUF + ID_MAX + 8];
  int i;

  snprintf(path, sizeof path, "%s/%s", logdir, child_id);
  remove_tree(path);
  for (i = 0; i < 3; i++) {
    *strrchr(path, '/') = '\0';
    if (rmdir(path) < 0)
      break;
  }
}

/* The pid on the log's second line, "Process created with pid N"; -1 if
 * the log does not get that far. */
long logged_pid(const char *path) {
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

/* Find instance ID under --log-dir DIR. Complains and returns -1 if ID is
 * not an identifier or there is no such instance. */
int find_instance(const char *dir, const char *child_id, struct instance *in) {
  char logdir[PATH_BUF];
  struct stat sb;

  if (!child_id) {
    fprintf(stderr, "babysit: no instance identifier given\n");
    return -1;
  }
  if (!is_instance_id(child_id)) {
    fprintf(stderr,
            "babysit: '%s' is not an instance identifier "
            "(YYYYmmdd/HH/MM/xxxx)\n",
            child_id);
    return -1;
  }
  if (resolve_logdir(dir, logdir, sizeof logdir) < 0)
    return -1;
  snprintf(in->dir, sizeof in->dir, "%s/%s", logdir, child_id);
  snprintf(in->log, sizeof in->log, "%s/log", in->dir);
  if (stat(in->log, &sb) < 0) {
    fprintf(stderr,
            "babysit: no instance %s under %s (babysitd may have packed "
            "it)\n",
            child_id, logdir);
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
int instance_state(const struct instance *in, int *code) {
  *code = recorded_exit(in);
  if (*code >= 0)
    return ST_EXITED;
  if (child_is_running(in->log))
    return ST_RUNNING;
  *code = recorded_exit(in);
  return *code >= 0 ? ST_EXITED : ST_LOST;
}

/* Ask the instance's babysitter to send SIG to the command. 0 if asked,
 * -1 if there is nobody to ask: the command is not running. */
int request_signal(const struct instance *in, int sig) {
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
