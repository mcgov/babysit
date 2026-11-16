#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: --get-child-pid and --get-child-log
#
# Goal:
#   A caller that only remembers the pid of the babysit it started can ask
#   babysit for the command's pid and its log, without knowing how the log
#   directory is laid out.
#
# Procedure:
#   1. Start babysit in the background, wait for it, and look both up by $!.
#   2. Lay out a second generation by hand in which the same babysit pid
#      appears again, and look it up once more.
#   3. Look up a pid that has no log, one whose command could not be
#      executed, and something that is not a pid; give a command as well.
#
# Fails if:
#   - the command's pid or the log path differs from what babysit itself printed.
#   - the newest generation does not win when the pid appears in two.
#   - a pid without a log does not exit 1 with nothing on stdout.
#   - a malformed lookup does not exit 2.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

"$bs" --log-dir "$work/g" -- sh -c 'echo $$' >"$work/o1" 2>/dev/null &
ppid=$!
wait "$ppid"
bbid=$(sed -n 's/^OUT: //p' "$work/o1")
check "--get-child-pid prints the command's pid" \
    "$("$bs" --log-dir "$work/g" --get-child-pid "$ppid")" "$bbid"
check "--get-child-log prints the log path" \
    "$("$bs" --log-dir "$work/g" --get-child-log "$ppid")" "$(logof "$work/o1")"
check "--get-child-log=PID form" \
    "$("$bs" --log-dir "$work/g" --get-child-log="$ppid")" "$(logof "$work/o1")"
check "both at once print the pid, then the log" \
    "$("$bs" --log-dir "$work/g" --get-child-pid "$ppid" --get-child-log "$ppid" | count)" 2

mkdir -p "$work/g/$stamp/1/$ppid/77"
: >"$work/g/$stamp/1/$ppid/77/log"
check "the newest generation wins when the pid was reused" \
    "$("$bs" --log-dir "$work/g" --get-child-pid "$ppid")" 77

out=$("$bs" --log-dir "$work/g" --get-child-pid 999999999 2>"$work/e1")
check "unknown pid exits 1" $? 1
check "unknown pid prints nothing on stdout" "$out" ""
has "unknown pid explains itself" "$work/e1" 'no log for babysit pid 999999999'

"$bs" --log-dir "$work/x" -- /nonexistent/command >/dev/null 2>&1 &
ppid=$!
wait "$ppid"
"$bs" --log-dir "$work/x" --get-child-log "$ppid" >/dev/null 2>&1
check "a command that could not be executed has no log to find" $? 1

"$bs" --log-dir "$work/g" --get-child-pid nope >/dev/null 2>&1
check "a pid that is not a number is a usage error" $? 2
"$bs" --log-dir "$work/g" --get-child-pid 1 -- true >/dev/null 2>&1
check "a lookup with a command is a usage error" $? 2
"$bs" --log-dir "$work/g" --get-child-pid >/dev/null 2>&1
check "a lookup without a pid is a usage error" $? 2

finish
