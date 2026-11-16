#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: pid reuse: the pid of the command is already filed
#
# Goal:
#   If the command's pid already appears under any babysit pid in the
#   current generation, the old log is left untouched and a new generation
#   is started. Child pids cannot be chosen, so the test plants a log for
#   each of the next 400 pids and relies on pids being handed out in order.
#
# Procedure:
#   1. Run babysit once to learn the current pid.
#   2. Plant <log-dir>/<boot>/0/1/<pid>/log for the next 400 pids, using a
#      single process so that planting does not use up pids.
#   3. Run babysit again.
#
# Fails if:
#   - the command's pid did not land on a planted log (the test did not
#     exercise the case; pids are not sequential on this system).
#   - the new log is not under generation 1.
#   - any of the 400 planted logs was changed or removed.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

mkdir -p "$work/u/$stamp/0/1"
"$bs" --log-dir "$work/u" -- true >"$work/o9" 2>/dev/null
cp=$(basename "$(dirname "$(logof "$work/o9")")")
lo=$((cp + 1)); hi=$((cp + 400))
eval "mkdir \"$work/u/$stamp/0/1/\"{$lo..$hi}"   # one process: the loop must not use up pids
for ((p = lo; p <= hi; p++)); do echo old >"$work/u/$stamp/0/1/$p/log"; done
"$bs" --log-dir "$work/u" -- true >"$work/o9" 2>/dev/null
cp=$(basename "$(dirname "$(logof "$work/o9")")")
check "child pid landed on a planted log" "$([ "$cp" -ge "$lo" ] && [ "$cp" -le "$hi" ] && echo y)" y
check "reused child pid: logs to generation 1" \
    "$(logof "$work/o9" | grep -cE "^$work/u/$stamp/1/[0-9]+/$cp/log$")" 1
check "reused child pid: old logs untouched" "$(cat "$work/u/$stamp/0/1/$cp/log"; ls "$work/u/$stamp/0/1" | count)" "old
400"

finish
