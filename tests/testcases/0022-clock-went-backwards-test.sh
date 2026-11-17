#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: records dated later than now
#
# Goal:
#   When the log directory holds records dated later than now, the clock
#   has been set back. babysit then starts nothing and exits 75, "try
#   again", and it does not pack anything itself: that is left to babysitd,
#   which packs the later-dated records that are finished. A running
#   babysitd is woken by the refusal, so that a retry succeeds unaided.
#
# Procedure:
#   1. Plant a finished instance dated in the year 2099 and run babysit;
#      then "babysitd --once"; then babysit again.
#   2. The same for one dated today at 23:59, unless it is 23:59.
#   3. Plant a later-dated instance whose log has a writer, and run babysit
#      while it is written and after the writer has gone.
#   4. With babysitd running as a service, plant another, run babysit, wait
#      a moment and run it again.
#
# Fails if:
#   - babysit starts the command, or prints an identifier, while a
#     later-dated record is there, or exits with anything but 75.
#   - babysit removes or packs anything itself.
#   - looking up an existing instance is refused as well.
#   - babysit does not run normally once babysitd has packed the record.
#   - a later-dated instance that is still being written makes babysit
#     refuse, or is packed by babysitd.
#   - it is not treated like any other later-dated record once finished.
#   - the running service is not woken by the refusal.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

d="$work/cb"
b() { "$bs" start --log-dir "$d" "$@"; }
once() { "$bsd" --once --log-dir "$d" --config /dev/null -u 0 2>>"$work/cb.err"; }

first=$(b -- true 2>/dev/null)
plant "$d" 20991231/01/02/aaaa future
out=$(b -- sh -c 'echo ran >"$0"' "$work/ran" 2>"$work/e1")
check "later-dated day: exit 75" $? 75
check "nothing on stdout" "$out" ""
check "the command was not started" "$([ -e "$work/ran" ] && echo ran || echo no)" no
has "the reason and the next step are given" "$work/e1" 'clock has gone backwards.*20991231'
has "babysitd is named as the next step" "$work/e1" 'babysitd --once'
check "babysit packed nothing itself" "$(cat "$d/20991231/01/02/aaaa/log")" future
check "lookups are not refused" "$("$bs" info --log-dir "$d" --log "$first")" "$d/$first/log"
once
check "babysitd archives the later-dated day" \
    "$(tar -xzOf "$d/archive/babysit_logs_20991231.tar.gz" 20991231/01/02/aaaa/log 2>/dev/null)" future
b -- true >/dev/null 2>&1
check "after which babysit runs again" $? 0

if [ "$(date +%H%M)" != 2359 ]; then
    plant "$d" "$today/23/59/bbbb" tonight
    b -- true >/dev/null 2>"$work/e2"
    check "later today: exit 75" $? 75
    has "the later record is named" "$work/e2" "$today/23"
    once
    check "babysitd packs it in place" \
        "$(find "$d/$today" -name '*.tar.gz' -exec tar -xzOf {} \; 2>/dev/null)" tonight
    b -- true >/dev/null 2>&1
    check "after which babysit runs again" $? 0
fi

plant "$d" 20991230/03/04/cccc busy
"$lp" --hold 3 "$d/20991230/03/04/cccc/log" &
sleep 0.5
b -- true >/dev/null 2>&1
check "a later-dated instance still being written does not hold up a new one" $? 0
once
check "and babysitd leaves it where it is" "$(cat "$d/20991230/03/04/cccc/log")" busy
wait
b -- true >/dev/null 2>&1
check "once it has finished it is in the way like any other: 75" $? 75
once
b -- true >/dev/null 2>&1
check "and once packed, babysit runs again" $? 0

"$bsd" --log-dir "$d" --config /dev/null -u 0 2>"$work/cb.svc" &
sv=$!
sleep 0.5
plant "$d" 20991229/05/06/dddd again
b -- true >/dev/null 2>&1
check "with the service running: 75 at first" $? 75
sleep 1.5
b -- true >/dev/null 2>&1
check "and 0 on the retry, with nobody running babysitd by hand" $? 0
kill "$sv"; wait "$sv"
has "the refusal woke the service" "$work/cb.svc" 'woken by a request'

finish
