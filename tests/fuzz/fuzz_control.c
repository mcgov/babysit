/*
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Matthew G. McGovern
 *
 * This file is part of babysit, which is distributed under the MIT
 * license; see the file LICENSE for details.
 */
/*
 * libFuzzer target: what the babysitter is told from outside, and what it
 * leaves behind.
 *
 * The input is written to an instance's control FIFO, the one "babysit
 * signal" writes a signal number to, and the babysitter's reader is run
 * on it. Anybody who can write to the log directory can write anything
 * there. The signals go to a process of our own that exists for the
 * purpose, in a process group of its own, and to nothing else: it is
 * started again whenever one of them has killed it.
 *
 * Then the input's first byte is recorded as the command's exit status,
 * and read back the way check, info and wait read it.
 *
 * Fails if: the log gets a line that is not "signal N sent" with N a
 * signal number; or the recorded status does not read back as itself.
 */
#include "../../src/babysit_internal.h"

#include "fuzz_common.h"
#include <sys/wait.h>

void usage(FILE *f) { (void)f; }

#define FUZZ_ID "20200101/00/00/abcd"
static char dir[PATH_BUF + 64];
static int scratch_fd = -1;
static pid_t victim = -1;

/* Something to signal: sleeps in its own process group until we are gone. */
static void new_victim(void) {
  struct timespec nap = {0, 100000000};
  pid_t parent = getpid();
  sigset_t all, old;

  /* The fuzzer has handlers for several signals, and the victim starts as
   * a copy of it. No signal may reach it before it has put them back. */
  sigfillset(&all);
  sigprocmask(SIG_BLOCK, &all, &old);
  victim = fork();
  if (victim < 0)
    die("cannot fork");
  if (victim == 0) {
    int sig;

    for (sig = 1; sig < 65; sig++)
      signal(sig, SIG_DFL);
    setpgid(0, 0);
    sigprocmask(SIG_SETMASK, &old, NULL);
    while (getppid() == parent)
      nanosleep(&nap, NULL);
    _exit(0);
  }
  setpgid(victim, victim);
  sigprocmask(SIG_SETMASK, &old, NULL);
}

int LLVMFuzzerInitialize(int *argc, char ***argv) {
  char path[PATH_BUF + 96];

  (void)argc;
  (void)argv;
  fuzz_init();
  snprintf(dir, sizeof dir, "%s/%s", fuzz_root, FUZZ_ID);
  if (mkdirs(dir) < 0)
    die("cannot make the instance directory");
  snprintf(path, sizeof path, "%s/log", dir);
  scratch_fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_APPEND, 0600);
  if (scratch_fd < 0)
    die("cannot make the log");
  ctl_open(dir);
  if (ctl_fd < 0 || ctl_wfd < 0)
    die("cannot open the control FIFO");
  set_nonblock(ctl_wfd);
  new_victim();
  return 0;
}

static void check_log(void) {
  static char got[1 << 20];
  ssize_t n = pread(scratch_fd, got, sizeof got - 1, 0);
  char *p, *nl;
  int sig, used;

  if (n < 0)
    die("cannot read the log back");
  got[n] = '\0';
  for (p = got; *p; p = nl + 1) {
    nl = strchr(p, '\n');
    if (!nl)
      die("the log does not end with a newline");
    used = 0;
    if (sscanf(p, "babysit: signal %d sent to the command on request%n", &sig,
               &used) != 1 ||
        p + used != nl || sig < 1 || sig > 64)
      die("the log has a line that is not a signal sent");
  }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  char id[] = FUZZ_ID;
  struct instance in;
  int status, code;

  FUZZ_GUARD();
  if (size < 1)
    return 0;
  /* A victim that a signal has killed stays a zombie, and its number its
   * own, until it is replaced here: nothing else can be hit meanwhile. */
  if (waitpid(victim, &status, WNOHANG) == victim)
    new_victim();
  kill(victim, SIGCONT);

  if (ftruncate(scratch_fd, 0) < 0)
    die("cannot empty the log");
  log_fd = scratch_fd;
  status_fd = -1;
  watching = 0;
  if (size > 60000) /* a FIFO takes this much without a reader */
    size = 60000;
  if (write(ctl_wfd, data, size) < 0 || write(ctl_wfd, "\n", 1) < 0)
    die("cannot write to the control FIFO");
  ctl_read(victim);
  out_flush();
  log_fd = -1;
  check_log();

  record_exit(dir, data[0]);
  if (find_instance(fuzz_root, id, &in) < 0)
    die("the instance is not found");
  if (instance_state(&in, &code) != ST_EXITED || code != data[0])
    die("the recorded exit status does not read back as itself");
  return 0;
}
