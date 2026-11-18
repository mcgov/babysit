#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: babysit follow-all shows every command's output
#
# Goal:
#   "babysit follow-all" prints the output of every command under the log
#   directory as it is written: a command already running, from the moment
#   follow-all started; a command started later, from its first line, even if
#   it exits at once. Every line has the salt of its instance in front, and
#   each instance is announced once with its full identifier. Commands that
#   had finished before follow-all started are not shown. It stops, with status
#   0, when it is interrupted.
#
# Procedure:
#   1. Run a command to its end. Start one that prints "early", and three
#      seconds later "late".
#   2. Start "babysit follow-all" in the background, its output to a file.
#   3. Start a command that prints one line on each stream and exits 4, and
#      two more at the same time that print five numbered lines each.
#   4. Wait for the first command's "late", then send follow-all SIGTERM.
#   5. Run follow-all with an argument it does not take, and on a log directory
#      that does not exist (interrupting it after a moment).
#
# Fails if:
#   - a line of a command started while following is missing, or is not
#     prefixed with that command's salt, or appears twice.
#   - the lines of one command are not in the order it wrote them.
#   - "early", or anything from the finished command, is shown.
#   - "late" is not shown, or the running command was not announced with
#     its identifier and command line.
#   - follow-all does not exit 0 on SIGTERM.
#   - a stray argument is not refused with status 2.
#
# And the two options:
#   6. --since: make the log of one finished command two hours old, then
#      run follow-all --since 1m for a moment. Logs written to in the last
#      minute must be shown from their first line, finished or not; the
#      old one must not be.
#   7. --idle: follow, with --idle 1, a command that prints "one", is
#      silent for three seconds, and prints "two". Its log is closed in
#      between; both lines must be shown, once each. The same with
#      --idle 0, which never closes a log.
#   8. Values that are no intervals are refused with status 2.
#   9. Follow a command that ends its lines with CR LF: the carriage
#      return must not be in what follow-all prints.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

L="$work/d"
st() { "$bs" start --log-dir "$L" "$@"; }

gone=$(st -- sh -c 'echo finished-before' 2>/dev/null)
old=$(st --detach -- sh -c 'echo early; sleep 3; echo late' 2>/dev/null)
sleep 0.3

"$bs" follow-all --log-dir "$L" >"$work/out" 2>"$work/err" &
fpid=$!
sleep 0.5

quick=$(st -- sh -c 'echo to-out; echo to-err >&2; exit 4' 2>/dev/null)
five='for i in 1 2 3 4 5; do echo "line $i"; done'
st -- sh -c "$five" >"$work/a" 2>/dev/null &
st -- sh -c "$five" >"$work/b" 2>/dev/null &
wait %2 %3 2>/dev/null
a=$(idof "$work/a")
b=$(idof "$work/b")

n=0
while [ "$n" -lt 80 ] && ! grep -q 'OUT: late$' "$work/out"; do
    sleep 0.1
    n=$((n + 1))
done
sleep 0.5
kill -TERM "$fpid"
wait "$fpid"
check "follow-all exits 0 when it is interrupted" $? 0
check "and says nothing on stderr" "$(count <"$work/err")" 0

s=${quick##*/}
has "a command started later is announced with its identifier" "$work/out" "^$s babysit: following $quick\$"
has "its command line is shown" "$work/out" "^$s sh -c 'echo to-out; echo to-err >&2; exit 4'\$"
has "its stdout, with the salt in front" "$work/out" "^$s OUT: to-out\$"
has "its stderr" "$work/out" "^$s ERR: to-err\$"
has "and how it ended" "$work/out" "^$s Process exited with code 4\$"

for id in "$a" "$b"; do
    s=${id##*/}
    check "five lines of $s, in order, once each" \
        "$(grep "^$s OUT: line" "$work/out" | tr '\n' ' ')" \
        "$s OUT: line 1 $s OUT: line 2 $s OUT: line 3 $s OUT: line 4 $s OUT: line 5 "
done

s=${old##*/}
has "a command already running is announced, with its command line" "$work/out" \
    "^$s babysit: following $old, already running: sh -c 'echo early; sleep 3; echo late'\$"
has "what it writes from then on is shown" "$work/out" "^$s OUT: late\$"
check "what it wrote before is not" "$(grep -c 'OUT: early' "$work/out")" 0
has "its end is shown" "$work/out" "^$s Process exited with code 0\$"
check "a command that had finished is not shown" \
    "$(grep -c -e 'finished-before' -e "^${gone##*/} " "$work/out")" 0
check "every line has a salt in front" "$(grep -c -v -E '^[a-z0-9]{8} ' "$work/out")" 0

"$bs" follow-all --log-dir "$L" stray >/dev/null 2>"$work/e2"
check "a stray argument: 2" $? 2
has "it is named" "$work/e2" "unexpected argument 'stray'"

"$bs" follow-all --log-dir "$work/nowhere" >"$work/o3" 2>&1 &
fpid=$!
sleep 0.4
st2=$("$bs" start --log-dir "$work/nowhere" -- sh -c 'echo first-ever' 2>/dev/null)
sleep 0.5
kill -INT "$fpid"
wait "$fpid"
check "a log directory that is not there yet: follow-all waits, exit 0 on SIGINT" $? 0
has "and shows the first command to use it" "$work/o3" "^${st2##*/} OUT: first-ever\$"

# ---- --since ----
touch -t "$(ago 7200)" "$L/$gone/log"
"$bs" follow-all --log-dir "$L" --since 1m >"$work/o4" 2>&1 &
fpid=$!
sleep 1
kill -TERM "$fpid"
wait "$fpid"
check "--since: exit 0 when interrupted" $? 0
s=${quick##*/}
has "--since shows a finished command's log from its first line" "$work/o4" \
    "^$s sh -c 'echo to-out; echo to-err >&2; exit 4'\$"
has "to its last" "$work/o4" "^$s Process exited with code 4\$"
check "each line once" "$(grep -c "^$s OUT: to-out\$" "$work/o4")" 1
has "and another's" "$work/o4" "^${old##*/} OUT: early\$"
check "a log not written to in that time is not shown" \
    "$(grep -c -e 'finished-before' -e "^${gone##*/} " "$work/o4")" 0

# ---- --idle ----
for idle in 1 0; do
    "$bs" follow-all --log-dir "$work/i$idle" --idle "$idle" >"$work/o5" 2>&1 &
    fpid=$!
    sleep 0.3
    "$bs" start --log-dir "$work/i$idle" -- sh -c 'echo one; sleep 3; echo two' >/dev/null 2>&1
    sleep 1.5
    kill -TERM "$fpid"
    wait "$fpid"
    check "--idle $idle: what came before the silence, and after it, once each" \
        "$(grep -c 'OUT: one$' "$work/o5") $(grep -c 'OUT: two$' "$work/o5") $(grep -c 'Process exited with code 0$' "$work/o5")" \
        "1 1 1"
done

"$bs" follow-all --log-dir "$work/cr" >"$work/o8" 2>&1 &
fpid=$!
sleep 0.3
"$bs" start --log-dir "$work/cr" -- sh -c 'printf "dos line\r\nplain\ntwo\r\r\n"' >"$work/o8id" 2>/dev/null
sleep 0.5
kill -TERM "$fpid"
wait "$fpid"
has "the log itself keeps the carriage return" "$(logof "$work/o8id")" "$(printf 'OUT: dos line\r$')"
has "follow-all leaves it out" "$work/o8" ' OUT: dos line$'
has "however many there are" "$work/o8" ' OUT: two$'
check "none is printed" "$(tr -d -c '\r' <"$work/o8" | wc -c | tr -d ' ')" 0

"$bs" follow-all --log-dir "$L" --idle soon >/dev/null 2>"$work/e6"
check "--idle that is no interval: 2" $? 2
has "it is named" "$work/e6" "invalid --idle interval 'soon'"
"$bs" follow-all --log-dir "$L" --since 0 >/dev/null 2>"$work/e7"
check "--since that is no interval: 2" $? 2
has "it is named" "$work/e7" "invalid --since interval '0'"

finish
