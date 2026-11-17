/*
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Matthew G. McGovern
 *
 * This file is part of babysit, which is distributed under the MIT
 * license; see the file LICENSE for details.
 */
/*
 * libFuzzer target: an instance on disk, as the commands that look at one
 * find it.
 *
 * The input up to its first newline becomes the instance's exit_code
 * file (no such file if that is empty); the rest becomes its log. Then
 * check, info, cat (all of it, and stdout alone), wait and watch (with a
 * fixed --expect) are run on it. Logs are written by the babysitter, but
 * they hold whatever a command printed, and a log directory can hold
 * anything at all. A crash, or memory misused, is the failure.
 */
#include "../../src/babysit_internal.h"

#include "fuzz_common.h"

void usage(FILE *f) { (void)f; }

#define FUZZ_ID "20200101/00/00/abcd"
static char inst_log[PATH_BUF + 64], inst_code[PATH_BUF + 64];

int LLVMFuzzerInitialize(int *argc, char ***argv) {
  char dir[PATH_BUF + 64];

  (void)argc;
  (void)argv;
  fuzz_init();
  snprintf(dir, sizeof dir, "%s/%s", fuzz_root, FUZZ_ID);
  if (mkdirs(dir) < 0)
    die("cannot make the instance directory");
  snprintf(inst_log, sizeof inst_log, "%s/log", dir);
  snprintf(inst_code, sizeof inst_code, "%s/exit_code", dir);
  return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  char id[] = FUZZ_ID, name[] = "babysit", dash_l[] = "--log-dir";
  char out[] = "stdout", exp[] = "--expect", fix[] = "--fixed", x[] = "x";
  char *argv[8];
  const uint8_t *nl = memchr(data, '\n', size);
  size_t head = nl ? (size_t)(nl - data) + 1 : size;
  int from;

  FUZZ_GUARD();
  expect_reset();
  if (head > 1) {
    if (put_file(inst_code, data, head) < 0)
      return 0; /* no room */
  } else {
    unlink(inst_code);
  }
  if (put_file(inst_log, data + head, size - head) < 0)
    return 0;

  from = lowest_free_fd();
  argv[0] = name;
  argv[1] = dash_l;
  argv[2] = fuzz_root;
  argv[3] = id;
  argv[4] = NULL;
  (void)logged_pid(inst_log);
  (void)cmd_check(4, argv);
  (void)cmd_info(4, argv);
  (void)cmd_cat(4, argv);
  (void)cmd_wait(4, argv);
  argv[4] = out;
  argv[5] = NULL;
  (void)cmd_cat(5, argv);
  argv[4] = exp;
  argv[5] = x;
  argv[6] = fix;
  argv[7] = NULL;
  (void)cmd_watch(7, argv);
  close_from(from);
  expect_reset();
  return 0;
}
