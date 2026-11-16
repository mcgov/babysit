/*
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Matthew G. McGovern
 *
 * This file is part of babysit, which is distributed under the MIT
 * license; see the file LICENSE for details.
 */
/*
 * babysit - run a command under a babysitter that logs it and tells you
 * early when it stalls, prints an expected line, or runs too long.
 *
 * Three processes are involved:
 *
 *   parent      the babysit you invoked. It creates the instance, prints
 *               its identifier on stdout, then only waits for the
 *               babysitter to say why it should return, and turns that
 *               into an exit code.
 *   babysitter  detached (own session). Owns the command's stdout/stderr
 *               pipes, writes the log, echoes it to the caller's stderr,
 *               and runs the timers. When it decides the parent should
 *               return it closes its copy of the caller's stderr and keeps
 *               logging.
 *   child       the command. Its stdio never touches the caller's, so the
 *               parent and babysitter can let go without disturbing it.
 *
 * Instance: <YYYYmmdd>/<HH>/<MM>/<salt>, which is also where its log is:
 * <log-dir>/<instance>/log. See babysit.h.
 *
 * Stdout carries the identifier and nothing else, so that
 * id=$(babysit start ...) is all a caller needs to find the instance again.
 *
 * The babysitter records the command's exit status in <instance>/exit_code
 * and takes signals for it from <instance>/ctl. The other subcommands
 * (watch, check, info, signal) only ever look at an instance from outside,
 * through those files, the log and its locks.
 *
 * POSIX.1-2008, C99, no extensions.
 */
#include "babysit_internal.h"

void usage(FILE *f) {
  fprintf(
      f,
      "Usage: babysit start [options] [--] command [args...]\n"
      "       babysit watch ID [options]\n"
      "       babysit check ID\n"
      "       babysit info ID [--pid | --log | --exit-code]\n"
      "       babysit signal SIGNAL ID\n"
      "\n"
      "start runs COMMAND detached from this terminal/pipe and logs its "
      "output to\n"
      "LOGDIR/ID/log, where ID identifies this run: the minute it started "
      "and four\n"
      "random characters, for example 20261116/06/15/k3f9. ID is printed on "
      "stdout,\n"
      "alone; the command's output is echoed on stderr, each line prefixed "
      "with\n"
      "'OUT: ' (stdout) or 'ERR: ' (stderr). When the command exits, its "
      "status is\n"
      "recorded in LOGDIR/ID/exit_code. The other commands take that ID.\n"
      "\n"
      "By default start waits for the command and returns its exit status. "
      "The\n"
      "options below make it return early. When it does, the command is NOT\n"
      "stopped: it keeps running and its output keeps going to the log.\n"
      "\n"
      "Options for start and watch:\n"
      "  -e, --expect REGEX   return 0 once an output line matches the POSIX\n"
      "                       extended REGEX (matched without the OUT:/ERR: "
      "prefix)\n"
      "  -F, --fixed          treat the --expect pattern as a plain substring\n"
      "  -i, --idle SECS      return 125 if the command writes nothing for "
      "SECS\n"
      "  -t, --timeout SECS   return 124 after SECS in total\n"
      "Options for start:\n"
      "  -d, --detach         return 0 as soon as the command has started\n"
      "  -c, --config FILE    configuration file (default:\n"
      "                       $XDG_CONFIG_HOME/babysit/babysit.conf, or\n"
      "                       $HOME/.config/babysit/babysit.conf)\n"
      "Options for watch:\n"
      "      --onfail SIGNAL  on returning 124 or 125, send SIGNAL to the "
      "command\n"
      "Options for every command:\n"
      "  -l, --log-dir DIR    base log directory\n"
      "                       (default: $HOME/.local/var/log/babysit)\n"
      "  -V, --version        show the version\n"
      "  -h, --help           show this help\n"
      "\n"
      "watch follows the log of a run that was started earlier, from its "
      "beginning,\n"
      "and returns as start would; with no options it waits for the command "
      "to exit.\n"
      "check says whether the command is running (exit 0) or not (exit 1).\n"
      "info prints the run's log path, state and, while it is running, pid; "
      "or just\n"
      "the one asked for (--pid fails once the command has exited, "
      "--exit-code\n"
      "until it has). signal has the babysitter send SIGNAL (INT, TERM, 9, "
      "...) to\n"
      "the command's process group.\n"
      "\n"
      "SECS may be fractional and may carry an s, m or h suffix (0.5, 30s, "
      "2m).\n"
      "The command gets /dev/null as stdin and its own process group.\n"
      "\n"
      "Exit status of start and watch:\n"
      "  N        the command exited with status N (128+signal if killed)\n"
      "  0        --expect matched, or --detach\n"
      "  1        --expect was given but the command exited 0 without a match\n"
      "  124      --timeout expired; command still running\n"
      "  125      stalled for --idle seconds; command still running\n"
      "  126/127  the command could not be executed / was not found\n"
      "  75       the clock has gone backwards; nothing was started; try "
      "again\n"
      "           once babysitd has packed the records dated later than "
      "now\n"
      "  2        usage or internal error, or no such run\n");
}

int main(int argc, char **argv) {
  const char *c = argc > 1 ? argv[1] : "";

  if (strcmp(c, "-h") == 0 || strcmp(c, "--help") == 0) {
    usage(stdout);
    return 0;
  }
  if (strcmp(c, "-V") == 0 || strcmp(c, "--version") == 0) {
    puts("babysit " BABYSIT_VERSION);
    return 0;
  }
  if (strcmp(c, "start") == 0)
    return cmd_start(argc - 1, argv + 1);
  if (strcmp(c, "watch") == 0)
    return cmd_watch(argc - 1, argv + 1);
  if (strcmp(c, "check") == 0)
    return cmd_check(argc - 1, argv + 1);
  if (strcmp(c, "info") == 0)
    return cmd_info(argc - 1, argv + 1);
  if (strcmp(c, "signal") == 0)
    return cmd_signal(argc - 1, argv + 1);
  if (*c)
    fprintf(stderr, "babysit: unknown command '%s'\n\n", c);
  else
    fprintf(stderr, "babysit: no command given\n\n");
  usage(stderr);
  return EXIT_USAGE;
}
