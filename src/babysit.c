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
 *               pipes, writes the log, and runs the timers. It holds
 *               nothing of the caller's. When it decides the parent should
 *               return it tells it why and keeps logging.
 *   child       the command. Its stdio never touches the caller's, so the
 *               parent and babysitter can let go without disturbing it.
 *
 * Instance: <YYYYmmdd>/<HH>/<MM>/<salt>, which is also where its log is:
 * <log-dir>/<instance>/log. See babysit.h.
 *
 * Stdout carries the identifier and nothing else, so that
 * child_id=$(babysit start ...) is all a caller needs to find the instance
 * again.
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
      "       babysit watch CHILD_ID [options]\n"
      "       babysit wait CHILD_ID [--timeout SECS]\n"
      "       babysit check CHILD_ID\n"
      "       babysit info CHILD_ID [--pid | --log | --exit-code]\n"
      "       babysit signal SIGNAL CHILD_ID\n"
      "       babysit cat [stdout] [stderr] CHILD_ID\n"
      "\n"
      "start runs COMMAND detached from this terminal/pipe and logs its "
      "output to\n"
      "LOGDIR/CHILD_ID/log, where CHILD_ID identifies this run: the minute "
      "it\n"
      "started and four random characters, for example 20261116/06/15/k3f9.\n"
      "CHILD_ID is printed on stdout, alone. The command's output goes to "
      "the\n"
      "log and nowhere else, each line prefixed with 'OUT: ' (stdout) or "
      "'ERR: '\n"
      "(stderr); read it with cat, watch or tail -f. When the command exits, "
      "its\n"
      "status is recorded in LOGDIR/CHILD_ID/exit_code.\n"
      "The other commands take that CHILD_ID.\n"
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
      "  -f, --follow         show the log on stderr as it grows, until "
      "start\n"
      "                       returns; for watching at a terminal\n"
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
      "wait blocks in silence until the command has exited and returns its "
      "exit\n"
      "status, or 124 if --timeout passes first; it reads no output at all.\n"
      "check, or status, says whether the command is running (exit 0) or "
      "not\n"
      "(exit 1).\n"
      "info prints the run's log path, state and, while it is running, pid; "
      "or just\n"
      "the one asked for (--pid fails once the command has exited, "
      "--exit-code\n"
      "until it has). signal has the babysitter send SIGNAL (INT, TERM, 9, "
      "...) to\n"
      "the command's process group. cat prints what the command has "
      "written so\n"
      "far, without the OUT:/ERR: labels: its stdout, its stderr, or both "
      "if\n"
      "neither or both are named.\n"
      "\n"
      "SECS may be fractional and may carry an s, m or h suffix (0.5, 30s, "
      "2m).\n"
      "The command gets /dev/null as stdin and its own process group.\n"
      "\n"
      "Exit status of start, watch and wait:\n"
      "  N        the command exited with status N (128+signal if killed)\n"
      "  0        --expect matched, or --detach\n"
      "  1        --expect was given but the command exited 0 without a match\n"
      "  124      --timeout expired; command still running\n"
      "  125      stalled for --idle seconds; command still running\n"
      "  126/127  the command could not be executed / was not found\n"
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
  if (strcmp(c, "wait") == 0)
    return cmd_wait(argc - 1, argv + 1);
  if (strcmp(c, "check") == 0 || strcmp(c, "status") == 0)
    return cmd_check(argc - 1, argv + 1);
  if (strcmp(c, "info") == 0)
    return cmd_info(argc - 1, argv + 1);
  if (strcmp(c, "signal") == 0)
    return cmd_signal(argc - 1, argv + 1);
  if (strcmp(c, "cat") == 0)
    return cmd_cat(argc - 1, argv + 1);
  if (*c)
    fprintf(stderr, "babysit: unknown command '%s'\n\n", c);
  else
    fprintf(stderr, "babysit: no command given\n\n");
  usage(stderr);
  return EXIT_USAGE;
}
