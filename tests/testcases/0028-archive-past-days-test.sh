#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: past days are archived
#
# Goal:
#   Every day directory before today's is packed into
#   archive/babysit_logs_<YYYYmmdd>.tar.gz and removed, today's is left in
#   place, and an archive that already exists is never overwritten.
#
# Procedure:
#   1. Lay out a past day, run two commands today (one still running), and
#      run "babysitd --once".
#   2. Lay out the same past day again and run it once more.
#   3. Take every archive name up to ".99" for that day and try a third
#      time.
#
# Fails if:
#   - the archive is missing, is misnamed, or does not contain the log.
#   - the past day's directory is still there.
#   - either of today's logs is gone.
#   - the second archive overwrote the first instead of being named with
#     ".2".
#   - with every name up to ".99" taken, an existing archive is overwritten
#     or the day's directory is removed.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

day=20250101
plant "$work/d2" "$day/08/09/aaaa" x
"$bs" start --log-dir "$work/d2" --detach -- sleep 3 >"$work/o10" 2>/dev/null
live=$(logof "$work/o10")
"$bs" start --log-dir "$work/d2" -- true >"$work/o10b" 2>/dev/null
done_log=$(logof "$work/o10b")
"$bsd" --once --log-dir "$work/d2" --config /dev/null -D 0 -u 0 2>/dev/null
tarball="$work/d2/archive/babysit_logs_$day.tar.gz"
check "babysitd: past day packed as babysit_logs_<day>.tar.gz" \
    "$(tar -xzOf "$tarball" "$day/08/09/aaaa/log" 2>/dev/null)" x
check "babysitd: archived day directory removed" "$([ -e "$work/d2/$day" ] && echo present || echo gone)" gone
check "babysitd: today left in place" "$([ -e "$live" ] && [ -e "$done_log" ] && echo kept)" kept
plant "$work/d2" "$day/10/11/bbbb" again
"$bsd" --once --log-dir "$work/d2" --config /dev/null -D 0 -u 0 2>/dev/null
check "babysitd: never overwrites an archive" \
    "$(ls "$work/d2/archive" | tr '\n' ' ')" "babysit_logs_$day.2.tar.gz babysit_logs_$day.tar.gz "

for i in $(seq 3 99); do echo "kept $i" >"$work/d2/archive/babysit_logs_$day.$i.tar.gz"; done
plant "$work/d2" "$day/12/13/cccc" third
"$bsd" --once --log-dir "$work/d2" --config /dev/null -D 0 -u 0 2>"$work/d2.err"
check "babysitd: with every archive name taken, nothing is overwritten or removed" \
    "$(cat "$work/d2/archive/babysit_logs_$day.99.tar.gz" "$work/d2/$day/12/13/cccc/log" | tr '\n' ' ')" \
    "kept 99 third "
has "babysitd: and it says so" "$work/d2.err" 'every archive name for'

finish
