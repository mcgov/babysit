#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: a failed pack loses nothing
#
# Goal:
#   If tar cannot be run, babysitd leaves every directory in place, leaves
#   no half-written file, removes stale .partial files, warns, and exits 0;
#   the next run completes the work.
#
# Procedure:
#   1. Lay out two past days and two stale .partial files; later, a finished
#      hour of today with the disk limit set to 1% so that it is due.
#   2. Run babysitd once with a PATH that contains no tar.
#   3. Run it again with the normal PATH.
#
# Fails if:
#   - babysitd exits non-zero or does not warn about both failures.
#   - either planted log is missing, or any .partial or .tar.gz file exists
#     after the failed run.
#   - the second run does not produce the two archives.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

plant "$work/tf" 20230101/00/01/aaaa a
plant "$work/tf" 20230102/02/03/bbbb b
mkdir -p "$work/tf/archive" "$work/tf/$today"
: >"$work/tf/$today/07.tar.gz.partial"; : >"$work/tf/archive/babysit_logs_x.tar.gz.partial"
PATH=/nonexistent "$bsd" --once --log-dir "$work/tf" --config /dev/null -D 0 -u 0 2>"$work/d7"
check "tar missing: babysitd still exits 0" $? 0
check "tar missing: both days left in place, each with a warning" \
    "$(grep -c 'could not archive' "$work/d7")" 2
check "tar missing: nothing lost, nothing half-written" \
    "$(cat "$work/tf/20230101/00/01/aaaa/log" "$work/tf/20230102/02/03/bbbb/log" | tr '\n' ' '; find "$work/tf" -name '*.partial' -o -name '*.tar.gz' | count)" "a b 0"
"$bsd" --once --log-dir "$work/tf" --config /dev/null -D 0 -u 0 2>/dev/null
check "recovers on the next run once tar works" \
    "$(ls "$work/tf/archive" | tr '\n' ' '; ls -d "$work/tf"/2023* 2>/dev/null | count)" \
    "babysit_logs_20230101.tar.gz babysit_logs_20230102.tar.gz 0"

now=$(date +%H)
if [ "$now" != 00 ]; then
    prev=$(printf '%02d' $((10#$now - 1)))
    plant "$work/tf" "$today/$prev/00/cccc" c
    PATH=/nonexistent "$bsd" --once --log-dir "$work/tf" --config /dev/null -D 0 -u 1 2>"$work/d7b"
    has "tar missing: a finished hour of today left in place, with a warning" "$work/d7b" 'could not pack'
    check "tar missing: its log is still there" "$(cat "$work/tf/$today/$prev/00/cccc/log")" c
fi

finish
