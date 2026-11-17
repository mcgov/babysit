#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: wait
#
# Goal:
#   "babysit wait ID" blocks, without printing anything, until the command
#   has exited, and returns its exit status. With --timeout it returns 124
#   if the command is still running when the time is up, and leaves it
#   running. It is the quiet half of "start --expect ..., then wait".
#
# Procedure:
#   1. Start a command that prints "ready", works for a second and a half
#      in silence and exits 7; wait for it; wait for it again.
#   2. Start one that sleeps for thirty seconds; wait half a second for it;
#      then wait for it in the background and stop it with a signal.
#   3. Wait for an instance whose babysitter has been killed, for one that
#      does not exist, and with a bad time.
#
# Fails if:
#   - wait returns before the command has exited, returns anything but its
#     exit status, or prints anything.
#   - waiting for a finished instance does not return its status at once.
#   - --timeout does not return 124 on time, or stops the command.
#   - a wait in progress does not end when the command does.
#   - a lost or missing instance, or a bad time, does not exit 2.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

wt() { "$bs" wait --log-dir "$work/w" "$@"; }

id=$("$bs" start --log-dir "$work/w" --expect ready -- sh -c 'echo ready; sleep 1.5; exit 7' 2>/dev/null)
t=$(date +%s)
out=$(wt "$id" 2>&1)
check "wait returns the command's exit status" $? 7
check "after the command has finished, not before" "$([ "$(elapsed "$t")" -ge 1 ] && echo y)" y
check "and prints nothing" "$out" ""
t=$(date +%s)
wt "$id"
check "waiting for a finished instance: its status, at once" "$? $(quick "$t" 1)" "7 y"

id=$("$bs" start --log-dir "$work/w" --detach -- sleep 30 2>/dev/null)
t=$(date +%s)
wt "$id" --timeout 0.5 >"$work/o1" 2>"$work/e1"
check "--timeout: 124, on time" "$? $(quick "$t" 3)" "124 y"
check "nothing on stdout" "$(count <"$work/o1")" 0
has "the reason on stderr" "$work/e1" 'still running after 0.5'
check "the command is left running" "$("$bs" check --log-dir "$work/w" "$id")" running
wt "$id" >"$work/rc" 2>&1 &
waiter=$!
sleep 0.5
"$bs" signal --log-dir "$work/w" TERM "$id"
wait "$waiter"
check "a wait in progress ends when the command does, with its status" $? 143

id=$("$bs" start --log-dir "$work/w" --detach -- sleep 30 2>/dev/null)
cpid=$("$bs" info --log-dir "$work/w" --pid "$id")
kill -9 "$(ps -o ppid= -p "$cpid" | tr -d ' ')"; sleep 0.3
wt "$id" 2>"$work/e2"
check "an instance whose babysitter was killed: exit 2" $? 2
has "which is explained" "$work/e2" 'left no exit status'
kill "$cpid" 2>/dev/null

wt 20200101/00/00/abcd 2>/dev/null
check "no such instance: exit 2" $? 2
wt "$id" --timeout soon 2>/dev/null
check "a bad time: exit 2" $? 2
wt "$id" --idle 5 2>/dev/null
check "wait takes no --idle: exit 2" $? 2

finish
