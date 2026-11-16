#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: babysit: a log directory that cannot be created
#
# Goal:
#   If the log cannot be created, babysit reports it and exits 2 without
#   running the command.
#
# Procedure:
#   1. Point --log-dir at a path below a regular file.
#
# Fails if:
#   - the exit status is not 2, or the error does not say the log could not
#     be created.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

: >"$work/notdir"
"$bs" --log-dir "$work/notdir/x" -- true >/dev/null 2>"$work/h7"
check "uncreatable log directory is an error" $? 2
has "uncreatable log directory is explained" "$work/h7" 'cannot create log'

finish
