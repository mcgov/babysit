#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: the caller's pipe is released
#
# Goal:
#   A caller reading babysit's output through a pipe sees end-of-file as
#   soon as babysit returns early, and also if babysit is killed. This is
#   the property that stops an agent harness from hanging on a background
#   process.
#
# Procedure:
#   1. Capture babysit's output with command substitution for a command that
#      stalls; the substitution only returns once every holder of the pipe
#      has closed it.
#   2. Pipe babysit into a reader, kill babysit with SIGKILL after 1 second,
#      and time how long the reader takes to see end-of-file.
#
# Fails if:
#   - either read takes 4 seconds or more, which means the babysitter or the
#     command still held the caller's pipe.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

t=$(date +%s)
: "$("$bs" --log-dir "$work/h" --idle 1 -- sh -c 'echo a; sleep 5' 2>/dev/null)"
check "pipe released after early return" "$(quick "$t" 4)" y
t=$(date +%s)
{ "$bs" --log-dir "$work/h" -- sleep 5 2>/dev/null & echo $! >"$work/pp"; } |
    { sleep 1; kill -9 "$(cat "$work/pp")"; cat >/dev/null; }
check "pipe released when babysit is killed" "$(quick "$t" 4)" y

finish
