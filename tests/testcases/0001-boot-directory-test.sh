#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: logs are filed under a per-boot directory
#
# Goal:
#   Every log of the current boot lives under <log-dir>/<boot timestamp>/,
#   so logs from different boots can never collide even though pids start
#   over.
#
# Procedure:
#   1. The shared setup runs babysit once in a fresh log directory and
#      records the single directory it created as $stamp.
#   2. Check that the name has the form YYYYmmdd-HHMMSS.
#
# Fails if:
#   - the directory name is not a timestamp of that form (for example
#     "unknown-boot", which means the boot could not be identified on this
#     system).

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

check "boot directory is a timestamp" "$(echo "$stamp" | grep -cE '^[0-9]{8}-[0-9]{6}$')" 1

finish
