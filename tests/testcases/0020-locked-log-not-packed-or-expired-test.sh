#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: babysitd leaves a log that is being written alone
#
# Goal:
#   A generation or log with an active writer is neither packed nor expired,
#   however old it looks.
#
# Procedure:
#   1. Start a 3-second command with --detach.
#   2. Create a newer generation directory and backdate the old one, so it
#      qualifies for packing.
#   3. Run babysitd once; backdate everything in the old generation by 10
#      days and run babysitd again with a 1-day age limit.
#   4. Probe the log.
#
# Fails if:
#   - the log is no longer there and locked, which means babysitd packed or
#     deleted a log in use.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

"$bs" --log-dir "$work/lk2" --detach -- sleep 3 >"$work/l4" 2>/dev/null
mkdir "$work/lk2/$stamp/1"; touch -t "$old1m" "$work/lk2/$stamp/0"
"$bsd" --once --log-dir "$work/lk2" --max-age-days 1 --config /dev/null 2>/dev/null
find "$work/lk2/$stamp/0" -exec touch -t "$old10d" {} + 2>/dev/null
"$bsd" --once --log-dir "$work/lk2" --max-age-days 1 --config /dev/null 2>/dev/null
check "lock: a locked log is neither packed nor expired" "$("$lp" "$(logof "$work/l4")")" locked
sleep 3.5

finish
