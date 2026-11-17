/*
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Matthew G. McGovern
 *
 * This file is part of babysit, which is distributed under the MIT
 * license; see the file LICENSE for details.
 */
/*
 * faultinject.so: make one library call fail, for the tests.
 *
 * Loaded into babysit or babysitd with LD_PRELOAD, it stands in front of
 * the C library for a handful of functions and makes the Nth call of one
 * of them fail the way the real one can: malloc returns NULL with ENOMEM,
 * fork returns -1 with EAGAIN, and so on. Every other call goes through
 * untouched. The tests run a command once for each N until a run goes by
 * in which nothing was left to fail, which visits every place the program
 * calls that function, and every one of them must be handled.
 *
 * It is configured through the environment, since a preloaded library has
 * no command line:
 *
 *   BABYSIT_FAULT=FUNCTION:N   fail the Nth call of FUNCTION (N from 1)
 *   BABYSIT_FAULT_LOG=FILE     append a line to FILE when that happens
 *
 * FUNCTION is one of: malloc realloc fork pipe statvfs fsync mkfifo
 * opendir localtime gmtime.
 *
 * Only a program named babysit or babysitd is affected: the command that
 * babysit runs, tar, and the shell inherit the variables and must work as
 * usual. The count is kept per process and is carried over fork, so that
 * N counts along the line from the first process to the babysitter.
 *
 * Linux (glibc) and FreeBSD. ASan and TSan replace malloc and realloc
 * themselves, so in those builds these two cannot be made to fail; the
 * other functions can.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static const char *target; /* the function to fail, or NULL */
static long nth, seen;
static int ready;

static const char *progname(void) {
#ifdef __GLIBC__
  return program_invocation_short_name;
#else
  return getprogname();
#endif
}

static void setup(void) {
  static char name[32];
  const char *spec, *colon, *prog;

  prog = progname();
  if (!prog || !*prog)
    return; /* called before the C library knows: ask again next time */
  ready = 1;
  if (strcmp(prog, "babysit") != 0 && strcmp(prog, "babysitd") != 0)
    return;
  spec = getenv("BABYSIT_FAULT");
  colon = spec ? strchr(spec, ':') : NULL;
  if (!colon || (size_t)(colon - spec) >= sizeof name)
    return;
  memcpy(name, spec, (size_t)(colon - spec));
  nth = strtol(colon + 1, NULL, 10);
  if (nth > 0)
    target = name;
}

/* Is this call of FUNC the one to fail? Says so in the log if it is. */
static int strike(const char *func) {
  const char *path;
  char line[96];
  int fd, n, saved = errno;

  if (!ready)
    setup();
  if (!target || strcmp(target, func) != 0 || ++seen != nth)
    return 0;
  path = getenv("BABYSIT_FAULT_LOG");
  fd = path ? open(path, O_WRONLY | O_CREAT | O_APPEND, 0644) : -1;
  if (fd >= 0) {
    n = snprintf(line, sizeof line, "%s call %ld failed in pid %ld\n", func,
                 nth, (long)getpid());
    if (write(fd, line, (size_t)n) < 0)
      n = 0;
    close(fd);
  }
  errno = saved;
  return 1;
}

/* The C library's own function of this name. */
#define REAL(ret, params, name)                                                \
  static ret(*real) params;                                                    \
  if (!real)                                                                   \
  real = (ret(*) params)dlsym(RTLD_NEXT, name)

#ifdef __GLIBC__
/* dlsym may itself allocate; glibc exports its allocator under other names. */
extern void *__libc_malloc(size_t size);
extern void *__libc_realloc(void *ptr, size_t size);

void *malloc(size_t size) {
  if (strike("malloc")) {
    errno = ENOMEM;
    return NULL;
  }
  return __libc_malloc(size);
}

void *realloc(void *ptr, size_t size) {
  if (strike("realloc")) {
    errno = ENOMEM;
    return NULL;
  }
  return __libc_realloc(ptr, size);
}
#else
void *malloc(size_t size) {
  REAL(void *, (size_t), "malloc");
  if (strike("malloc")) {
    errno = ENOMEM;
    return NULL;
  }
  return real(size);
}

void *realloc(void *ptr, size_t size) {
  REAL(void *, (void *, size_t), "realloc");
  if (strike("realloc")) {
    errno = ENOMEM;
    return NULL;
  }
  return real(ptr, size);
}
#endif

pid_t fork(void) {
  REAL(pid_t, (void), "fork");
  if (strike("fork")) {
    errno = EAGAIN;
    return -1;
  }
  return real();
}

int pipe(int fds[2]) {
  REAL(int, (int *), "pipe");
  if (strike("pipe")) {
    errno = EMFILE;
    return -1;
  }
  return real(fds);
}

int statvfs(const char *path, struct statvfs *buf) {
  REAL(int, (const char *, struct statvfs *), "statvfs");
  if (strike("statvfs")) {
    errno = EIO;
    return -1;
  }
  return real(path, buf);
}

#ifdef __GLIBC__
/* What statvfs becomes when a program is built for 64-bit file offsets. */
int statvfs64(const char *path, struct statvfs64 *buf) {
  REAL(int, (const char *, struct statvfs64 *), "statvfs64");
  if (strike("statvfs")) {
    errno = EIO;
    return -1;
  }
  return real(path, buf);
}
#endif

int fsync(int fd) {
  REAL(int, (int), "fsync");
  if (strike("fsync")) {
    errno = EIO;
    return -1;
  }
  return real(fd);
}

int mkfifo(const char *path, mode_t mode) {
  REAL(int, (const char *, mode_t), "mkfifo");
  if (strike("mkfifo")) {
    errno = EACCES;
    return -1;
  }
  return real(path, mode);
}

DIR *opendir(const char *path) {
  REAL(DIR *, (const char *), "opendir");
  if (strike("opendir")) {
    errno = EMFILE;
    return NULL;
  }
  return real(path);
}

struct tm *localtime(const time_t *t) {
  REAL(struct tm *, (const time_t *), "localtime");
  if (strike("localtime")) {
    errno = EOVERFLOW;
    return NULL;
  }
  return real(t);
}

struct tm *gmtime(const time_t *t) {
  REAL(struct tm *, (const time_t *), "gmtime");
  if (strike("gmtime")) {
    errno = EOVERFLOW;
    return NULL;
  }
  return real(t);
}
