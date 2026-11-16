#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: --expect returns when a line matches
#
# Goal:
#   --expect returns 0 as soon as an output line matches, leaves the command
#   running, returns 1 if the command exits 0 without a match, and with
#   --fixed matches the pattern literally.
#
# Procedure:
#   1. Run a command that prints "ready" after 1 second and "done" 2 seconds
#      later, with --expect '^ready$'.
#   2. Run a command that never prints the expected text.
#   3. Run a command whose output contains regex metacharacters, with
#      --fixed.
#
# Fails if:
#   - babysit does not return 0 within 3 seconds of starting, or the
#     matching line is not echoed.
#   - the output written after babysit returned is missing from the log.
#   - a clean exit without a match does not give status 1.
#   - the --fixed pattern is treated as a regular expression.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

t=$(date +%s)
"$bs" start --log-dir "$work/d" --expect '^ready$' -- sh -c 'echo boot; sleep 1; echo ready; sleep 2; echo done' >"$work/o4" 2>&1
check "--expect matched" $? 0
check "--expect returns early" "$(quick "$t" 3)" y
has "--expect echoes the line" "$work/o4" '^OUT: ready$'
sleep 3
has "expected command finished in background" "$(logof "$work/o4")" '^OUT: done$'
"$bs" start --log-dir "$work/d" --expect never -- sh -c 'echo x' >/dev/null 2>&1
check "--expect without match, exit 0 -> 1" $? 1
"$bs" start --log-dir "$work/d" --fixed --expect '[::]:80' -- sh -c 'echo "on [::]:80"; sleep 1' >/dev/null 2>&1
check "--fixed matches literally" $? 0

finish
