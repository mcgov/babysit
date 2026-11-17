/*
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Matthew G. McGovern
 *
 * This file is part of babysit, which is distributed under the MIT
 * license; see the file LICENSE for details.
 */
/*
 * libFuzzer target: the babysitter's output path, from the command's two
 * pipes to the log.
 *
 * The input is a little program. Its first byte is the length (0 to 8) of
 * the text that follows, which becomes a fixed --expect pattern. Then
 * come operations, one byte each (taken modulo 8), with their arguments:
 *
 *   0  N, then N bytes: they arrive on the command's stdout
 *   1  the same on stderr
 *   2  the unfinished lines are written out, as after a second of silence
 *   3  the pending output is written to the log
 *   4  the log stops taking writes (it becomes /dev/full)
 *   5  the log takes writes again
 *   6  B, N (two bytes): N mod 20000 copies of byte B arrive on stdout
 *   7  the same on stderr
 *
 * At the end both pipes are closed and drained. Then the log is checked:
 * every line is labelled OUT: or ERR:, none is longer than a line may be,
 * and it holds exactly as many of each byte value on each stream as
 * arrived. If the log refused writes along the way, it may hold fewer,
 * never more, and then babysit's note of what is missing is allowed.
 *
 * Filling the disk. With BABYSIT_FUZZ_FILL=FILE in the environment what
 * each input put in the log is logged again, to a log that is never
 * emptied, so that the
 * filesystem under $TMPDIR fills up for real; tests/fuzz/run.sh puts a
 * small RAM disk there. After FULL_RUNS inputs on the full disk the log
 * is cut back, and the first thing then written to it must be the note
 * of how much is missing. FILE is where the target says how often that
 * happened. While the disk is nearly full the program above is not run:
 * its own scratch log needs room.
 */
#include "../../src/babysit_internal.h"

#include "fuzz_common.h"
#include <sys/statvfs.h>

void usage(FILE *f) { (void)f; }

#define FULL_RUNS 2000           /* inputs to throw at a full disk */
#define ROOM_NEEDED (512 * 1024) /* for the scratch log */

static int scratch_fd = -1, full_fd = -1, fill_fd = -1;
static const char *fill_stats;
static long full_runs, times_full, times_recovered;

static struct lstream streams[2];
static int feed_fd[2];
static uint64_t want[2][256];
static int refused;             /* the log refused writes during this input */
static size_t arrived;          /* bytes so far in this input */
#define ARRIVE_MAX (128 * 1024) /* keeps the log within check_log's buffer */

int LLVMFuzzerInitialize(int *argc, char ***argv) {
  char path[PATH_BUF + 64];

  (void)argc;
  (void)argv;
  fuzz_init();
  snprintf(path, sizeof path, "%s/log", fuzz_root);
  scratch_fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_APPEND, 0600);
  if (scratch_fd < 0)
    die("cannot make a scratch log");
  full_fd = open("/dev/full", O_WRONLY); /* -1 where there is none */
  fill_stats = getenv("BABYSIT_FUZZ_FILL");
  if (fill_stats && *fill_stats) {
    snprintf(path, sizeof path, "%s/fill-log", fuzz_root);
    fill_fd = open(path, O_RDWR | O_CREAT | O_APPEND, 0600);
    if (fill_fd < 0)
      die("cannot make the log that fills the disk");
  }
  return 0;
}

static void open_stream(int s) {
  int p[2];

  if (pipe(p) < 0)
    die("cannot make a pipe");
  set_nonblock(p[0]);
  streams[s].fd = p[0];
  streams[s].len = 0;
  memcpy(streams[s].buf, s ? "ERR: " : "OUT: ", PREFIX_LEN);
  feed_fd[s] = p[1];
}

static void arrive(int s, const uint8_t *buf, size_t n) {
  size_t i;

  if (n == 0 || arrived + n > ARRIVE_MAX)
    return;
  arrived += n;
  for (i = 0; i < n; i++)
    want[s][buf[i]]++;
  if (write_all(feed_fd[s], buf, n) < 0)
    die("cannot fill a pipe");
  (void)read_stream(&streams[s]);
}

/* The scratch log as it was last read back. */
static char got[1 << 20];
static size_t ngot;

static void check_log(void) {
  static uint64_t have[2][256];
  ssize_t n = pread(scratch_fd, got, sizeof got, 0);
  char *p, *end, *nl;
  int s, c;

  if (n < 0)
    die("cannot read the log back");
  ngot = (size_t)n;
  memset(have, 0, sizeof have);
  end = got + n;
  for (p = got; p < end; p = nl + 1) {
    size_t len, i;

    nl = memchr(p, '\n', (size_t)(end - p));
    if (!nl)
      die("the log does not end with a newline");
    len = (size_t)(nl - p);
    if (len >= PREFIX_LEN && (memcmp(p, "OUT: ", PREFIX_LEN) == 0 ||
                              memcmp(p, "ERR: ", PREFIX_LEN) == 0)) {
      if (len > PREFIX_LEN + LINE_MAX_)
        die("a line of the log is longer than a line may be");
      s = p[0] == 'E';
      for (i = PREFIX_LEN; i < len; i++)
        have[s][(unsigned char)p[i]]++;
    } else if (!refused || len < 9 || memcmp(p, "babysit: ", 9) != 0) {
      die("a line of the log has no label");
    }
  }
  for (s = 0; s < 2; s++)
    for (c = 0; c < 256; c++) {
      if (c == '\n')
        continue;
      if (have[s][c] > want[s][c])
        die("the log holds output the command did not write");
      if (have[s][c] < want[s][c] && !refused)
        die("output is missing from the log");
    }
}

static void fill_report(void) {
  FILE *f = fopen(fill_stats, "w");

  if (!f)
    return;
  fprintf(f, "the disk filled up %ld times\n", times_full);
  fprintf(f, "the log recovered, with its note of what was lost, %ld times\n",
          times_recovered);
  fclose(f);
}

/* Log DATA, as stdout, to the log that is never emptied. */
static void fill_disk(const uint8_t *data, size_t size) {
  struct stat before, after;
  char start[64];
  size_t done, n;

  if (size == 0)
    return;
  if (fstat(fill_fd, &before) < 0)
    die("cannot look at the log that fills the disk");
  open_stream(0);
  log_fd = fill_fd;
  status_fd = -1;
  watching = 0;
  for (done = 0; done < size; done += n) {
    n = size - done > 60000 ? 60000 : size - done; /* what a pipe takes */
    if (write_all(feed_fd[0], data + done, n) < 0)
      die("cannot fill a pipe");
    (void)read_stream(&streams[0]);
  }
  close(feed_fd[0]);
  drain(&streams[0]);
  out_flush();
  log_fd = -1;
  if (fstat(fill_fd, &after) < 0)
    die("cannot look at the log that fills the disk");

  if (after.st_size - before.st_size >= (off_t)size)
    return;
  /* It did not all fit: the disk is full, and babysit is counting what
   * the log has missed. */
  if (full_runs++ == 0) {
    times_full++;
    fill_report();
  }
  if (full_runs < FULL_RUNS)
    return;
  /* Enough of that. Make room; the first thing babysit then writes must
   * be its note of how much is missing. (It keeps one count, for the one
   * log a babysitter has, so this is settled before the scratch log is
   * written to again.) */
  if (ftruncate(fill_fd, 0) < 0)
    die("cannot empty the log that fills the disk");
  log_fd = fill_fd;
  log_write("", 0);
  log_fd = -1;
  n = (size_t)pread(fill_fd, start, sizeof start - 1, 0);
  start[n < sizeof start ? n : 0] = '\0';
  if (strncmp(start, "babysit: ", 9) != 0 || !strstr(start, " bytes of output"))
    die("after a full disk the log does not say what is missing");
  full_runs = 0;
  times_recovered++;
  fill_report();
}

static int no_room(void) {
  struct statvfs vfs;

  return statvfs(fuzz_root, &vfs) < 0 ||
         (unsigned long long)vfs.f_bavail * vfs.f_frsize < ROOM_NEEDED;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  static uint8_t run[20000];
  int status[2], s;
  size_t i, n;

  FUZZ_GUARD();
  expect_pat = NULL;
  if (size < 1)
    return 0;
  if (fill_fd >= 0 && (full_runs > 0 || no_room())) {
    fill_disk(data, size); /* the full disk gets the input as it is */
    return 0;
  }

  if (ftruncate(scratch_fd, 0) < 0)
    die("cannot empty the scratch log");
  memset(want, 0, sizeof want);
  refused = 0;
  arrived = 0;
  open_stream(0);
  open_stream(1);
  if (pipe(status) < 0)
    die("cannot make a pipe");
  set_nonblock(status[1]);
  log_fd = scratch_fd;
  status_fd = status[1];
  watching = 1;
  n = data[0] % 9;
  if (n > size - 1)
    n = size - 1;
  expect_pat = n ? text_of(data + 1, n) : NULL;
  expect_fixed = 1;

  for (i = 1 + n; i < size;) {
    unsigned op = data[i++] % 8;

    switch (op) {
    case 0:
    case 1:
      if (i >= size)
        break;
      n = data[i++];
      if (n > size - i)
        n = size - i;
      arrive((int)op, data + i, n);
      i += n;
      break;
    case 2:
      flush_partial(&streams[0]);
      flush_partial(&streams[1]);
      break;
    case 3:
      out_flush();
      break;
    case 4:
      if (full_fd >= 0) {
        log_fd = full_fd;
        refused = 1;
      }
      break;
    case 5:
      log_fd = scratch_fd;
      break;
    default:
      if (i + 2 >= size) {
        i = size;
        break;
      }
      n = (((size_t)data[i + 1] << 8) | data[i + 2]) % sizeof run;
      memset(run, data[i], n);
      i += 3;
      arrive((int)op - 6, run, n);
      break;
    }
  }

  /* The command exits: the pipes close, and what is left is logged. */
  log_fd = scratch_fd;
  for (s = 0; s < 2; s++) {
    close(feed_fd[s]);
    drain(&streams[s]);
  }
  out_flush();
  log_write("", 0); /* babysit's note, if anything was refused */
  if (status_fd >= 0)
    close(status_fd);
  status_fd = -1;
  close(status[0]);
  log_fd = -1;
  expect_pat = NULL;
  check_log();
  if (fill_fd >= 0)
    fill_disk((const uint8_t *)got, ngot);
  return 0;
}
