#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: a caller that has stopped listening
#
# Goal:
#   babysit owes its caller an exit status whatever has become of the
#   caller's stderr. If nobody reads it any more, the message saying why
#   babysit returned is lost, and nothing else: babysit is not killed by
#   SIGPIPE, the command is not disturbed, and the log is complete.
#
# Procedure:
#   1. Run babysit with its stderr on a pipe whose reader exits at once, for
#      a command that prints over a second and exits 7.
#   2. Read the log.
#
# Fails if:
#   - babysit's own exit status in the pipeline is not the command's.
#   - the command's last line is missing from the log.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

"$bs" start --log-dir "$work/oh" -- sh -c 'echo a; sleep 1; echo c; exit 7' 2>&1 >"$work/h12" | true
check "a caller that is not listening still gets the exit status" "${PIPESTATUS[0]}" 7
has "and the command ran to its end" "$(logof "$work/h12")" '^OUT: c$'

finish
