#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: long lines, unterminated lines and empty lines
#
# Goal:
#   Lines longer than 4096 bytes are split, output that ends without a
#   newline is still logged, and empty lines are preserved.
#
# Procedure:
#   1. Print one 5000-byte line.
#   2. Print text with no trailing newline on stdout and on stderr, then
#      exit.
#   3. Print an empty line followed by a normal one.
#
# Fails if:
#   - the long line is not logged as two lines of 4096 and 904 bytes.
#   - either unterminated tail is missing from the log.
#   - the empty line is dropped or altered.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

"$bs" --log-dir "$work/oh" -- sh -c 'head -c 5000 /dev/zero | tr "\0" x; echo' >"$work/h8" 2>/dev/null
check "long lines are split at 4096 bytes" \
    "$(grep '^OUT: ' "$(logof "$work/h8")" | awk '{ print length($0) - 5 }' | tr '\n' ' ')" "4096 904 "
"$bs" --log-dir "$work/oh" -- sh -c 'printf tail; printf etail >&2' >"$work/h9" 2>/dev/null
check "unterminated last lines are logged" \
    "$(grep -c -e '^OUT: tail$' -e '^ERR: etail$' "$(logof "$work/h9")")" 2
"$bs" --log-dir "$work/oh" -- sh -c 'echo; echo x' >"$work/h10" 2>/dev/null
check "empty lines are kept" "$(sed -n 3p "$(logof "$work/h10")")" "OUT: "

finish
