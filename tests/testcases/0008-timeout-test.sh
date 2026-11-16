#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: --timeout returns after a fixed time
#
# Goal:
#   --timeout returns 124 once the time is up, even when the command is
#   producing output steadily.
#
# Procedure:
#   1. Run a command that prints every 0.3 seconds for 3 seconds, with
#      --timeout 1.
#
# Fails if:
#   - the exit status is not 124.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

"$bs" start --log-dir "$work/e" --timeout 1 -- sh -c 'for i in 1 2 3 4 5 6 7 8 9 10; do echo x; sleep 0.3; done' >/dev/null 2>&1
check "--timeout" $? 124

finish
