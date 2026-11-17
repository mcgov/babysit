#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: a log that is being written stays, the rest of its day does not wait
#
# Goal:
#   An instance whose log has a writer is never archived or expired, and it
#   stays at its path, so its identifier keeps working. It does not hold up
#   the finished instances around it: they are archived without it, and it
#   follows, in an archive of its own, once it has finished.
#
# Procedure:
#   1. Lay out a past day with two instances and have a helper hold the
#      writer lock on one of the logs, as a babysitter would.
#   2. Run "babysitd --once".
#   3. Make the log being written look ten days old and run it again with a
#      one-day age limit.
#   4. Let the lock go and run it once more.
#
# Fails if:
#   - the finished instance is not archived while the other is written, or
#     the one being written is archived with it.
#   - the instance being written is moved, expired or deleted.
#   - it is not archived, into a second archive for that day, once the lock
#     is gone, or the day's directory is left behind.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

d="$work/lk2"
once() { "$bsd" --once --log-dir "$d" --config /dev/null -u 0 "$@" 2>>"$work/lk2.err"; }
first="$d/archive/babysit_logs_20230101.tar.gz"

plant "$d" 20230101/03/04/aaaa live
plant "$d" 20230101/03/05/bbbb finished
"$lp" --hold 4 "$d/20230101/03/04/aaaa/log" &
sleep 0.5
once
check "the finished instance is archived without waiting for the other" \
    "$(tar -xzOf "$first" 20230101/03/05/bbbb/log 2>/dev/null)" finished
check "the one being written is not in the archive" "$(tar -tzf "$first" | grep -c aaaa)" 0
check "it stays at its path" "$(cat "$d/20230101/03/04/aaaa/log")" live
check "the archived one is gone from the day" \
    "$([ -e "$d/20230101/03/05/bbbb" ] && echo present || echo gone)" gone
has "the report says one stayed behind" "$work/lk2.err" \
    'archived the finished logs of 20230101 .* 1 still being written'

find "$d/20230101" -exec touch -t "$old10d" {} +
once --max-age-days 1
check "however old it looks, it is neither expired nor archived" \
    "$(cat "$d/20230101/03/04/aaaa/log"; ls "$d/archive" | count)" "live
1"

wait
settle "$d/20230101"
once
check "once its writer is gone it follows, in a second archive" \
    "$(tar -xzOf "$d/archive/babysit_logs_20230101.2.tar.gz" 20230101/03/04/aaaa/log 2>/dev/null)" live
check "and the day's directory is gone" "$([ -e "$d/20230101" ] && echo present || echo gone)" gone

finish
