#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: a log whose writer cannot be determined is left alone
#
# Goal:
#   babysitd packs and deletes only what has no writer. When it cannot find
#   out (here: a log it may not open), it must assume there is one rather
#   than assume there is none.
#
# Procedure:
#   1. Lay out a past day and an old log in today's directory, and make a
#      log in each unreadable.
#   2. Run "babysitd --once" with a one-day age limit.
#   3. Make them readable again and run it once more.
#
# Fails if:
#   - the day is archived, or archiving is even attempted, while its log
#     cannot be opened.
#   - the old unreadable log is expired.
#   - they are not packed and expired once they can be read.
#
# Skipped as root, who can open anything.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

if [ "$(id -u)" -eq 0 ]; then
    echo "ok   - skipped: root can open any file"
    finish
fi

d="$work/ub"
plant "$d" 20230101/01/02/aaaa a
plant "$d" "$today/03/04/bbbb" b
plant "$d" "$today/03/05/cccc" c
chmod 000 "$d/20230101/01/02/aaaa/log" "$d/$today/03/04/bbbb/log"
touch -t "$old10d" "$d/$today/03/04/bbbb/log" "$d/$today/03/04/bbbb"

"$bsd" --once --log-dir "$d" --config /dev/null -D 0 -u 0 --max-age-days 1 2>"$work/ub.err"
check "the day with an unreadable log is not archived" \
    "$([ -d "$d/20230101" ] && [ ! -e "$d/archive" ] && echo left)" left
check "archiving it was not even attempted" "$(grep -c 'could not' "$work/ub.err")" 0
check "the old unreadable log is not expired" \
    "$([ -e "$d/$today/03/04/bbbb/log" ] && echo kept)" kept

chmod 644 "$d/20230101/01/02/aaaa/log" "$d/$today/03/04/bbbb/log"
touch -t "$old10d" "$d/$today/03/04/bbbb/log"
"$bsd" --once --log-dir "$d" --config /dev/null -D 0 -u 0 --max-age-days 1 2>>"$work/ub.err"
check "once readable, the day is archived and the old log expired" \
    "$(ls "$d/archive" | tr '\n' ' '; ls "$d/$today/03" | tr '\n' ' ')" "babysit_logs_20230101.tar.gz 05 "

finish
