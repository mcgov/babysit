#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: start --follow
#
# Goal:
#   "babysit start" shows nothing of the command's output unless --follow
#   is given. With it, the log is shown on stderr as it grows, for as long
#   as start is waiting, and stdout is still the identifier and nothing
#   else.
#
# Procedure:
#   1. Run a command that writes to both streams and exits 3, with --follow.
#   2. Run one that prints a line, an expected line, and later a third,
#      with --follow and --expect.
#
# Fails if:
#   - stdout is anything but the identifier.
#   - the output, labelled as in the log, is missing from stderr, or the
#     exit status is not the command's.
#   - with --expect, what was printed after start returned is shown.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

"$bs" start --log-dir "$work/f" --follow -- sh -c 'echo one; sleep 0.3; echo two >&2; exit 3' \
    >"$work/o1" 2>"$work/e1"
check "--follow: the command's exit status" $? 3
check "stdout is still only the identifier" "$(count <"$work/o1") $(idof "$work/o1" | count)" "1 1"
check "the log is shown on stderr, labels and all" \
    "$(grep -c -e '^OUT: one$' -e '^ERR: two$' -e '^Process exited with code 3$' "$work/e1")" 3

"$bs" start --log-dir "$work/f" -f --expect two -- sh -c 'echo one; echo two; sleep 2; echo three' \
    >"$work/o2" 2>"$work/e2"
check "--follow with --expect returns on the match" $? 0
check "having shown the log up to it" "$(grep -c -e '^OUT: one$' -e '^OUT: two$' "$work/e2")" 2
sleep 2.5
check "and nothing of what came after" "$(grep -c '^OUT: three$' "$work/e2")" 0
has "which is in the log all the same" "$(logof "$work/o2")" '^OUT: three$'

finish
