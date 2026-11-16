#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: a caller that stops reading
#
# Goal:
#   If the reader of the output babysit echoes on stderr goes away, babysit returns 0 rather
#   than failing, and the command keeps running and logging.
#
# Procedure:
#   1. Pipe babysit into "head -n 1" for a command that prints over 2
#      seconds.
#   2. Wait and read the log.
#
# Fails if:
#   - babysit's own exit status in the pipeline is not 0.
#   - the command's last line is missing from the log.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

"$bs" start --log-dir "$work/oh" -- sh -c 'echo a; sleep 1; echo b; sleep 1; echo c' 2>&1 >"$work/h12" | head -n 1 >/dev/null
check "caller that stops reading lets babysit return 0" "${PIPESTATUS[0]}" 0
sleep 2
has "command keeps logging after the caller stopped reading" "$(logof "$work/h12")" '^OUT: c$'

finish
