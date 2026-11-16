#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: a caller that keeps the pipe open but does not read
#
# Goal:
#   A reader that has stopped reading without going away must not be able
#   to hold babysit past its --timeout, and must not make a busy command
#   look idle.
#
# Procedure:
#   1. Pipe the stderr of babysit --timeout 2 into a reader that sleeps for 8 seconds
#      before reading, for a command that writes far more than a pipe holds
#      and then carries on.
#   2. Do the same with --idle 2 and a reader that sleeps for 4 seconds, for
#      a command that writes steadily and exits 7.
#
# Fails if:
#   - babysit takes more than 5 seconds to report the timeout, or reports
#     anything but 124.
#   - output written after the timeout is missing from the log.
#   - the steadily writing command is reported as stalled (125) instead of
#     by its own exit status.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

t0=$(date +%s)
{
    "$bs" start --log-dir "$work/sl" --timeout 2 -- \
        sh -c 'yes | head -c 2000000; echo after the flood' 2>&1 >/dev/null
    echo "$? $(elapsed "$t0")" >"$work/sl.rc"
} | { sleep 8; cat >/dev/null; }
read -r rc took <"$work/sl.rc"
check "--timeout is reported while the reader is not reading" "$rc" 124
check "and on time" "$([ "$took" -lt 5 ] && echo y)" y
check "the command carried on and its later output is in the log" \
    "$(grep -c '^OUT: after the flood$' "$(find "$work/sl" -name log)")" 1

{
    "$bs" start --log-dir "$work/sl2" --idle 2 -- \
        sh -c 'yes | head -c 2000000; exit 7' 2>&1 >/dev/null
    echo "$?" >"$work/sl2.rc"
} | { sleep 4; cat >/dev/null; }
check "waiting for the reader is not counted as the command being idle" \
    "$(cat "$work/sl2.rc")" 7

finish
