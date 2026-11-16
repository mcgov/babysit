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
#   1. Lay out a past day, a finished hour in a day that still has a
#      writer (with the disk limit set to 1% so that it is due), and two stale
#      .partial files.
#   2. Run babysitd once with a PATH that contains no tar.
#   3. Run it again with the normal PATH.
#
# Fails if:
#   - babysitd exits non-zero or does not warn about both failures.
#   - either planted log is missing, or any .partial or .tar.gz file exists
#     after the failed run.
#   - the second run does not produce the archive and the packed hour.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

plant "$work/tf" 20230101/00/01/aaaa a
plant "$work/tf" 20230102/02/03/bbbb b
plant "$work/tf" 20230102/04/05/cccc live
mkdir -p "$work/tf/archive"
: >"$work/tf/20230102/07.tar.gz.partial"; : >"$work/tf/archive/babysit_logs_x.tar.gz.partial"
"$lp" --hold 3 "$work/tf/20230102/04/05/cccc/log" &
sleep 0.5
PATH=/nonexistent "$bsd" --once --log-dir "$work/tf" --config /dev/null -u 1 2>"$work/d7"
check "tar missing: babysitd still exits 0" $? 0
has "tar missing: past day left in place, with a warning" "$work/d7" 'could not archive'
has "tar missing: finished hour left in place, with a warning" "$work/d7" 'could not pack'
check "tar missing: nothing lost, nothing half-written" \
    "$(cat "$work/tf/20230101/00/01/aaaa/log" "$work/tf/20230102/02/03/bbbb/log" | tr '\n' ' '; find "$work/tf" -name '*.partial' -o -name '*.tar.gz' | count)" "a b 0"
"$bsd" --once --log-dir "$work/tf" --config /dev/null -u 1 2>/dev/null
check "recovers on the next run once tar works" \
    "$(ls "$work/tf/archive" | tr '\n' ' '; ls "$work/tf/20230102" | tr '\n' ' ')" "babysit_logs_20230101.tar.gz 02.tar.gz 04 "
wait

finish
