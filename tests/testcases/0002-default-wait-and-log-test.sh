#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: default mode waits, echoes and logs
#
# Goal:
#   With no options babysit waits for the command, returns its exit status,
#   echoes its output and writes the same text to the log.
#
# Procedure:
#   1. Run a command that writes one line to stdout, one to stderr, and exits
#      3.
#   2. Compare babysit's exit status, its stdout and stderr, and the log
#      file.
#
# Fails if:
#   - the exit status is not the command's (3).
#   - the log is not at <log-dir>/<identifier>/log.
#   - the log does not start with the command line and the "Process created"
#     line, or lacks the OUT:/ERR: lines or the final exit line.
#   - anything but babysit's own message is on stderr, or "babysit cat"
#     does not give the output back.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

"$bs" start --log-dir "$work/a" -- sh -c 'echo hi; echo oops >&2; exit 3' >"$work/o1" 2>"$work/e1"
check "default returns the exit code" $? 3
log=$(logof "$work/o1")
check "log path is dir/identifier/log" "$log" "$work/a/$(cat "$work/o1")/log"
check "log line 1 is the command line" "$(sed -n 1p "$log")" \
    "sh -c 'echo hi; echo oops >&2; exit 3'"
has "log has pid line" "$log" '^Process created with pid [0-9]'
has "log has stdout" "$log" '^OUT: hi$'
has "log has stderr" "$log" '^ERR: oops$'
has "log has exit code" "$log" '^Process exited with code 3$'
check "stderr carries only babysit's own message" "$(grep -vc '^babysit: ' "$work/e1")" 0
has "exit is reported on stderr" "$work/e1" 'exited with code 3'
check "cat gives the output back" "$("$bs" cat --log-dir "$work/a" "$(cat "$work/o1")")" "hi
oops"

finish
