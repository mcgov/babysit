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
 */
#include "../babysit.h"

int main(int argc, char **argv) {
  int i, any = 0;

  if (argc < 2) {
    fprintf(stderr, "usage: lockprobe LOG...\n");
    return 2;
  }
  for (i = 1; i < argc; i++) {
    int busy = log_has_writer(argv[i]);
    puts(busy ? "locked" : "unlocked");
    any |= busy;
  }
  return any;
}
