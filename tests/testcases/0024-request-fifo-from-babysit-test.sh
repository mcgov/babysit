#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: babysit wakes babysitd when it starts a generation
#
# Goal:
#   A running babysitd listens on <log-dir>/.request. When babysit starts a
#   new generation it writes there, and babysitd packs the old generation
#   without waiting for its 6-hour interval. babysit must not be affected
#   when nothing is listening.
#
# Procedure:
#   1. Start babysitd with its default interval.
#   2. Trigger a new generation with the reused-pid technique.
#   3. Wait 5 seconds, stop babysitd, and inspect its messages and the
#      directory.
#   4. Run babysit again with the FIFO present but no reader.
#
# Fails if:
#   - the FIFO is not created.
#   - babysitd does not report being woken, or generation 0 is not packed.
#   - babysit fails or hangs when nobody reads the FIFO.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

"$bsd" --log-dir "$work/q" --config /dev/null 2>"$work/e14" &   # default interval: 6h
qd=$!
sleep 0.5
check "babysitd: listens on a FIFO" "$([ -p "$work/q/.request" ] && echo fifo)" fifo
sh -c 'mkdir -p "$2/0/$$/999"; echo old >"$2/0/$$/999/log"; exec "$0" --log-dir "$1" -- true' \
    "$bs" "$work/q" "$work/q/$stamp" >/dev/null 2>&1
sleep 5
kill "$qd"; wait "$qd" 2>/dev/null
has "babysitd: woken by babysit" "$work/e14" 'woken by a request'
check "babysitd: packs on request, without waiting for the interval" \
    "$(ls "$work/q/$stamp" | tr '\n' ' ')" "0.tar.gz 1 "
"$bs" --log-dir "$work/q" -- true >/dev/null 2>&1
check "babysit is unaffected by a FIFO nobody reads" $? 0

finish
