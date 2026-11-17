#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: the log can no longer be written
#
# Goal:
#   When the log stops taking writes while the command runs (full disk,
#   file size limit), the command is not disturbed, the caller still gets
#   its exit status, and is told that the log is incomplete.
#
# Procedure:
#   1. Run babysit under a file size limit of 2 MB, for a command that
#      writes 3 MB in some 3000 lines and exits 7. babysit's stderr is a pipe, so
#      only the log runs into the limit. (The limit is no smaller because a
#      coverage build writes its profile data under it too.)
#
# Fails if:
#   - babysit does not return the command's exit status (the babysitter
#     being killed by the limit shows up as status 2).
#   - there is no warning, or more than one.
#   - the log grew past the limit.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

(
    ulimit -f 2048
    "$bs" start --log-dir "$work/lw" -- \
        sh -c 'head -c 3000000 /dev/zero | tr "\0" x | fold -w 999; exit 7' 2>&1 >"$work/lw.id"
    echo "$?" >&3
) 3>"$work/lw.rc" | cat >"$work/lw.out"
check "the command's exit status is still reported" "$(cat "$work/lw.rc")" 7
check "the caller is warned once" "$(grep -c 'warning: cannot write to the log' "$work/lw.out")" 1
log=$(logof "$work/lw.id")
check "the log stopped at the limit" "$([ "$(wc -c <"$log")" -le 2097152 ] && echo y)" y

finish
