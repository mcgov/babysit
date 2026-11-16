#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: the log path carries both pids
#
# Goal:
#   The log path contains the pid of the babysit that was invoked and the
#   pid of the command, so a caller that knows the former can find the
#   latter.
#
# Procedure:
#   1. Start babysit through "exec" from a shell that first prints its own
#      pid, so babysit's pid is known.
#   2. Have the command print its own pid.
#   3. Compare the reported log path with <babysit pid>/<command pid>, and
#      list the babysit pid's directory.
#
# Fails if:
#   - the path does not contain exactly those two pids.
#   - listing <log-dir>/<boot>/*/<babysit pid> does not yield the command's
#     pid.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

sh -c 'echo $$; exec "$0" --log-dir "$1" -- sh -c "echo \$\$"' "$bs" "$work/g" >"$work/o1b" 2>/dev/null
ppid=$(sed -n 1p "$work/o1b")
cpid=$(sed -n 's/^OUT: //p' "$work/o1b")
check "log is at <babysit pid>/<child pid>/log" \
    "$(logof "$work/o1b")" "$work/g/$stamp/0/$ppid/$cpid/log"
check "babysit pid resolves to the child pid" "$(ls "$work/g/$stamp"/*/"$ppid")" "$cpid"

finish
