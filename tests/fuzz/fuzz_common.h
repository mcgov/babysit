/*
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Matthew G. McGovern
 *
 * This file is part of babysit, which is distributed under the MIT
 * license; see the file LICENSE for details.
 */
/*
 * What the libFuzzer targets in this directory share. Include it after
 * the babysit source or header the target is about: those set the
 * feature-test macros.
 *
 * Every target is built with AddressSanitizer and with -Dexit=fuzz_exit:
 * babysit answers a missing option argument by exiting with a usage
 * message, and an exit is an answer here, not a crash. It becomes a jump
 * back to the target; see FUZZ_GUARD.
 *
 * The targets are run with -close_fd_mask=3, which closes stdout and
 * stderr to the code under test, and that code is talkative. A target's
 * own reason for giving up goes to a copy of stderr made before that.
 */
#ifndef FUZZ_COMMON_H
#define FUZZ_COMMON_H

#include <setjmp.h>
#include <stdint.h>

int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static int fuzz_report_fd = 2;
static char fuzz_root[PATH_BUF]; /* a scratch directory of the target's own */

static inline void die(const char *what) {
  char line[512];
  int n = snprintf(line, sizeof line, "fuzz: %s (%s)\n", what, strerror(errno));

  if (n > 0 && write(fuzz_report_fd, line, (size_t)n) < 0)
    n = 0;
  abort();
}

/* What exit() is in these builds: back to where FUZZ_GUARD was. */
static jmp_buf fuzz_back;
void exit(int code) {
  (void)code;
  longjmp(fuzz_back, 1);
}
/* At the top of LLVMFuzzerTestOneInput: an exit() ends this input. */
#define FUZZ_GUARD()                                                           \
  do {                                                                         \
    fuzz_release();                                                            \
    if (setjmp(fuzz_back) != 0)                                                \
      return 0;                                                                \
  } while (0)

/* Call first in LLVMFuzzerInitialize: the scratch directory, under
 * $TMPDIR, and the way to the terminal. */
static inline void fuzz_init(void) {
  const char *tmp = getenv("TMPDIR");

  fuzz_report_fd = dup(2);
  if (fuzz_report_fd < 0)
    fuzz_report_fd = 2;
  snprintf(fuzz_root, sizeof fuzz_root, "%s/babysit-fuzz-XXXXXX",
           tmp ? tmp : "/tmp");
  if (!mkdtemp(fuzz_root))
    die("cannot make a scratch directory");
  signal(SIGPIPE, SIG_IGN);
}

/* Text handed to the code under test. It is freed when the next input
 * arrives, since an exit() may jump past whoever would have freed it. */
static char *fuzz_held[8];
static int fuzz_nheld;

static inline void fuzz_release(void) {
  while (fuzz_nheld > 0)
    free(fuzz_held[--fuzz_nheld]);
}

static inline char *text_of(const uint8_t *data, size_t size) {
  char *s = malloc(size + 1);

  if (!s)
    die("out of memory");
  if (size)
    memcpy(s, data, size);
  s[size] = '\0';
  if (fuzz_nheld == (int)(sizeof fuzz_held / sizeof fuzz_held[0]))
    die("too much text held");
  fuzz_held[fuzz_nheld++] = s;
  return s;
}

/* Split TEXT at newlines into at most MAX-1 arguments after NAME. */
static inline int split_args(char *text, char *name, char **argv, int max) {
  int n = 0;
  char *p = text;

  argv[n++] = name;
  while (n < max - 1 && *p) {
    argv[n++] = p;
    p = strchr(p, '\n');
    if (!p)
      break;
    *p++ = '\0';
  }
  argv[n] = NULL;
  return n;
}

/* Returns -1 if the file could not be written: the disk may be full. */
static inline int put_file(const char *path, const uint8_t *data, size_t size) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  int ok;

  if (fd < 0)
    return -1;
  ok = size == 0 || write(fd, data, size) == (ssize_t)size;
  return close(fd) == 0 && ok ? 0 : -1;
}

/*
 * babysit's commands are run here as functions, one after another, in a
 * process that goes on. In babysit each is the last thing its process
 * does, and some leave a file open when they return; here that would use
 * up the descriptors. Whatever was left open is closed: FROM and up.
 */
static inline int lowest_free_fd(void) {
  int fd = dup(0);

  if (fd < 0)
    die("out of file descriptors");
  close(fd);
  return fd;
}

static inline void close_from(int from) {
  int fd;

  for (fd = from; fd < from + 8; fd++)
    close(fd);
}

#endif
