/*
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Matthew G. McGovern
 *
 * This file is part of babysit, which is distributed under the MIT
 * license; see the file LICENSE for details.
 */
/*
 * libFuzzer target: babysit's command line.
 *
 * The input is a command line, one argument per line. It is read as the
 * options of every subcommand in turn (start, check, info, watch, wait,
 * cat), quoted as a command line for the log, and each argument is tried
 * as the things an argument can be: an interval, a signal, an identifier.
 * Nothing is started. A crash, or memory misused, is the failure.
 */
#include "../../src/babysit_internal.h"

#include "fuzz_common.h"

void usage(FILE *f) { (void)f; } /* babysit.c, which has main(), is left out */

int LLVMFuzzerInitialize(int *argc, char ***argv) {
  (void)argc;
  (void)argv;
  fuzz_init();
  return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  char name[] = "babysit", *argv[32], *s, *joined;
  const char *ops[3];
  struct start_opts so;
  struct look o;
  long long ms;
  int argc, i;

  FUZZ_GUARD();
  expect_reset();
  s = text_of(data, size);
  argc = split_args(s, name, argv, 32);

  for (i = 1; i < argc; i++) {
    (void)parse_interval(argv[i], &ms);
    (void)parse_signal(argv[i]);
    (void)is_instance_id(argv[i]);
  }
  joined = join_cmdline(argv);
  free(joined);

  if (start_args(argc, argv, &so) == 0) {
    if (!so.cmd || !*so.cmd)
      die("start accepted a command line with no command");
    if (so.idle_ms == 0 || so.timeout_ms == 0)
      die("start accepted a limit of no time at all");
  }
  (void)look_args("check", argc, argv, 0, 0, &o, ops, 1);
  (void)look_args("info", argc, argv, 0, 1, &o, ops, 1);
  (void)look_args("watch", argc, argv, LOOK_WATCH, 0, &o, ops, 1);
  (void)look_args("wait", argc, argv, LOOK_WAIT, 0, &o, ops, 1);
  (void)look_args("cat", argc, argv, 0, 0, &o, ops, 3);
  expect_reset(); /* the patterns pointed into s */
  return 0;
}
