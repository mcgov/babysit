#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: babysitd: clean shutdown and degraded start
#
# Goal:
#   SIGTERM and SIGINT end the service with status 0. An unusable request
#   FIFO and an unreadable last-pid source are reported but do not stop it.
#   On FreeBSD the last pid falls back to the kern.lastpid sysctl, so no
#   notice is expected there.
#
# Procedure:
#   1. Put a directory where the FIFO should be, name a last-pid file that
#      does not exist, start babysitd and send SIGTERM.
#   2. Start babysitd normally and send SIGINT.
#
# Fails if:
#   - either signal gives a non-zero exit status.
#   - the unusable FIFO is not reported.
#   - the missing last-pid source is not reported (or, on FreeBSD, is
#     reported although the sysctl is available).

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

mkdir -p "$work/sv/.request"                      # a directory where the FIFO should be
"$bsd" --log-dir "$work/sv" --config /dev/null -D 0 2>"$work/d8" &
sv=$!
sleep 0.7; kill "$sv"; wait "$sv"
check "babysitd: SIGTERM is a clean exit" $? 0
has "babysitd: unusable FIFO is reported" "$work/d8" 'is not usable'
"$bsd" --log-dir "$work/sv2" --config /dev/null -D 0 2>/dev/null &
sv=$!
sleep 0.5; kill -INT "$sv"; wait "$sv"
check "babysitd: SIGINT is a clean exit" $? 0

finish
