#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: --detach returns at once and the command carries on
#
# Goal:
#   --detach returns 0 as soon as the command has started; the command keeps
#   running and logging.
#
# Procedure:
#   1. Run a command that sleeps 2 seconds and then prints a line, with
#      --detach.
#   2. Time the call, then wait and read the log.
#
# Fails if:
#   - babysit does not exit 0, or takes 2 seconds or more.
#   - the "Process created" line is not printed.
#   - the line printed after babysit returned is missing from the log.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

t=$(date +%s)
"$bs" start --log-dir "$work/b" --detach -- sh -c 'sleep 2; echo bye' >"$work/o2" 2>&1
check "--detach exits 0" $? 0
check "--detach returns immediately" "$(quick "$t" 2)" y
has "--detach: the log has its header by the time it returns" "$(logof "$work/o2")" '^Process created with pid [0-9]'
sleep 3
has "detached command finished" "$(logof "$work/o2")" '^OUT: bye$'

finish
