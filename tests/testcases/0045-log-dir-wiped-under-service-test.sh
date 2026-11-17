#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: the log directory is emptied while babysitd is running
#
# Goal:
#   Removing everything in the log directory, the service's own lock file
#   and request FIFO included, does not leave a running babysitd holding on
#   to files nobody else can reach. At its next pass it sets itself up
#   again and carries on.
#
# Procedure:
#   1. Start babysitd with a one-second check interval.
#   2. Remove the whole log directory, then plant a past day in a new one.
#   3. After the next pass, ask for another through the new request FIFO.
#
# Fails if:
#   - the request FIFO or the lock file is not there again.
#   - the planted day is not archived by the scheduled pass.
#   - the request through the new FIFO is not heard.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

d="$work/wp"
"$bsd" --log-dir "$d" --config /dev/null -D 0 -u 0 --check-interval 1s 2>"$work/wp.err" &
sv=$!
sleep 0.5
rm -rf "$d"
plant "$d" 20220101/00/01/aaaa z
sleep 2.5
check "the request FIFO and the lock file are back" \
    "$([ -p "$d/.request" ] && [ -f "$d/.lock" ] && echo y)" y
check "the scheduled pass works in the new directory" \
    "$(ls "$d/archive" 2>/dev/null | tr '\n' ' ')" "babysit_logs_20220101.tar.gz "
[ -p "$d/.request" ] && echo 1 >"$d/.request"
sleep 0.5
kill "$sv"; wait "$sv"
check "babysitd still stops cleanly" $? 0
has "a request through the new FIFO is heard" "$work/wp.err" 'woken by a request'

finish
