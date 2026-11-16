#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: babysitd leaves a log that is being written alone
#
# Goal:
#   A day directory is not archived, and a log in it not expired, for as
#   long as a log there has a writer, however old it all looks.
#
# Procedure:
#   1. Lay out a past day with two instances and have a helper hold the
#      writer lock on one of the logs, as a babysitter would.
#   2. Make everything look ten days old and run "babysitd --once" with a
#      one-day age limit.
#   3. Let the lock go and run it again.
#
# Fails if:
#   - the day is archived, or the locked log deleted, while the lock is
#     held.
#   - the finished log next to it is not expired in the meantime.
#   - the day is not archived once the lock is gone.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

d="$work/lk2"
plant "$d" 20230101/03/04/aaaa live
plant "$d" 20230101/03/05/bbbb finished
"$lp" --hold 3 "$d/20230101/03/04/aaaa/log" &
sleep 0.5
find "$d/20230101" -exec touch -t "$old10d" {} +
"$bsd" --once --log-dir "$d" --max-age-days 1 --config /dev/null -u 0 2>"$work/lk2.err"
check "a day with a writer is not archived" "$([ -e "$d/archive" ] && echo archived || echo no)" no
check "the log being written is not expired" "$(cat "$d/20230101/03/04/aaaa/log")" live
check "the finished log beside it is" "$([ -e "$d/20230101/03/05/bbbb" ] && echo present || echo gone)" gone
wait
settle "$d/20230101"
"$bsd" --once --log-dir "$d" --config /dev/null -u 0 2>>"$work/lk2.err"
check "once the writer is gone the day is archived" \
    "$(ls "$d/archive" | tr '\n' ' '; [ -e "$d/20230101" ] || echo removed)" "babysit_logs_20230101.tar.gz removed"

finish
