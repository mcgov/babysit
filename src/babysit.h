/*
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Matthew G. McGovern
 *
 * This file is part of babysit, which is distributed under the MIT
 * license; see the file LICENSE for details.
 */
/*
 * Shared by babysit and babysitd: the log directory layout, the
 * configuration file, and the "does this log still have a writer" test.
 *
 *   <log-dir>/<YYYYmmdd>/<HH>/<MM>/<salt>/log
 *   <log-dir>/<YYYYmmdd>/<HH>/<MM>/<salt>/exit_code   once it has exited
 *   <log-dir>/<YYYYmmdd>/<HH>/<MM>/<salt>/ctl         while it is running
 *   <log-dir>/<YYYYmmdd>/<HH>.tar.gz                           (babysitd)
 *   <log-dir>/archive/babysit_logs_<YYYYmmdd>.tar.gz           (babysitd)
 *
 * Every run of babysit is an instance with an identifier of its own,
 * "<YYYYmmdd>/<HH>/<MM>/<salt>": the minute it started in and four random
 * characters. The identifier is the path of the instance's directory under
 * the log directory, so it can be read, and used, as it stands. It is made
 * unique by creating that directory with mkdir(), which fails if another
 * instance got there first; the next salt is then tried.
 *
 * Nothing in the layout is a pid. A pid says which process something is
 * only while that process is alive, so the pid of the command is given out
 * (babysit info --pid) only for as long as it is, and signals for it are
 * handed to its babysitter through the ctl FIFO (babysit signal).
 *
 * A babysitter holds two fcntl() write locks on its log: on byte 0 for as
 * long as it is logging, and on byte 1 for as long as the command has not
 * been reaped. The kernel drops them when the babysitter exits, however it
 * exits, so "is byte 0 locked" is a reliable "is it still being written"
 * and "is byte 1 locked" a reliable "is that pid still the command".
 *
 * POSIX.1-2008 + XSI, C99.
 */
#ifndef BABYSIT_H
#define BABYSIT_H

/*
 * Feature-test macros. The BSDs and macOS expose everything used here by
 * default; elsewhere POSIX.1-2008 + XSI is requested explicitly.
 */
#ifdef __APPLE__
#ifndef _DARWIN_C_SOURCE
/* NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) */
#define _DARWIN_C_SOURCE 1
#endif
#elif !defined(__FreeBSD__) && !defined(__NetBSD__) &&                         \
    !defined(__OpenBSD__) && !defined(__DragonFly__) &&                        \
    !defined(_XOPEN_SOURCE)
/* NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) */
#define _XOPEN_SOURCE 700
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BABYSIT_VERSION "0.1.0"

#define EXIT_USAGE 2
#define EXIT_AGAIN 75 /* EX_TEMPFAIL: nothing started; try again later */

/*
 * Processes that leave through _exit() skip the atexit hook that writes
 * LLVM coverage data, so in a coverage build they write it themselves.
 */
#ifdef BABYSIT_COVERAGE
/* NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) */
int __llvm_profile_write_file(void);
#define COVERAGE_FLUSH() ((void)__llvm_profile_write_file())
#else
#define COVERAGE_FLUSH() ((void)0)
#endif
#define PATH_BUF 4096

static inline int is_pid_name(const char *s) {
  if (!*s)
    return 0;
  for (; *s; s++)
    if (*s < '0' || *s > '9')
      return 0;
  return 1;
}

/* $HOME/.local/var/log/babysit */
static inline int default_log_dir(char *buf, size_t n) {
  const char *home = getenv("HOME");
  int len;

  if (!home || home[0] != '/')
    return -1;
  len = snprintf(buf, n, "%s/.local/var/log/babysit", home);
  return (len < 0 || (size_t)len >= n) ? -1 : 0;
}

static inline int mkdirs(const char *path) {
  char tmp[PATH_BUF];
  char *p;

  size_t len = strlen(path);

  if (len >= sizeof tmp) {
    errno = ENAMETOOLONG;
    return -1;
  }
  memcpy(tmp, path, len + 1);
  for (p = tmp + 1; *p; p++) {
    if (*p != '/')
      continue;
    *p = '\0';
    if (mkdir(tmp, 0777) < 0 && errno != EEXIST)
      return -1;
    *p = '/';
  }
  if (mkdir(tmp, 0777) < 0 && errno != EEXIST)
    return -1;
  return 0;
}

/* FIFO in the log directory on which babysitd listens for early requests. */
#define REQUEST_FIFO ".request"

/* Ask babysitd for a pass now, if it is running. Never blocks; failure is
 * harmless. */
static inline void request_pass(const char *logdir) {
  char path[PATH_BUF + 32];
  int fd, n;
  ssize_t w;

  n = snprintf(path, sizeof path, "%s/%s", logdir, REQUEST_FIFO);
  if (n < 0 || (size_t)n >= sizeof path)
    return;
  fd = open(path, O_WRONLY | O_NONBLOCK); /* fails if nobody listens */
  if (fd < 0)
    return;
  w = write(fd, "", 1);
  (void)w;
  close(fd);
}

static inline int read_first_line(const char *path, char *buf, size_t n) {
  FILE *f = fopen(path, "r");
  size_t len;

  if (!f)
    return -1;
  if (!fgets(buf, (int)n, f)) {
    fclose(f);
    return -1;
  }
  fclose(f);
  len = strlen(buf);
  while (len && (buf[len - 1] == '\n' || buf[len - 1] == ' '))
    buf[--len] = '\0';
  return 0;
}

/* ---------- configuration shared by both programs ---------- */

/* $XDG_CONFIG_HOME/babysit/babysit.conf, or under $HOME/.config. */
static inline int default_config_path(char *buf, size_t n) {
  const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
  int len = -1;

  if (xdg && xdg[0] == '/')
    len = snprintf(buf, n, "%s/babysit/babysit.conf", xdg);
  else if (home && home[0] == '/')
    len = snprintf(buf, n, "%s/.config/babysit/babysit.conf", home);
  return (len < 0 || (size_t)len >= n) ? -1 : 0;
}

/* "local" or "utc" (any case), trailing blanks allowed. */
static inline int parse_timezone(const char *val, int *utc) {
  char word[8];
  size_t i;

  for (i = 0; i < sizeof word - 1 && val[i] && val[i] != ' ' &&
              val[i] != '\t' && val[i] != '\r' && val[i] != '\n';
       i++)
    word[i] = (char)(val[i] >= 'A' && val[i] <= 'Z' ? val[i] + 32 : val[i]);
  word[i] = '\0';
  while (val[i] == ' ' || val[i] == '\t' || val[i] == '\r' || val[i] == '\n')
    i++;
  if (val[i])
    return -1;
  if (strcmp(word, "utc") == 0)
    *utc = 1;
  else if (strcmp(word, "local") == 0)
    *utc = 0;
  else
    return -1;
  return 0;
}

/*
 * Split one line of the configuration file into its key and value, in
 * place. Returns 0 for a blank line or comment, 1 for "key = value", -1
 * for anything else.
 */
static inline int config_split(char *line, char **key, char **val) {
  char *end;

  while (*line == ' ' || *line == '\t')
    line++;
  if (*line == '#' || *line == '\n' || *line == '\r' || *line == '\0')
    return 0;
  *val = strchr(line, '=');
  if (!*val)
    return -1;
  end = *val;
  while (end > line && (end[-1] == ' ' || end[-1] == '\t'))
    end--;
  *end = '\0';
  (*val)++;
  while (**val == ' ' || **val == '\t')
    (*val)++;
  *key = line;
  return 1;
}

/* ---------- instance identifiers ---------- */

#define SALT_LEN 4
#define ID_MAX 32 /* "YYYYmmdd/HH/MM/ssss" and room to spare */

/* The characters a salt is made of: no 0/o or 1/l to mistake for another. */
static const char salt_chars[] = "abcdefghijkmnpqrstuvwxyz23456789";

/* Does S start with exactly N digits, followed by the character AFTER? */
static inline int digits_then(const char *s, int n, char after) {
  int i;

  for (i = 0; i < n; i++)
    if (s[i] < '0' || s[i] > '9')
      return 0;
  return s[n] == after;
}

static inline int is_day_name(const char *s) { return digits_then(s, 8, '\0'); }
static inline int is_hour_name(const char *s) {
  return digits_then(s, 2, '\0');
}

static inline int is_salt_name(const char *s) {
  int i;

  for (i = 0; i < SALT_LEN; i++)
    if (!s[i] || !strchr(salt_chars, s[i]))
      return 0;
  return s[SALT_LEN] == '\0';
}

/* Is ID "<YYYYmmdd>/<HH>/<MM>/<salt>" and nothing else? */
static inline int is_instance_id(const char *id) {
  return digits_then(id, 8, '/') && digits_then(id + 9, 2, '/') &&
         digits_then(id + 12, 2, '/') && is_salt_name(id + 15);
}

/* Is NAME "babysit_logs_<something>.tar.gz", the archive of a day? */
static inline int is_archive_name(const char *name) {
  size_t len = strlen(name);

  return len > 20 && strncmp(name, "babysit_logs_", 13) == 0 &&
         strcmp(name + len - 7, ".tar.gz") == 0;
}

/* Is NAME "<HH>.tar.gz" or "<HH>.<n>.tar.gz", a packed hour? */
static inline int is_packed_hour_name(const char *name) {
  const char *p = name + 2;

  if (name[0] < '0' || name[0] > '9' || name[1] < '0' || name[1] > '9')
    return 0;
  if (*p == '.' && p[1] >= '0' && p[1] <= '9') {
    p++;
    while (*p >= '0' && *p <= '9')
      p++;
  }
  return strcmp(p, ".tar.gz") == 0;
}

/* "<YYYYmmdd>/<HH>/<MM>" for time T, in UTC or local time. */
static inline int minute_path(time_t t, int utc, char *buf, size_t n) {
  struct tm *tm = utc ? gmtime(&t) : localtime(&t);

  return (tm && strftime(buf, n, "%Y%m%d/%H/%M", tm) > 0) ? 0 : -1;
}

/* ---------- who is still writing ---------- */

static inline int lock_byte(int fd, off_t byte, short type) {
  struct flock fl;

  memset(&fl, 0, sizeof fl);
  fl.l_type = type;
  fl.l_whence = SEEK_SET;
  fl.l_start = byte;
  fl.l_len = 1;
  return fcntl(fd, F_SETLK, &fl);
}

#define LOCK_WRITER 0 /* held while the babysitter is logging */
#define LOCK_CHILD 1  /* held until the command has been reaped */

/* Did the last open()/opendir() fail because there is nothing there? */
static inline int is_absent(int err) { return err == ENOENT || err == ENOTDIR; }

/*
 * Is BYTE of the log at PATH locked by another process? 1 yes, 0 no, -1 if
 * that cannot be found out (and *absent says whether the log is simply not
 * there).
 */
static inline int byte_locked(const char *path, off_t byte, int *absent) {
  struct flock fl;
  int fd, rc;

  *absent = 0;
  fd = open(path, O_RDWR);
  if (fd < 0)
    fd = open(path, O_RDONLY);
  if (fd < 0) {
    *absent = is_absent(errno);
    return -1;
  }
  memset(&fl, 0, sizeof fl);
  fl.l_type = F_WRLCK;
  fl.l_whence = SEEK_SET;
  fl.l_start = byte;
  fl.l_len = 1;
  if (fcntl(fd, F_GETLK, &fl) < 0)
    rc = -1;
  else
    rc = fl.l_type != F_UNLCK;
  close(fd);
  return rc;
}

/*
 * 1 if another process holds the writer lock on this log, else 0.
 *
 * The callers pack and delete what has no writer, so every doubt counts as
 * a writer: a log that cannot be opened, or a filesystem that cannot say
 * who holds a lock, is left alone. Only a log that is not there has none.
 */
static inline int log_has_writer(const char *path) {
  int absent, rc = byte_locked(path, LOCK_WRITER, &absent);

  return rc < 0 ? !absent : rc;
}

/*
 * 1 if the command logged at PATH has certainly not been reaped yet. Here
 * doubt counts the other way: a pid is only worth giving out if it is sure
 * to be the command's.
 */
static inline int child_is_running(const char *path) {
  int absent;

  return byte_locked(path, LOCK_CHILD, &absent) == 1;
}

/* 1 if any file called "log" anywhere under DIR has a writer. */
static int tree_busy;
static inline int tree_writer_cb(const char *path, const struct stat *sb,
                                 int flag, struct FTW *ftw) {
  (void)sb;
  if (flag == FTW_DNR || flag == FTW_NS) {
    tree_busy = 1; /* cannot look inside: assume the worst */
    return 1;
  }
  if (flag == FTW_F && strcmp(path + ftw->base, "log") == 0 &&
      log_has_writer(path)) {
    tree_busy = 1;
    return 1;
  }
  return 0;
}

static inline int tree_has_writer(const char *dir) {
  struct stat sb;

  if (stat(dir, &sb) < 0)
    return !is_absent(errno);
  tree_busy = 0;
  if (nftw(dir, tree_writer_cb, 16, FTW_PHYS) < 0)
    return 1;
  return tree_busy;
}

/* ---------- records dated later than now ---------- */

/*
 * 1 if anything under DIR is finished: a log nobody is writing, or a
 * tarball. That is what babysitd can pack away. An instance that is still
 * being written does not count, and neither does one babysitd could not
 * touch because its writer cannot be determined.
 */
static int tree_done;
static inline int tree_finished_cb(const char *path, const struct stat *sb,
                                   int flag, struct FTW *ftw) {
  size_t len = strlen(path);

  (void)sb;
  if (flag != FTW_F)
    return 0;
  if ((strcmp(path + ftw->base, "log") == 0 && !log_has_writer(path)) ||
      (len > 7 && strcmp(path + len - 7, ".tar.gz") == 0)) {
    tree_done = 1;
    return 1;
  }
  return 0;
}

static inline int tree_has_finished(const char *dir) {
  tree_done = 0;
  nftw(dir, tree_finished_cb, 16, FTW_PHYS);
  return tree_done;
}

/*
 * Is there a directory in DIR whose name IS_NAME accepts, which sorts
 * after REF, and which holds something finished? Its name is copied to
 * FOUND.
 */
static inline int later_entry(const char *dir, int (*is_name)(const char *),
                              const char *ref, char *found, size_t n) {
  char path[PATH_BUF + 300];
  struct dirent *de;
  struct stat sb;
  DIR *d = opendir(dir);
  int hit = 0;

  if (!d)
    return 0;
  while (!hit && (de = readdir(d)) != NULL) {
    if (!is_name(de->d_name) || strcmp(de->d_name, ref) <= 0)
      continue;
    snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
    if (stat(path, &sb) == 0 && S_ISDIR(sb.st_mode) &&
        tree_has_finished(path)) {
      snprintf(found, n, "%s", de->d_name);
      hit = 1;
    }
  }
  closedir(d);
  return hit;
}

/*
 * Does LOGDIR hold records dated later than MINUTE ("YYYYmmdd/HH/MM")? If
 * so the clock has been set back since they were filed (or the timezone
 * setting changed), and new records would be filed in among older ones that
 * claim to be newer. FOUND gets the first such directory, relative to
 * LOGDIR.
 *
 * Only finished records count. babysitd packs those away, after which a
 * retry gets through; an instance that is still being written cannot be
 * packed by anyone, and refusing on its account would only mean refusing
 * for as long as its command cares to run.
 */
static inline int find_later(const char *logdir, const char *minute,
                             char *found, size_t n) {
  char day[9], hour[3], dir[PATH_BUF + 32], name[16];

  memcpy(day, minute, 8);
  day[8] = '\0';
  memcpy(hour, minute + 9, 2);
  hour[2] = '\0';
  if (later_entry(logdir, is_day_name, day, name, sizeof name)) {
    snprintf(found, n, "%s", name);
    return 1;
  }
  snprintf(dir, sizeof dir, "%s/%s", logdir, day);
  if (later_entry(dir, is_hour_name, hour, name, sizeof name)) {
    snprintf(found, n, "%s/%s", day, name);
    return 1;
  }
  snprintf(dir, sizeof dir, "%s/%s/%s", logdir, day, hour);
  if (later_entry(dir, is_hour_name, minute + 12, name, sizeof name)) {
    snprintf(found, n, "%s/%s/%s", day, hour, name);
    return 1;
  }
  return 0;
}

static inline int remove_tree_cb(const char *path, const struct stat *sb,
                                 int flag, struct FTW *ftw) {
  (void)sb;
  (void)flag;
  (void)ftw;
  return remove(path);
}

static inline int remove_tree(const char *path) {
  return nftw(path, remove_tree_cb, 16, FTW_DEPTH | FTW_PHYS);
}

/*
 * Hand-rolled option matching (getopt_long is not POSIX).
 * Flag option (val == NULL): matches "-x" or "--long".
 * Value option: matches "-x V", "--long V" or "--long=V"; advances *i.
 */
static inline int opt_match(const char *prog, int argc, char **argv, int *i,
                            const char *sname, const char *lname,
                            const char **val) {
  const char *a = argv[*i];
  size_t ll = strlen(lname);

  if (strcmp(a, sname) == 0 || strcmp(a, lname) == 0) {
    if (!val)
      return 1;
    if (*i + 1 >= argc) {
      fprintf(stderr, "%s: option %s requires a value\n", prog, a);
      exit(EXIT_USAGE);
    }
    *val = argv[++*i];
    return 1;
  }
  if (val && strncmp(a, lname, ll) == 0 && a[ll] == '=') {
    *val = a + ll + 1;
    return 1;
  }
  return 0;
}

#endif
