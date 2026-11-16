#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: --idle detects a stalled command
#
# Goal:
#   --idle returns 125 when the command writes nothing for the interval,
#   without stopping it; a command that keeps producing output is left
#   alone; output with no trailing newline is shown when the stall is
#   reported.
#
# Procedure:
#   1. Run a command that prints, sleeps 4 seconds and prints again, with
#      --idle 1.
#   2. Run a command that prints once a second, with --idle 2.
#   3. Run a command that prints a prompt with no newline and then sleeps,
#      with --idle 0.5.
#   4. Wait and read the first command's log.
#
# Fails if:
#   - the stall is not reported with status 125 within 3 seconds, or the
#     word "stalled" is missing.
#   - the steadily printing command is reported as stalled.
#   - the unterminated prompt is not shown.
#   - the stalled command's later output is missing from its log (it was
#     stopped or lost its log).

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

t=$(date +%s)
"$bs" --log-dir "$work/c" --idle 1 -- sh -c 'echo a; sleep 4; echo b' >"$work/o3" 2>&1
check "--idle reports stall" $? 125
check "--idle returns early" "$(quick "$t" 3)" y
has "--idle says stalled" "$work/o3" 'stalled'
"$bs" --log-dir "$work/c" --idle 2 -- sh -c 'for i in 1 2 3 4; do echo $i; sleep 1; done' >/dev/null 2>&1
check "--idle lets a progressing command finish" $? 0
"$bs" --log-dir "$work/c" --idle 0.5 -- sh -c 'printf "Continue? "; sleep 3' >"$work/o3c" 2>&1
has "stall shows an unterminated prompt" "$work/o3c" '^OUT: Continue? $'
sleep 3
has "stalled command finished in background" "$(logof "$work/o3")" '^OUT: b$'

finish
