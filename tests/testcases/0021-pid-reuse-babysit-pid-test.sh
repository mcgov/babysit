#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: pid reuse: the pid of babysit is already filed
#
# Goal:
#   If an earlier babysit with the same pid already has a directory in the
#   current generation, the old log is left untouched and a new generation
#   is started, which later runs use too.
#
# Procedure:
#   1. From a shell, create <log-dir>/<boot>/0/<own pid>/999/log, then "exec"
#      babysit so that it has that same pid.
#   2. Run babysit once more, normally.
#
# Fails if:
#   - babysit fails.
#   - the new log is not under generation 1.
#   - the planted log was changed or removed.
#   - the following run goes anywhere but generation 1.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

sh -c 'mkdir -p "$2/0/$$/999"; echo old >"$2/0/$$/999/log"; echo $$; exec "$0" --log-dir "$1" -- true' \
    "$bs" "$work/r" "$work/r/$stamp" >"$work/o7" 2>/dev/null
check "reused babysit pid: runs anyway" $? 0
rp=$(sed -n 1p "$work/o7")
check "reused babysit pid: logs to generation 1" \
    "$(logof "$work/o7" | grep -cE "^$work/r/$stamp/1/$rp/[0-9]+/log$")" 1
check "reused babysit pid: old log untouched" "$(cat "$work/r/$stamp/0/$rp/999/log")" old
"$bs" --log-dir "$work/r" -- true >"$work/o7b" 2>/dev/null
check "later runs use the newest generation" \
    "$(logof "$work/o7b" | grep -cE "^$work/r/$stamp/1/[0-9]+/[0-9]+/log$")" 1

finish
