#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: the archive window
#
# Goal:
#   archive_after_days says how long finished logs are left in place: a day
#   is archived once it is that many days old. The default, 1, is every day
#   before today. 0 never archives by age. A filesystem at
#   archive_on_usage_percentage closes the window: every day before today
#   is archived at once.
#
# Procedure:
#   1. Lay out finished instances dated 1, 3 and 10 days ago.
#   2. Run "babysitd --once" with a window of 5 days, then 2 (from the
#      configuration file), then 0, then the default.
#   3. Lay out days 2 and 20 days ago and run with a window of 30 days and
#      a usage limit of 1%, which any filesystem in use exceeds.
#
# Fails if:
#   - a day inside the window is archived, or one outside it is not.
#   - a window of 0 archives anything.
#   - the default does not archive yesterday.
#   - a full disk does not archive every day before today.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

d="$work/aw"
d1=$(daysago 1); d3=$(daysago 3); d10=$(daysago 10)
archived() { ls "$d/archive" 2>/dev/null | sed 's/babysit_logs_//; s/\.tar\.gz//' | tr '\n' ' '; }
once() { "$bsd" --once --log-dir "$d" --config /dev/null -D 0 -u 0 "$@" 2>>"$work/aw.err"; }

plant "$d" "$d1/01/02/aaaa" one
plant "$d" "$d3/01/02/bbbb" three
plant "$d" "$d10/01/02/cccc" ten

once --archive-after-days 5
check "a window of 5 days archives only what is older" "$(archived)" "$d10 "
check "and leaves the rest readable in place" \
    "$(cat "$d/$d1/01/02/aaaa/log" "$d/$d3/01/02/bbbb/log" | tr '\n' ' ')" "one three "

printf 'archive_after_days = 2\narchive_on_usage_percentage = 0\ndelete_at_disk_percent = 0\n' >"$work/aw.conf"
"$bsd" --once --log-dir "$d" --config "$work/aw.conf" 2>>"$work/aw.err"
check "a window of 2 days, from the configuration file" "$(archived)" "$d10 $d3 "

once --archive-after-days 0
check "a window of 0 never archives by age" "$(archived)" "$d10 $d3 "

once
check "the default archives every day before today" "$(archived)" "$d10 $d3 $d1 "

d2=$(daysago 2); d20=$(daysago 20)
plant "$d" "$d2/01/02/dddd" two
plant "$d" "$d20/01/02/eeee" twenty
"$bsd" --once --log-dir "$d" --config /dev/null -D 0 --archive-after-days 30 -u 0 2>>"$work/aw.err"
check "inside a 30-day window nothing more is archived" "$(archived | wc -w | tr -d ' ')" 3
"$bsd" --once --log-dir "$d" --config /dev/null -D 0 --archive-after-days 30 -u 1 2>>"$work/aw.err"
check "a full disk closes the window" "$(archived | wc -w | tr -d ' ')" 5

"$bsd" --once --log-dir "$d" --config /dev/null -D 0 --archive-after-days x 2>/dev/null
check "a bad --archive-after-days is rejected" $? 2

finish
