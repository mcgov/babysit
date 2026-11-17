#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: info and check
#
# Goal:
#   The identifier is enough to ask about an instance: where its log is,
#   whether its command is running, its pid while it is, and its exit
#   status once it is not. A pid is never given out for a command that has
#   exited: by then the number may belong to something else.
#
# Procedure:
#   1. Start a command that prints its own pid and keeps running; ask.
#   2. Kill the command; ask again.
#   3. Kill the babysitter of another instance, so that no exit status is
#      ever recorded; ask about that one.
#   4. Ask about an identifier that has no instance, and about things that
#      are not identifiers.
#
# Fails if:
#   - check does not say "running" and exit 0 while the command runs, or
#     "exited N" and exit 1 afterwards.
#   - info --pid differs from the real pid, or still prints one afterwards.
#   - info --exit-code prints anything before the command has exited, or
#     the wrong status after.
#   - an instance whose babysitter died is not reported as lost.
#   - an unknown or malformed identifier does not exit 2.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

i() { "$bs" info --log-dir "$work/g" "$@"; }
c() { "$bs" check --log-dir "$work/g" "$@"; }

id=$("$bs" start --log-dir "$work/g" --expect . -- sh -c 'echo $$; exec sleep 30' 2>"$work/e1")
cpid=$("$bs" cat --log-dir "$work/g" stdout "$id")
check "check: running, exit 0" "$(c "$id"; echo $?)" "running
0"
check "status is another name for check" "$("$bs" status --log-dir "$work/g" "$id")" running
check "info --pid prints the pid of the running command" "$(i --pid "$id")" "$cpid"
check "info --log prints the log path" "$(i --log "$id")" "$work/g/$id/log"
check "options may follow the identifier" "$(i "$id" --log)" "$work/g/$id/log"
out=$(i --exit-code "$id" 2>/dev/null)
check "info --exit-code: nothing while it runs, exit 1" "$out $?" " 1"
check "info alone shows the lot" "$(i "$id" | tr '\n' '|')" "log: $work/g/$id/log|state: running|pid: $cpid|"

kill "$cpid"; sleep 0.5
check "check: exited with the status, exit 1" "$(c "$id"; echo $?)" "exited 143
1"
out=$(i --pid "$id" 2>"$work/e2")
check "info --pid: no pid once the command is gone, exit 1" "$out $?" " 1"
has "and says why" "$work/e2" 'is not running'
check "info --exit-code prints the status" "$(i --exit-code "$id")" 143
check "the exit_code file holds the same" "$(cat "$work/g/$id/exit_code")" 143
check "info --log still works" "$(i --log "$id")" "$work/g/$id/log"

id2=$("$bs" start --log-dir "$work/g" --detach -- sleep 30 2>/dev/null)
cpid2=$(i --pid "$id2")
kill -9 "$(ps -o ppid= -p "$cpid2" | tr -d ' ')"; sleep 0.3
check "an instance whose babysitter was killed is lost" "$(c "$id2"; echo $?)" "lost
1"
kill "$cpid2" 2>/dev/null

c 20200101/00/00/abcd >/dev/null 2>"$work/e3"
check "no such instance: exit 2" $? 2
has "and it is named" "$work/e3" 'no instance 20200101/00/00/abcd'
for bad in 12345 ../../etc/passwd 20200101/00/00/abcd/x "$id/.." 20200101-00-00-abcd; do
    i --log "$bad" >/dev/null 2>&1
    check "'$bad' is not an identifier: exit 2" $? 2
done
i --pid --log "$id" >/dev/null 2>&1
check "two selectors at once is a usage error" $? 2
c >/dev/null 2>&1
check "check without an identifier is a usage error" $? 2

finish
