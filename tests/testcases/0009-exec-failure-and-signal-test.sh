#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: commands that cannot run, and commands killed by a signal
#
# Goal:
#   A command that does not exist gives 127, one that is not executable
#   gives 126, and a command killed by a signal gives 128 plus the signal
#   number. A command that could not be executed still gets an instance:
#   its identifier is printed, and its log and exit_code say what happened.
#
# Procedure:
#   1. Run a path that does not exist.
#   2. Run an empty file that has no execute permission.
#   3. Run a command that kills itself with SIGKILL.
#
# Fails if:
#   - the statuses are not 127, 126 and 137.
#   - no identifier is printed for a command that could not be executed.
#   - its exit_code file does not hold the same status, or its log does not
#     say that it could not be executed.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

"$bs" start --log-dir "$work/f" -- /no/such/program >"$work/m1" 2>"$work/m1e"
check "missing command exits 127" $? 127
has "the identifier is printed all the same" "$work/m1" '^[0-9]\{8\}/[0-9][0-9]/[0-9][0-9]/....$'
has "the error names the instance" "$work/m1e" "$(cat "$work/m1"): cannot execute"
check "exit_code records 127" "$(cat "$work/f/$(cat "$work/m1")/exit_code")" 127
has "the log says why" "$work/f/$(cat "$work/m1")/log" '^babysit: cannot execute: '
"$bs" start --log-dir "$work/f" -- sh -c 'kill -9 $$' >"$work/m2" 2>/dev/null
check "signal death is 128+sig" $? 137
check "exit_code records it the same way" "$(cat "$work/f/$(cat "$work/m2")/exit_code")" 137
: >"$work/noexec"
"$bs" start --log-dir "$work/f" -- "$work/noexec" >"$work/m3" 2>/dev/null
check "non-executable command exits 126" $? 126
check "exit_code records 126" "$(cat "$work/f/$(cat "$work/m3")/exit_code")" 126

finish
