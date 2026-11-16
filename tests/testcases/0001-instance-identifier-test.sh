#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: the instance identifier
#
# Goal:
#   babysit prints one thing on stdout: the identifier of the instance it
#   started, <YYYYmmdd>/<HH>/<MM>/<salt>. The identifier is the path of the
#   instance's directory under the log directory, and no two instances
#   share one. The command's output is echoed on stderr, not stdout.
#
# Procedure:
#   1. Run a command and capture stdout and stderr separately.
#   2. Run twenty more in the same log directory.
#
# Fails if:
#   - stdout is anything but a single well-formed identifier dated today.
#   - there is no log at <log-dir>/<identifier>/log.
#   - the command's output is on stdout, or missing from stderr.
#   - two of the twenty-one identifiers are the same.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

"$bs" start --log-dir "$work/i" -- sh -c 'echo hi' >"$work/o" 2>"$work/e"
check "stdout is one line" "$(count <"$work/o")" 1
check "and it is an identifier" \
    "$(grep -cE '^[0-9]{8}/[0-9]{2}/[0-9]{2}/[a-km-np-z2-9]{4}$' "$work/o")" 1
check "dated today" "$(cut -c 1-8 "$work/o")" "$today"
check "the identifier is where the log is" \
    "$([ -f "$work/i/$(cat "$work/o")/log" ] && echo y)" y
has "the command's output is echoed on stderr" "$work/e" '^OUT: hi$'

cp "$work/o" "$work/ids"
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    "$bs" start --log-dir "$work/i" -- true 2>/dev/null >>"$work/ids"
done
check "twenty-one instances, twenty-one identifiers" "$(sort -u "$work/ids" | count)" 21

finish
