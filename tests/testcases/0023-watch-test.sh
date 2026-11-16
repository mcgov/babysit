#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: watch
#
# Goal:
#   "babysit watch ID" attaches to an instance that was started earlier and
#   returns as "start" would have: on an expected line, on silence, after a
#   time limit, or with the command's exit status. It reads the log from
#   the beginning, so a line printed before it attached still counts, and
#   it never stops the command unless --onfail says so.
#
# Procedure:
#   1. Start a command detached that prints "early" at once, "late" after
#      two seconds, and then stays silent.
#   2. Watch it for "early" (already in the log), for "late" (not yet), with
#      a one-second timeout, and for one second of silence.
#   3. Watch it again for silence with --onfail TERM.
#   4. Watch a finished instance with no options, and with an expectation
#      that was never met.
#
# Fails if:
#   - a line already in the log does not satisfy --expect.
#   - the statuses are not 0, 0, 124, 125 and 125.
#   - the command is not running after the first four, or is running after
#     the one with --onfail.
#   - lines that appear while watching are not echoed on stderr.
#   - watching a finished instance does not return its exit status, or 1
#     when it exited 0 without the expected line.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

wt() { "$bs" watch --log-dir "$work/w" "$@"; }

id=$("$bs" start --log-dir "$work/w" --detach -- sh -c 'echo early; sleep 2; echo late; sleep 30' 2>/dev/null)
sleep 0.3
t=$(date +%s)
wt "$id" --expect early 2>/dev/null
check "a line already in the log satisfies --expect, at once" "$? $(quick "$t" 2)" "0 y"
wt "$id" --timeout 0.5 2>"$work/w1"
check "--timeout: 124" $? 124
wt "$id" --expect '^late$' --timeout 10 2>"$work/w2"
check "a line that arrives while watching: 0" $? 0
has "and it is echoed on stderr" "$work/w2" '^OUT: late$'
check "what was already there is not echoed" "$(grep -c '^OUT: early$' "$work/w2")" 0
wt --idle 1 "$id" 2>"$work/w3"
check "--idle: 125" $? 125
check "the command is still running after all that" "$("$bs" check --log-dir "$work/w" "$id")" running
wt "$id" --idle 1 --onfail TERM 2>"$work/w4"
check "--idle with --onfail: 125 again" $? 125
has "the signal is reported" "$work/w4" 'sent TERM to the command'
sleep 0.5
check "and this time the command was stopped" "$("$bs" check --log-dir "$work/w" "$id")" "exited 143"

wt "$id" 2>/dev/null
check "watching a finished instance returns its status" $? 143
id2=$("$bs" start --log-dir "$work/w" -- sh -c 'echo x' 2>/dev/null)
wt "$id2" --expect never 2>"$work/w5"
check "finished with status 0 and no match: 1" $? 1
has "which is explained" "$work/w5" 'before the expected output'
id3=$("$bs" start --log-dir "$work/w" --detach -- sh -c 'sleep 1; exit 7' 2>/dev/null)
wt "$id3" 2>/dev/null
check "with no options, watch waits for the command and returns its status" $? 7

wt 20200101/00/00/abcd 2>/dev/null
check "no such instance: 2" $? 2
wt "$id" --onfail NOPE 2>/dev/null
check "unknown --onfail signal: 2" $? 2
wt "$id" --idle soon 2>/dev/null
check "bad interval: 2" $? 2

finish
