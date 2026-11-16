/*
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Matthew G. McGovern
 *
 * This file is part of babysit, which is distributed under the MIT
 * license; see the file LICENSE for details.
 */
/*
 * lockprobe - test helper: report whether each given babysit log still has
 * a writer, using the very check babysit and babysitd rely on.
 *
 * Prints "locked" or "unlocked" per file; exits 0 if all are unlocked,
 * 1 if any is locked, 2 on usage error.
 *
 * With --hold it plays the babysitter instead: it takes the writer lock on
 * LOG and keeps it for SECONDS, so a test can have a "live" log in a
 * directory of its choosing.
 */
#include "../babysit.h"

int main(int argc, char **argv) {
  int i, any = 0;

  if (argc == 4 && strcmp(argv[1], "--hold") == 0) {
    int fd = open(argv[3], O_RDWR);

    if (fd < 0 || lock_byte(fd, LOCK_WRITER, F_WRLCK) < 0)
      return 2;
    sleep((unsigned)strtoul(argv[2], NULL, 10));
    return 0;
  }
  if (argc < 2) {
    fprintf(stderr, "usage: lockprobe LOG...\n"
                    "       lockprobe --hold SECONDS LOG\n");
    return 2;
  }
  for (i = 1; i < argc; i++) {
    int busy = log_has_writer(argv[i]);
    puts(busy ? "locked" : "unlocked");
    any |= busy;
  }
  return any;
}
