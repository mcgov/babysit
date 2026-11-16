#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: a log is locked exactly while it is being written
#
# Goal:
#   The babysitter holds a write lock on its log for as long as it is
#   logging, and the kernel releases it when the babysitter goes away for
#   any reason. babysitd relies on this to know which logs are safe to pack
#   or delete. The check uses lockprobe, which calls the same function
#   babysitd does.
#
# Procedure:
#   1. Start a 2-second command with --detach and probe its log while it runs
#      and after it ends.
#   2. Start a 30-second and a 2-second command; find the first one's
#      babysitter (the command's parent) and kill it with SIGKILL.
#   3. Probe both logs, check the first command is still alive, then stop it.
#   4. Probe a path that does not exist.
#
# Fails if:
#   - a log is unlocked while its command runs, or locked after it finished.
#   - the lock survives the babysitter being killed, which would mean the
#     command inherited it.
#   - killing one babysitter releases another log's lock.
#   - a missing file is reported as locked.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

"$bs" --log-dir "$work/lk" --detach -- sleep 2 >"$work/l1" 2>/dev/null
check "lock: held while the command runs, after babysit returned" "$("$lp" "$(logof "$work/l1")")" locked
sleep 3
check "lock: released when the command finishes" "$("$lp" "$(logof "$work/l1")")" unlocked
"$bs" --log-dir "$work/lk" --detach -- sleep 30 >"$work/l2" 2>/dev/null
"$bs" --log-dir "$work/lk" --detach -- sleep 2 >"$work/l3" 2>/dev/null
cpid=$(basename "$(dirname "$(logof "$work/l2")")")
sitter=$(ps -o ppid= -p "$cpid" | tr -d ' ')
kill -9 "$sitter"; sleep 0.3
check "lock: released when the babysitter is killed" "$("$lp" "$(logof "$work/l2")")" unlocked
check "lock: not inherited by the command" "$(kill -0 "$cpid" 2>/dev/null && echo alive)" alive
check "lock: each log has its own" "$("$lp" "$(logof "$work/l3")")" locked
kill "$cpid"
check "lock: a missing log counts as unlocked" "$("$lp" "$work/lk/none")" unlocked

finish
