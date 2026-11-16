#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: finished hours are packed when the disk fills up
#
# Goal:
#   At compact_at_disk_percent the service stops waiting for a whole day to
#   be finished: every hour of it that is over and has no writer is packed
#   in place as <day>/<HH>.tar.gz. Hours that still have a writer, and the
#   hour that is not over yet, are left alone.
#
# Procedure:
#   1. Lay out a past day with two finished hours and one in which a helper
#      holds a writer lock, so that the day as a whole cannot be archived.
#      Lay out a finished instance in today's current hour.
#   2. Run "babysitd --once" with the limit off, then with a limit of 1%,
#      which any filesystem in use exceeds.
#   3. Let the lock go and run it once more.
#
# Fails if:
#   - anything is packed while the limit is off.
#   - the two finished hours are not each packed into <HH>.tar.gz, or the
#     packed hour does not hold its log.
#   - the hour with a writer, or today's current hour, is packed.
#   - the day, packed hours included, is not archived once nothing writes.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

d="$work/ph"
plant "$d" 20230101/03/04/aaaa three
plant "$d" 20230101/05/06/bbbb five
plant "$d" 20230101/07/08/cccc live
plant "$d" "$today/$(date +%H)/00/dddd" now
"$lp" --hold 4 "$d/20230101/07/08/cccc/log" &
sleep 0.5

"$bsd" --once --log-dir "$d" --config /dev/null -u 0 2>"$work/ph1"
check "nothing is packed while the limit is off" "$(ls "$d/20230101" | tr '\n' ' ')" "03 05 07 "

"$bsd" --once --log-dir "$d" --config /dev/null -u 1 2>"$work/ph2"
check "finished hours are packed, the one with a writer is not" \
    "$(ls "$d/20230101" | tr '\n' ' ')" "03.tar.gz 05.tar.gz 07 "
check "a packed hour holds its log" \
    "$(tar -xzOf "$d/20230101/03.tar.gz" 03/04/aaaa/log 2>/dev/null)" three
has "packing is reported with the reason" "$work/ph2" 'full (limit 1%): packed .*/20230101/03 into'
check "today's current hour is left alone" "$(cat "$d/$today/$(date +%H)/00/dddd/log" 2>/dev/null)" now

wait
settle "$d/20230101"
"$bsd" --once --log-dir "$d" --config /dev/null -u 1 2>"$work/ph3"
check "once nothing writes, the whole day goes to the archive" \
    "$(tar -tzf "$d/archive/babysit_logs_20230101.tar.gz" | grep -c -e '03.tar.gz$' -e '05.tar.gz$' -e 'cccc/log$')" 3

finish
