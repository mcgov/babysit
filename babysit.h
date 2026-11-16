/*
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Matthew G. McGovern
 *
 * This file is part of babysit, which is distributed under the MIT
 * license; see the file LICENSE for details.
 */
/*
 * Shared by babysit and babysitd: the log directory layout and the
 * "does this log still have a writer" test.
 *
 *   <log-dir>/<boot timestamp>/<generation>/<babysit pid>/<command pid>/log
 *   <log-dir>/<boot timestamp>/<generation>.tar.gz             (babysitd)
 *   <log-dir>/archive/babysit_logs_<boot timestamp>.tar.gz     (babysitd)
 *
 * Pids are only unique within one boot, so every boot gets its own
 * directory, named after the time the system booted.
 *
 * Within a boot, logs go to the highest-numbered generation, starting at 0.
 * When a pid comes round again and its place in that generation is taken,
 * babysit starts the next generation instead of touching the old log. A
 * generation that is no longer the highest gets no new logs, so babysitd
 * packs it into <generation>.tar.gz once its last writer has finished.
 *
 * A babysitter holds an fcntl() write lock on its log for as long as it is
 * logging. The kernel drops the lock when the babysitter exits, however it
 * exits, so "is the log locked" is a reliable "is it still being written".
 *
 * POSIX.1-2008 + XSI, C99; sysctl() for the boot time on the BSDs and macOS.
 */
#ifndef BABYSIT_H
#define BABYSIT_H

/*
 * Feature-test macros. The BSDs and macOS expose POSIX plus their own
 * sysctl() by default, and asking for strict POSIX would hide sysctl();
 * everywhere else POSIX.1-2008 + XSI is requested explicitly.
 */
#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) ||     \
    defined(__DragonFly__)
#define BABYSIT_HAVE_SYSCTL 1
#elif defined(__APPLE__)
#define BABYSIT_HAVE_SYSCTL 1
#ifndef _DARWIN_C_SOURCE
/* NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) */
#define _DARWIN_C_SOURCE 1
#endif
#elif !defined(_XOPEN_SOURCE)
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
#ifdef BABYSIT_HAVE_SYSCTL
#include <sys/sysctl.h>
#endif

#define BABYSIT_VERSION "0.1.0"

#define EXIT_USAGE 2

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

/* Wake babysitd, if it is running. Never blocks; failure is harmless. */
static inline void request_packing(const char *logdir) {
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

#define BOOT_ID_FILE "/proc/sys/kernel/random/boot_id"
#define BOOT_STAMP_MAX 32
#define UNKNOWN_BOOT "unknown-boot"

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

/* Seconds since the epoch at which the system booted, or 0 if unknown. */
static inline time_t boot_time(void) {
#ifdef BABYSIT_HAVE_SYSCTL
  int mib[2] = {CTL_KERN, KERN_BOOTTIME};
  struct timeval tv;
  size_t len = sizeof tv;

  if (sysctl(mib, 2, &tv, &len, NULL, 0) == 0)
    return tv.tv_sec;
  return 0;
#else
  char line[256];
  FILE *f = fopen("/proc/stat", "r");
  long long t = 0;

  if (!f)
    return 0;
  while (fgets(line, sizeof line, f)) {
    if (strncmp(line, "btime ", 6) == 0) {
      t = strtoll(line + 6, NULL, 10);
      break;
    }
  }
  fclose(f);
  return t > 0 ? (time_t)t : 0;
#endif
}

/*
 * Something that identifies the current boot: the kernel's random boot id
 * on Linux, otherwise the boot time in seconds. Returns -1 if neither is
 * available.
 */
static inline int boot_identity(char *id, size_t n) {
  time_t bt;

  if (read_first_line(BOOT_ID_FILE, id, n) == 0 && id[0])
    return 0;
  bt = boot_time();
  if (bt == 0)
    return -1;
  snprintf(id, n, "%lld", (long long)bt);
  return 0;
}

/*
 * Do two boot identities name the same boot? A boot time can move a little
 * when the clock is corrected, so two of those match within two minutes.
 */
static inline int same_boot(const char *a, const char *b) {
  long long x, y;

  if (is_pid_name(a) && is_pid_name(b)) {
    x = strtoll(a, NULL, 10);
    y = strtoll(b, NULL, 10);
    return (x > y ? x - y : y - x) <= 120;
  }
  return strcmp(a, b) == 0;
}

/*
 * Name of the current boot's directory, as YYYYmmdd-HHMMSS.
 *
 * The kernel's idea of the boot time can shift when the clock is adjusted,
 * so the name is worked out once per boot and remembered in <log-dir>/.boot
 * next to the boot's identity; later callers reuse it as long as the
 * identity still matches. Where the system offers no way to tell boots
 * apart, all logs go under "unknown-boot" and are never archived.
 */
static inline void current_boot(const char *logdir, char *stamp, size_t n) {
  char id[128], old_id[128], old_stamp[BOOT_STAMP_MAX];
  char marker[PATH_BUF + 16], tmp[PATH_BUF + 48], line[256];
  struct tm *tm;
  time_t bt;
  FILE *f;

  if (boot_identity(id, sizeof id) < 0) {
    snprintf(stamp, n, "%s", UNKNOWN_BOOT);
    return;
  }
  snprintf(marker, sizeof marker, "%s/.boot", logdir);
  if (read_first_line(marker, line, sizeof line) == 0 &&
      sscanf(line, "%127s %31s", old_id, old_stamp) == 2 &&
      same_boot(old_id, id)) {
    snprintf(stamp, n, "%s", old_stamp);
    return;
  }
  bt = boot_time();
  if (bt == 0)
    bt = time(NULL);
  tm = localtime(&bt);
  if (!tm || strftime(stamp, n, "%Y%m%d-%H%M%S", tm) == 0) {
    snprintf(stamp, n, "%s", UNKNOWN_BOOT);
    return;
  }
  if (mkdirs(logdir) < 0)
    return;
  snprintf(tmp, sizeof tmp, "%s/.boot.%ld", logdir, (long)getpid());
  f = fopen(tmp, "w");
  if (!f)
    return;
  fprintf(f, "%s %s\n", id, stamp);
  if (fclose(f) != 0 || rename(tmp, marker) < 0)
    unlink(tmp);
}

/*
 * Is NAME the name babysit gives a boot directory: YYYYmmdd-HHMMSS, or
 * "unknown-boot"? babysitd touches nothing else in the log directory.
 */
static inline int is_boot_name(const char *name) {
  int i;

  if (strcmp(name, UNKNOWN_BOOT) == 0)
    return 1;
  for (i = 0; i < 15; i++) {
    if (i == 8 ? name[i] != '-' : (name[i] < '0' || name[i] > '9'))
      return 0;
  }
  return name[15] == '\0';
}

/* Is NAME "<number>.tar.gz", a packed generation? */
static inline int is_packed_gen_name(const char *name) {
  const char *p = name;

  while (*p >= '0' && *p <= '9')
    p++;
  return p != name && strcmp(p, ".tar.gz") == 0;
}

/* Is NAME "babysit_logs_<something>.tar.gz", the archive of a boot? */
static inline int is_archive_name(const char *name) {
  size_t len = strlen(name);

  return len > 20 && strncmp(name, "babysit_logs_", 13) == 0 &&
         strcmp(name + len - 7, ".tar.gz") == 0;
}

/* Take the writer lock on an open log. */
static inline int lock_log(int fd) {
  struct flock fl;

  memset(&fl, 0, sizeof fl);
  fl.l_type = F_WRLCK;
  fl.l_whence = SEEK_SET;
  return fcntl(fd, F_SETLK, &fl);
}

/* 1 if another process holds the writer lock on this log, else 0. */
static inline int log_has_writer(const char *path) {
  struct flock fl;
  int fd, busy = 0;

  fd = open(path, O_RDWR);
  if (fd < 0)
    fd = open(path, O_RDONLY);
  if (fd < 0)
    return 0;
  memset(&fl, 0, sizeof fl);
  fl.l_type = F_WRLCK;
  fl.l_whence = SEEK_SET;
  if (fcntl(fd, F_GETLK, &fl) == 0 && fl.l_type != F_UNLCK)
    busy = 1;
  close(fd);
  return busy;
}

/* 1 if any <dir>/<pid>/log (dir = <log-dir>/<babysit pid>) has a writer. */
static inline int ppid_dir_has_writer(const char *dir) {
  char path[PATH_BUF];
  struct dirent *de;
  DIR *d = opendir(dir);
  int busy = 0, n;

  if (!d)
    return 0;
  while (!busy && (de = readdir(d)) != NULL) {
    if (!is_pid_name(de->d_name))
      continue;
    n = snprintf(path, sizeof path, "%s/%s/log", dir, de->d_name);
    if (n > 0 && (size_t)n < sizeof path)
      busy = log_has_writer(path);
  }
  closedir(d);
  return busy;
}

/* 1 if any log under <boot>/<generation> has a writer. */
static inline int gen_dir_has_writer(const char *gendir) {
  char path[PATH_BUF + 600];
  struct dirent *de;
  DIR *d = opendir(gendir);
  int busy = 0, n;

  if (!d)
    return 0;
  while (!busy && (de = readdir(d)) != NULL) {
    if (!is_pid_name(de->d_name))
      continue;
    n = snprintf(path, sizeof path, "%s/%s", gendir, de->d_name);
    if (n > 0 && (size_t)n < sizeof path)
      busy = ppid_dir_has_writer(path);
  }
  closedir(d);
  return busy;
}

/*
 * Highest generation number present under <log-dir>/<boot>, counting both
 * live directories ("3") and packed ones ("3.tar.gz"); -1 if there is none.
 */
static inline long current_gen(const char *bootdir) {
  struct dirent *de;
  DIR *d = opendir(bootdir);
  long best = -1, g;
  char *end;

  if (!d)
    return -1;
  while ((de = readdir(d)) != NULL) {
    if (de->d_name[0] < '0' || de->d_name[0] > '9')
      continue;
    g = strtol(de->d_name, &end, 10);
    if ((*end == '\0' || strcmp(end, ".tar.gz") == 0) && g > best)
      best = g;
  }
  closedir(d);
  return best;
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
