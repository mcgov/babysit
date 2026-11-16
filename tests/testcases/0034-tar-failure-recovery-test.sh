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
#   1. Lay out an earlier boot, a superseded generation, and two stale
#      .partial files.
#   2. Run babysitd once with a PATH that contains no tar.
#   3. Run it again with the normal PATH.
#
# Fails if:
#   - babysitd exits non-zero or does not warn about both failures.
#   - either planted log is missing, or any .partial or .tar.gz file exists
#     after the failed run.
#   - the second run does not produce the archive and the packed generation.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

mkdir -p "$work/tf/20230101-000000/0/1/2" "$work/tf/$stamp/0/3/4" "$work/tf/$stamp/1" "$work/tf/archive"
echo a >"$work/tf/20230101-000000/0/1/2/log"; echo b >"$work/tf/$stamp/0/3/4/log"
: >"$work/tf/$stamp/7.tar.gz.partial"; : >"$work/tf/archive/babysit_logs_x.tar.gz.partial"
touch -t "$old1m" "$work/tf/$stamp/0"
PATH=/nonexistent "$bsd" --once --log-dir "$work/tf" --config /dev/null 2>"$work/d7"
check "tar missing: babysitd still exits 0" $? 0
has "tar missing: earlier boot left in place, with a warning" "$work/d7" 'could not archive'
has "tar missing: generation left in place, with a warning" "$work/d7" 'could not pack'
check "tar missing: nothing lost, nothing half-written" \
    "$(cat "$work/tf/20230101-000000/0/1/2/log" "$work/tf/$stamp/0/3/4/log" | tr '\n' ' '; find "$work/tf" -name '*.partial' -o -name '*.tar.gz' | count)" "a b 0"
"$bsd" --once --log-dir "$work/tf" --config /dev/null 2>/dev/null
check "recovers on the next run once tar works" \
    "$(ls "$work/tf/archive" | tr '\n' ' '; ls "$work/tf/$stamp" | tr '\n' ' ')" "babysit_logs_20230101-000000.tar.gz 0.tar.gz 1 "

finish
