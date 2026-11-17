/*
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Matthew G. McGovern
 *
 * This file is part of babysit, which is distributed under the MIT
 * license; see the file LICENSE for details.
 */
/*
 * libFuzzer target: babysit.conf, as both programs read it.
 *
 * The input is a configuration file. babysitd reads all of it, babysit
 * only the timezone. The input is also tried, as one string, as the
 * values babysitd takes: a number and a duration.
 *
 * Fails if: a setting ends up with a value the rest of babysitd does not
 * expect (negative, or an interval of no time); or the two programs
 * disagree on the timezone the same file gives.
 *
 * babysitd.c is one file with a main() of its own; it is included here
 * with that renamed, which also gives access to its static functions.
 */
#define main babysitd_main
#include "../../src/babysitd/babysitd.c"
#undef main

#include "fuzz_common.h"

/* babysit_cmdline.c; its header and babysitd.c both have a usage(). */
int load_timezone(const char *path, int required, int *utc);

static char conf[PATH_BUF + 16];

int LLVMFuzzerInitialize(int *argc, char ***argv) {
  (void)argc;
  (void)argv;
  fuzz_init();
  snprintf(conf, sizeof conf, "%s/babysit.conf", fuzz_root);
  return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  long max_age = 7, interval = 86400, disk = 90, del = 95, v;
  char *s;
  int utc = 0;

  FUZZ_GUARD();
  s = text_of(data, size);
  if (parse_num(s, &v) == 0 && v < 0)
    die("a negative number was accepted");
  if (parse_duration(s, &v) == 0 && v < 1)
    die("a duration of no time was accepted");

  if (put_file(conf, data, size) < 0)
    return 0; /* no room */
  use_utc = 0;
  archive_after = 1;
  load_config(conf, 0, &max_age, &interval, &disk, &del);
  if (max_age < 0 || interval < 1 || disk < 0 || del < 0 || archive_after < 0)
    die("a setting has a value babysitd does not expect");
  if (load_timezone(conf, 0, &utc) < 0 || (utc != 0 && utc != 1))
    die("babysit could not read the file babysitd read");
  if (utc != use_utc)
    die("babysit and babysitd disagree on the timezone");
  return 0;
}
