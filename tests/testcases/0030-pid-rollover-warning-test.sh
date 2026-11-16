#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: babysitd warns when the pids wrap around
#
# Goal:
#   babysitd compares the last allocated pid between wake-ups and warns when
#   it has gone down. The value is read from a file here so that the test
#   can make it go down.
#
# Procedure:
#   1. Write 5000 to a file and start babysitd reading it, with a 1-second
#      interval.
#   2. Change the file to 300, wait for a wake-up, stop babysitd.
#
# Fails if:
#   - the warning naming both values is not logged.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

echo 5000 >"$work/lastpid"
"$bsd" --log-dir "$work/d2" --last-pid-file "$work/lastpid" \
    --check-interval 1s --config /dev/null 2>"$work/e11" &
dpid=$!
sleep 1.5; echo 300 >"$work/lastpid"; sleep 1.5; kill "$dpid"; wait "$dpid" 2>/dev/null
has "babysitd: warns on pid rollover" "$work/e11" 'pid rollover detected (last pid went from 5000 to 300)'

finish
