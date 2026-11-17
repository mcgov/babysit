/*
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Matthew G. McGovern
 *
 * This file is part of babysit, which is distributed under the MIT
 * license; see the file LICENSE for details.
 */
/*
 * perftool - helper for the performance tests (tests/perf.sh).
 *
 *   perftool gen LINES WIDTH
 *       Write LINES lines of WIDTH characters each to stdout, as fast as
 *       it can: the "command" whose output babysit has to carry.
 *
 *   perftool time OUTFILE -- COMMAND [ARGS...]
 *       Run COMMAND, wait for it, and write the wall-clock seconds it took
 *       to OUTFILE. The exit status is COMMAND's. Timing is done here, in
 *       C and on the monotonic clock, so that the same small fixed cost is
 *       added to every measurement, whichever shell runs the tests.
 */
#include "../src/babysit.h"

static int gen(const char *lines_s, const char *width_s) {
  static char block[1 << 16];
  unsigned long lines = strtoul(lines_s, NULL, 10);
  unsigned long width = strtoul(width_s, NULL, 10);
  size_t per_block, len, i;

  if (width == 0 || width + 1 > sizeof block)
    return 2;
  per_block = sizeof block / (width + 1);
  for (i = 0; i < per_block; i++) {
    memset(block + (i * (width + 1)), 'a' + (int)(i % 26), width);
    block[(i * (width + 1)) + width] = '\n';
  }
  while (lines > 0) {
    size_t n = lines < per_block ? (size_t)lines : per_block;
    const char *p = block;

    for (len = n * (width + 1); len > 0;) {
      ssize_t w = write(1, p, len);

      if (w < 0) {
        if (errno == EINTR)
          continue;
        return 1;
      }
      p += w;
      len -= (size_t)w;
    }
    lines -= n;
  }
  return 0;
}

static double now(void) {
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + ((double)ts.tv_nsec / 1e9);
}

static int timed(const char *outfile, char **cmd) {
  double start = now(), took;
  int status = 0;
  pid_t pid = fork();
  FILE *f;

  if (pid < 0)
    return 2;
  if (pid == 0) {
    /* Running what the test named is the whole point. */
    /* NOLINTNEXTLINE(clang-analyzer-optin.taint.GenericTaint) */
    execvp(cmd[0], cmd);
    _exit(127);
  }
  while (waitpid(pid, &status, 0) < 0)
    if (errno != EINTR)
      return 2;
  took = now() - start;
  f = fopen(outfile, "w");
  if (!f)
    return 2;
  fprintf(f, "%.6f\n", took);
  if (fclose(f) != 0)
    return 2;
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

int main(int argc, char **argv) {
  if (argc == 4 && strcmp(argv[1], "gen") == 0)
    return gen(argv[2], argv[3]);
  if (argc >= 5 && strcmp(argv[1], "time") == 0 && strcmp(argv[3], "--") == 0)
    return timed(argv[2], argv + 4);
  fprintf(stderr, "usage: perftool gen LINES WIDTH\n"
                  "       perftool time OUTFILE -- COMMAND [ARGS...]\n");
  return 2;
}
