#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: several --expect patterns, seen in order
#
# Goal:
#   When --expect is given more than once, start and watch return 0 only
#   when every pattern has been seen, in the order given. Each pattern is
#   looked for from where the one before it matched, so text that came
#   earlier does not count, and one line can satisfy two patterns. If the
#   command ends first, its status is returned (1 if that was 0), with a
#   message saying how many patterns were seen. If the last pattern is seen
#   in the final output of a command that then fails, the status is still 0.
#
# Procedure:
#   1. Start a command that prints "one", "two", "three" a moment apart,
#      expecting "one" then "three"; it sleeps afterwards.
#   2. Start a command that prints "b" then "a" and exits 0, expecting "a"
#      then "b". The same, exiting 5.
#   3. Start a command that prints "alpha beta", expecting "alpha" then
#      "beta"; then "beta" then "alpha"; then "a" twice against a line with
#      one "a" and against "aa".
#   4. Expect "^x" twice from a command that prints "xx", and from one that
#      prints "x" on two lines.
#   5. With --fixed, expect "a.c" then "[d]" from "a.c [d]", and from "abc d".
#   6. Start a command that prints "one", then "two" and exits 3 at once,
#      expecting "one" then "two".
#   7. Expect "one" then "never" with --timeout from a command that goes on.
#   8. Give a bad pattern second.
#   9. watch: the same expectations against a log that already holds the
#      first line, and against a finished instance.
#
# Fails if:
#   - start or watch returns 0 before the last pattern, or does not return
#     0 when the patterns appear in order.
#   - patterns that appear in the wrong order are accepted.
#   - the status or the message for an early exit is wrong.
#   - a bad pattern is not refused with status 2 and named.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

st() { "$bs" start --log-dir "$work/d" "$@"; }
wt() { "$bs" watch --log-dir "$work/d" "$@"; }

t=$(date +%s)
st -e one -e three -- sh -c 'echo one; sleep 1; echo two; sleep 1; echo three; sleep 3; echo done' >"$work/o1" 2>"$work/e1"
check "both patterns seen in order: 0" $? 0
check "and not before the second" "$([ "$(elapsed "$t")" -ge 2 ] && echo y)" y
has "the last matching line is in the log by then" "$(logof "$work/o1")" '^OUT: three$'
check "the command had not finished" "$(grep -c '^OUT: done$' "$(logof "$work/o1")")" 0

st -e a -e b -- sh -c 'echo b; echo a' >/dev/null 2>"$work/e2"
check "patterns seen in the wrong order, exit 0: 1" $? 1
has "the message says how far it got" "$work/e2" 'before the expected output (1 of 2 patterns seen)'
st -e a -e b -- sh -c 'echo b; echo a; exit 5' >/dev/null 2>"$work/e3"
check "the same, exit 5: 5" $? 5
has "and says so too" "$work/e3" 'exited with code 5 before the expected output (1 of 2 patterns seen)'
st -e a -e b -- sh -c 'echo c' >/dev/null 2>"$work/e4"
check "neither seen: 1" $? 1
has "none of two" "$work/e4" '(0 of 2 patterns seen)'

st -e alpha -e beta -- sh -c 'echo alpha beta; sleep 1' >/dev/null 2>&1
check "one line satisfies two patterns in order" $? 0
st -e beta -e alpha -- sh -c 'echo alpha beta' >/dev/null 2>&1
check "but not out of order" $? 1
st -e a -e a -- sh -c 'echo a' >/dev/null 2>&1
check "the same pattern twice needs two matches" $? 1
st -e a -e a -- sh -c 'echo aa; sleep 1' >/dev/null 2>&1
check "which one line can hold" $? 0
st -e a -e a -- sh -c 'echo a; echo a; sleep 1' >/dev/null 2>&1
check "or two lines" $? 0

st -e '^x' -e '^x' -- sh -c 'echo xx' >/dev/null 2>&1
check "^ matches only at the start of a line, not after a match" $? 1
st -e '^x' -e '^x' -- sh -c 'echo x; echo x; sleep 1' >/dev/null 2>&1
check "and does on the next line" $? 0

st -F -e 'a.c' -e '[d]' -- sh -c 'echo "a.c [d]"; sleep 1' >/dev/null 2>&1
check "--fixed applies to every pattern" $? 0
st -F -e 'a.c' -e '[d]' -- sh -c 'echo "abc d"' >/dev/null 2>&1
check "and none is then a regular expression" $? 1

st -e one -e two -- sh -c 'echo one; echo two; exit 3' >/dev/null 2>"$work/e6"
check "all seen in the last output of a command that fails: 0" $? 0
has "reported as seen" "$work/e6" 'expected output seen'

st -e one -e never --timeout 1 -- sh -c 'echo one; sleep 5' >/dev/null 2>"$work/e7"
check "time runs out with one pattern to go: 124" $? 124
has "the message says how far it got" "$work/e7" 'timed out after 1; .*; 1 of 2 expected patterns seen'
st -e never --timeout 1 -- sh -c 'sleep 5' >/dev/null 2>"$work/e7b"
check "one pattern: the message is as it was" "$(grep -c 'patterns seen' "$work/e7b")" 0

st -e fine -e '(' -- true >"$work/o8" 2>"$work/e8"
check "a bad second pattern: 2" $? 2
has "it is named" "$work/e8" "bad --expect pattern '('"
check "and nothing was started" "$(count <"$work/o8")" 0

id=$(st --detach -- sh -c 'echo one; sleep 2; echo two; sleep 30' 2>/dev/null)
sleep 0.3
wt "$id" -e two -e one --timeout 4 >/dev/null 2>"$work/e9"
check "watch: the wrong order is not accepted: 124" $? 124
has "watch says how far it got" "$work/e9" '1 of 2 expected patterns seen'
wt "$id" -e one -e two --timeout 10 >/dev/null 2>&1
check "watch: one line from before and one from now, in order: 0" $? 0
"$bs" signal --log-dir "$work/d" TERM "$id" >/dev/null 2>&1
id2=$(st -- sh -c 'echo one; echo two' 2>/dev/null)
wt "$id2" -e one -e two 2>/dev/null
check "watch on a finished instance, all there: 0" $? 0
wt "$id2" -e two -e one 2>"$work/e10"
check "watch on a finished instance, wrong order: 1" $? 1
has "which is explained" "$work/e10" 'before the expected output (1 of 2 patterns seen)'

finish
