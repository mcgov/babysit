#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: babysitd, odd settings and odd files
#
# Goal:
#   Settings at their edges mean what the manual says, and a log directory
#   that is not laid out as babysitd expects costs no logs: an interval too
#   large to count is refused, "max_age_days 0" keeps everything, an old
#   packed hour is expired like any other old log, and when the archive
#   directory cannot be made the logs stay where they are.
#
# Procedure:
#   1. Give --check-interval a number of days that overflows.
#   2. Make an archive and a packed hour ten days old, next to a fresh
#      packed hour; run a pass with --max-age-days 0, then with 7.
#   3. Put a regular file where the archive directory goes, with a
#      finished instance from a past day; run a pass.
#
# Fails if:
#   - the overflowing interval is not refused with exit 2.
#   - --max-age-days 0 deletes anything.
#   - --max-age-days 7 leaves the old archive or the old packed hour, or
#     deletes the fresh packed hour.
#   - with no archive directory the instance's log is gone, the pass does
#     not say what is wrong, or the file in the way is touched.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

d() { "$bsd" --once --config /dev/null -D 0 -u 0 "$@"; }

d --log-dir "$work/a" --check-interval 99999999999999999d 2>"$work/e1"
check "an interval too large to count: exit 2" $? 2
has "and it is named" "$work/e1" '99999999999999999d'

A="$work/a"
mkdir -p "$A/archive" "$A/$today"
echo old >"$A/archive/babysit_logs_20200101.tar.gz"
echo old >"$A/$today/03.tar.gz"
echo new >"$A/$today/04.tar.gz"
touch -t "$old10d" "$A/archive/babysit_logs_20200101.tar.gz" "$A/$today/03.tar.gz"
touch -t "$old1m" "$A/$today/04.tar.gz"
d --log-dir "$A" --max-age-days 0 2>/dev/null
check "max age 0: the old archive is kept" "$([ -f "$A/archive/babysit_logs_20200101.tar.gz" ] && echo y)" y
check "and so is the old packed hour" "$([ -f "$A/$today/03.tar.gz" ] && echo y)" y
d --log-dir "$A" --max-age-days 7 2>/dev/null
check "max age 7: the old archive is deleted" "$([ -e "$A/archive/babysit_logs_20200101.tar.gz" ] || echo y)" y
check "the old packed hour is deleted" "$([ -e "$A/$today/03.tar.gz" ] || echo y)" y
check "the fresh packed hour is kept" "$([ -f "$A/$today/04.tar.gz" ] && echo y)" y

B="$work/b"
past=$(daysago 3)
plant "$B" "$past/00/01/aaaa" "keep me"
echo "in the way" >"$B/archive"
d --log-dir "$B" --max-age-days 0 2>"$work/e2"
check "no archive directory to be had: the log stays" "$(cat "$B/$past/00/01/aaaa/log" 2>/dev/null)" "keep me"
has "the pass says what is wrong" "$work/e2" 'cannot create .*/archive'
check "the file in the way is left alone" "$(cat "$B/archive")" "in the way"

finish
