#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Performance: heavy load
#
# Goal:
#   Measure what running a command under babysit costs when the command
#   floods its output (2,000,000 lines, 160 MB) as fast as it can write:
#   how far the babysitter holds a fast writer back.
#
# Procedure:
#   1. Time "perftool gen 2000000 79" on its own, its output redirected to a
#      file, as many times as --repeat says.
#   2. Time the same command under "babysit start", which waits for it and
#      writes the same output to the log.
#   3. Record every timing as a sample for tests/perf.sh to store.
#
# Fails if:
#   - either run fails, or does not write all 2000000 lines.
#   Speed alone never fails this case: the numbers are for the report.

# shellcheck source=tests/perflib.sh
. "$(dirname "$0")/../perflib.sh"

measure heavy 2000000 79

finish
