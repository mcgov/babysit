#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: today's finished hours are packed when the disk fills up
#
# Goal:
#   At compact_at_disk_percent the service stops waiting for today to be
#   over: every hour of it that is over is packed in place as
#   <day>/<HH>.tar.gz, as far as it is finished. An instance still being
#   written stays where it is, and the hour that is not over yet is left
#   alone.
#
# Procedure:
#   1. In the hour before this one, lay out a finished instance and one on
#      whose log a helper holds a writer lock. Lay out a finished instance
#      in the current hour.
#   2. Run "babysitd --once" with the limit off, then with a limit of 1%,
#      which any filesystem in use exceeds.
#   3. Let the lock go and run it once more.
#
# Fails if:
#   - anything is packed while the limit is off.
#   - the finished instance of the earlier hour is not packed into
#     <HH>.tar.gz, or the one being written is packed with it or moved.
#   - the current hour is packed.
#   - the straggler is not packed into <HH>.2.tar.gz once nothing writes.
#
# Skipped in the first hour of the day, when today has no earlier hour.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

now=$(date +%H)
if [ "$now" = 00 ]; then
    echo "ok   - skipped: today has no finished hour yet"
    finish
fi
prev=$(printf '%02d' $((10#$now - 1)))
d="$work/ph"
once() { "$bsd" --once --log-dir "$d" --config /dev/null "$@" 2>>"$work/ph.err"; }

plant "$d" "$today/$prev/04/aaaa" earlier
plant "$d" "$today/$prev/05/bbbb" live
plant "$d" "$today/$now/00/dddd" current
"$lp" --hold 4 "$d/$today/$prev/05/bbbb/log" &
sleep 0.5

once -u 0
check "nothing is packed while the limit is off" "$(ls "$d/$today" | tr '\n' ' ')" "$prev $now "

once -u 1
check "the earlier hour is packed as far as it is finished" \
    "$(ls "$d/$today" | tr '\n' ' ')" "$prev $prev.tar.gz $now "
check "the packed hour holds the finished log" \
    "$(tar -xzOf "$d/$today/$prev.tar.gz" "$prev/04/aaaa/log" 2>/dev/null)" earlier
check "the log being written stays at its path" "$(cat "$d/$today/$prev/05/bbbb/log")" live
has "packing is reported with the reason" "$work/ph.err" \
    "full (limit 1%): packed the finished logs of .*/$today/$prev into"
check "the current hour is left alone" "$(cat "$d/$today/$now/00/dddd/log" 2>/dev/null)" current

wait
settle "$d/$today/$prev"
once -u 1
check "once nothing writes, the straggler is packed too" \
    "$(tar -xzOf "$d/$today/$prev.2.tar.gz" "$prev/05/bbbb/log" 2>/dev/null)" live

finish
