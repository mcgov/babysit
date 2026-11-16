/*
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Matthew G. McGovern
 *
 * This file is part of babysit, which is distributed under the MIT
 * license; see the file LICENSE for details.
 */
/*
 * babysit: "babysit check", "babysit info" and "babysit signal".
 */
#include "babysit_internal.h"

/* babysit check ID: 0 if the command is running, 1 if it is not. */
int cmd_check(int argc, char **argv) {
  const char *ops[1] = {NULL};
  struct instance in;
  struct look o;
  int code;

  if (look_args("check", argc, argv, 0, 0, &o, ops, 1) < 0 ||
      find_instance(o.dir, ops[0], &in) < 0)
    return EXIT_USAGE;
  switch (instance_state(&in, &code)) {
  case ST_RUNNING:
    puts("running");
    return 0;
  case ST_EXITED:
    printf("exited %d\n", code);
    return 1;
  default:
    puts("lost");
    return 1;
  }
}

/* babysit info ID [--pid | --log | --exit-code] */
int cmd_info(int argc, char **argv) {
  const char *ops[1] = {NULL};
  struct instance in;
  struct look o;
  int code, state;
  long pid;

  if (look_args("info", argc, argv, 0, 1, &o, ops, 1) < 0 ||
      o.pid + o.log + o.code > 1 || find_instance(o.dir, ops[0], &in) < 0) {
    if (o.pid + o.log + o.code > 1)
      fprintf(stderr, "babysit info: give one of --pid, --log and "
                      "--exit-code\n");
    return EXIT_USAGE;
  }
  state = instance_state(&in, &code);
  /* A pid is given out only while the babysitter vouches for it, that is
   * until it has reaped the command: after that the number may be
   * anyone's. */
  pid = state == ST_RUNNING ? logged_pid(in.log) : -1;
  if (o.log) {
    puts(in.log);
    return 0;
  }
  if (o.pid) {
    if (pid < 0) {
      fprintf(stderr, "babysit: the command of %s is not running\n", ops[0]);
      return 1;
    }
    printf("%ld\n", pid);
    return 0;
  }
  if (o.code) {
    if (state != ST_EXITED) {
      fprintf(stderr, "babysit: the command of %s has no exit status%s\n",
              ops[0], state == ST_RUNNING ? " yet: it is running" : "");
      return 1;
    }
    printf("%d\n", code);
    return 0;
  }
  printf("log: %s\n", in.log);
  if (state == ST_RUNNING) {
    puts("state: running");
    if (pid > 0)
      printf("pid: %ld\n", pid);
  } else if (state == ST_EXITED) {
    printf("state: exited\nexit_code: %d\n", code);
  } else {
    puts("state: lost");
  }
  return 0;
}

/* babysit signal SIGNAL ID */
int cmd_signal(int argc, char **argv) {
  const char *ops[2] = {NULL, NULL};
  struct instance in;
  struct look o;
  int sig;

  if (look_args("signal", argc, argv, 0, 0, &o, ops, 2) != 2) {
    fprintf(stderr, "babysit signal: expected a signal and an instance "
                    "identifier\n");
    return EXIT_USAGE;
  }
  sig = parse_signal(ops[0]);
  if (sig < 0) {
    fprintf(stderr, "babysit signal: unknown signal '%s'\n", ops[0]);
    return EXIT_USAGE;
  }
  if (find_instance(o.dir, ops[1], &in) < 0)
    return EXIT_USAGE;
  if (request_signal(&in, sig) < 0) {
    fprintf(stderr, "babysit: the command of %s is not running\n", ops[1]);
    return 1;
  }
  return 0;
}
