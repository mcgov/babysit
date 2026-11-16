#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: a grandchild that outlives the command
#
# Goal:
#   When the command exits while one of its own children still holds its
#   stdout, babysit reports the exit at once and the grandchild's later
#   output still reaches the log.
#
# Procedure:
#   1. Run a command that starts a background child (sleep 2, then print) and
#      exits immediately.
#   2. Time babysit, then wait and read the end of the log.
#
# Fails if:
#   - babysit does not return 0 within 2 seconds (it waited for the
#     grandchild).
#   - the log does not end with the exit line followed by the grandchild's
#     output.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

t=$(date +%s)
"$bs" start --log-dir "$work/oh" -- sh -c '(sleep 2; echo late) & echo early' >"$work/h11" 2>/dev/null
check "exit is reported while a grandchild holds the pipes" "$?$(quick "$t" 2)" 0y
sleep 3
check "a grandchild's later output still reaches the log" \
    "$(tail -n 2 "$(logof "$work/h11")" | tr '\n' '|')" "Process exited with code 0|OUT: late|"

finish
