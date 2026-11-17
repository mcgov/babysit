#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: max_age_days expires archives and finished logs
#
# Goal:
#   Archives and finished logs older than max_age_days are deleted; recent
#   ones and logs still being written are kept.
#
# Procedure:
#   1. Create two archives, a finished log and a log that is still being
#      written.
#   2. Run babysitd with a 5-day limit while everything is new.
#   3. Backdate everything by 10 days and run it again, taking the limit from
#      a config file.
#
# Fails if:
#   - anything is deleted while it is newer than the limit.
#   - the old archives or the old finished log survive.
#   - the log that is still being written is deleted.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

"$bs" start --log-dir "$work/d2" --detach -- sleep 3 >"$work/o10" 2>/dev/null
live=$(logof "$work/o10")
"$bs" start --log-dir "$work/d2" -- true >"$work/o10b" 2>/dev/null
done_log=$(logof "$work/o10b")
mkdir -p "$work/d2/archive"
: >"$work/d2/archive/babysit_logs_20200101.tar.gz"
: >"$work/d2/archive/babysit_logs_20210101.tar.gz"
"$bsd" --once --log-dir "$work/d2" --max-age-days 5 --config /dev/null -D 0 -u 0 2>/dev/null
check "babysitd: keeps recent archives" "$(ls "$work/d2/archive" | count)" 2
touch -t "$old10d" "$work/d2"/archive/*
find "$(dirname "$done_log")" "$(dirname "$live")" -exec touch -t "$old10d" {} +
printf 'max_age_days = 5\narchive_on_usage_percentage = 0\ndelete_at_disk_percent = 0\n' >"$work/conf"
"$bsd" --once --log-dir "$work/d2" --config "$work/conf" 2>/dev/null
check "babysitd: expires old archives (from config)" "$(ls "$work/d2/archive" | count)" 0
check "babysitd: expires old finished logs" "$([ -e "$done_log" ] && echo present || echo gone)" gone
check "babysitd: expiry leaves active logs" "$([ -e "$live" ] && echo kept)" kept

finish
