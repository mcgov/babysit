#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: the command exits before any watch fires
#
# Goal:
#   With --idle, --timeout and --expect all set, a command that simply exits
#   is reported with its own status and a message saying the expected output
#   never came.
#
# Procedure:
#   1. Run a command that prints one line and exits 9, with all three options
#      set to values it will not reach.
#
# Fails if:
#   - the exit status is not 9.
#   - stderr does not say the command exited before the expected output.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

"$bs" start --log-dir "$work/oh" --idle 5 --timeout 5 --expect never -- sh -c 'echo x; exit 9' >/dev/null 2>"$work/h13"
check "all watches armed, command exits first" $? 9
has "early exit names the unmet expectation" "$work/h13" 'exited with code 9 before the expected output'

finish
