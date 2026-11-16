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
#   1. Lay out a superseded generation and an old log in the current
#      generation, and make one log in each unreadable.
#   2. Run "babysitd --once" with a one-day age limit.
#   3. Make them readable again and run it once more.
#
# Fails if:
#   - the generation is packed, or packing is even attempted, while its log
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
mkdir -p "$d/$stamp/0/1/2" "$d/$stamp/1/3/4" "$d/$stamp/1/5/6"
echo a >"$d/$stamp/0/1/2/log"; echo b >"$d/$stamp/1/3/4/log"; echo c >"$d/$stamp/1/5/6/log"
chmod 000 "$d/$stamp/0/1/2/log" "$d/$stamp/1/3/4/log"
touch -t "$old10d" "$d/$stamp/1/3/4/log" "$d/$stamp/1/3/4" "$d/$stamp/1/3"
touch -t "$old1m" "$d/$stamp/0"

"$bsd" --once --log-dir "$d" --config /dev/null -u 0 --max-age-days 1 2>"$work/ub.err"
check "the generation with an unreadable log is not packed" \
    "$(ls "$d/$stamp" | tr '\n' ' ')" "0 1 "
check "packing it was not even attempted" "$(grep -c 'could not pack' "$work/ub.err")" 0
check "the old unreadable log is not expired" \
    "$([ -e "$d/$stamp/1/3/4/log" ] && echo kept)" kept

chmod 644 "$d/$stamp/0/1/2/log" "$d/$stamp/1/3/4/log"
touch -t "$old10d" "$d/$stamp/1/3/4/log"
"$bsd" --once --log-dir "$d" --config /dev/null -u 0 --max-age-days 1 2>>"$work/ub.err"
check "once readable, the generation is packed and the old log expired" \
    "$(ls "$d/$stamp" | tr '\n' ' '; ls "$d/$stamp/1" | tr '\n' ' ')" "0.tar.gz 1 5 "

finish
