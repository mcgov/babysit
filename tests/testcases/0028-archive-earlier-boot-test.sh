#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: babysitd archives the logs of earlier boots
#
# Goal:
#   Every boot directory other than the current one is packed into
#   archive/babysit_logs_<boot>.tar.gz and removed; the current boot is left
#   alone; an existing archive is never overwritten.
#
# Procedure:
#   1. Plant a directory for an earlier boot; create a running and a finished
#      log in the current boot.
#   2. Run babysitd once.
#   3. Plant the same earlier boot again and run babysitd once more.
#
# Fails if:
#   - the archive is missing, is misnamed, or does not contain the planted
#     log.
#   - the earlier boot's directory is still there.
#   - either current-boot log is gone.
#   - the second archive overwrote the first instead of being named with
#     ".2".

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

oldboot=20250101-080000
mkdir -p "$work/d2/$oldboot/0/123/456"
echo x >"$work/d2/$oldboot/0/123/456/log"
"$bs" --log-dir "$work/d2" --detach -- sleep 3 >"$work/o10" 2>/dev/null
live=$(logof "$work/o10")
"$bs" --log-dir "$work/d2" -- true >"$work/o10b" 2>/dev/null
done_log=$(logof "$work/o10b")
"$bsd" --once --log-dir "$work/d2" --config /dev/null 2>/dev/null
tarball="$work/d2/archive/babysit_logs_$oldboot.tar.gz"
check "babysitd: earlier boot packed as babysit_logs_<boot>.tar.gz" \
    "$(tar -xzOf "$tarball" "$oldboot/0/123/456/log" 2>/dev/null)" x
check "babysitd: archived boot directory removed" "$([ -e "$work/d2/$oldboot" ] && echo present || echo gone)" gone
check "babysitd: current boot left in place" "$([ -e "$live" ] && [ -e "$done_log" ] && echo kept)" kept
mkdir -p "$work/d2/$oldboot/0/5/6"; echo again >"$work/d2/$oldboot/0/5/6/log"
"$bsd" --once --log-dir "$work/d2" --config /dev/null 2>/dev/null
check "babysitd: never overwrites an archive" \
    "$(ls "$work/d2/archive" | tr '\n' ' ')" "babysit_logs_$oldboot.2.tar.gz babysit_logs_$oldboot.tar.gz "

finish
