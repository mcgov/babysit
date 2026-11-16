#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: commands that cannot run, and commands killed by a signal
#
# Goal:
#   A command that does not exist gives 127, one that is not executable
#   gives 126, neither leaves a log behind, and a command killed by a signal
#   gives 128 plus the signal number.
#
# Procedure:
#   1. Run a path that does not exist.
#   2. Run an empty file that has no execute permission.
#   3. Run a command that kills itself with SIGKILL.
#
# Fails if:
#   - the statuses are not 127, 126 and 137.
#   - a log file mentioning the non-executable command exists afterwards.
#   - the failed runs leave directories behind: only the command that did
#     run may have one.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

"$bs" --log-dir "$work/f" -- /no/such/program >/dev/null 2>&1
check "missing command exits 127" $? 127
"$bs" --log-dir "$work/f" -- sh -c 'kill -9 $$' >/dev/null 2>&1
check "signal death is 128+sig" $? 137
: >"$work/noexec"
"$bs" --log-dir "$work/f" -- "$work/noexec" >/dev/null 2>&1
check "non-executable command exits 126" $? 126
check "failed exec leaves no log" "$(find "$work/f" -name log -exec grep -l noexec {} + | count)" 0
check "failed exec leaves no directories either" \
    "$(find "$work/f/$stamp/0" -mindepth 1 -maxdepth 1 -type d | count)" 1

finish
