#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Performance: light load
#
# Goal:
#   Measure what running a command under babysit costs when the command
#   prints next to nothing (200 lines, 16 kB): the fixed cost of starting
#   the babysitter, creating the instance and recording the exit status.
#
# Procedure:
#   1. Time "perftool gen 200 79" on its own, its output redirected to a
#      file, as many times as --repeat says.
#   2. Time the same command under "babysit start", which waits for it and
#      writes the same output to the log.
#   3. Record every timing as a sample for tests/perf.sh to store.
#
# Fails if:
#   - either run fails, or does not write all 200 lines.
#   Speed alone never fails this case: the numbers are for the report.

# shellcheck source=tests/perflib.sh
. "$(dirname "$0")/../perflib.sh"

measure light 200 79

finish
