#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: the logged command line is quoted and on one line
#
# Goal:
#   The first line of the log is the command line, quoted so that it could
#   be pasted into a shell, and always a single line.
#
# Procedure:
#   1. Run a command whose arguments include a single quote, an embedded
#      newline and an empty string.
#   2. Read line 1 of the log.
#
# Fails if:
#   - the line is not exactly the expected shell-quoted text (the newline is
#     replaced by a space).

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

"$bs" start --log-dir "$work/eq" -- sh -c 'exit 0' "it's" 'two
lines' '' >"$work/h6" 2>/dev/null
check "command line is shell-quoted on one line" "$(sed -n 1p "$(logof "$work/h6")")" \
    "sh -c 'exit 0' 'it'\\''s' 'two lines' ''"

finish
