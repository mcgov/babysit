#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: the log can no longer be written
#
# Goal:
#   When the log stops taking writes while the command runs (full disk,
#   file size limit), the command is not disturbed, the caller still sees
#   its output and exit status, and is told that the log is incomplete.
#
# Procedure:
#   1. Run babysit under a file size limit of one block, for a command that
#      writes 200 lines and exits 7. babysit's stdout is a pipe, so only the
#      log runs into the limit.
#
# Fails if:
#   - babysit does not return the command's exit status (the babysitter
#     being killed by the limit shows up as status 2).
#   - any of the 200 lines is missing from babysit's output.
#   - there is no warning, or more than one.
#   - the log grew past the limit.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

(
    ulimit -f 1
    "$bs" --log-dir "$work/lw" -- \
        sh -c 'i=0; while [ $i -lt 200 ]; do echo "line $i of output"; i=$((i+1)); done; exit 7' 2>/dev/null
    echo "$?" >&3
) 3>"$work/lw.rc" | cat >"$work/lw.out"
check "the command's exit status is still reported" "$(cat "$work/lw.rc")" 7
check "all of its output still reaches the caller" "$(grep -c '^OUT: line ' "$work/lw.out")" 200
check "the caller is warned once" "$(grep -c 'warning: cannot write to the log' "$work/lw.out")" 1
log=$(logof "$work/lw.out")
check "the log stopped at the limit" "$([ "$(wc -c <"$log")" -le 1024 ] && echo y)" y

finish
