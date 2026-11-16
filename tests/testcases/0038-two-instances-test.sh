#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: two babysitd instances on one log directory
#
# Goal:
#   A second babysitd started while another is in the middle of packing
#   waits for it instead of working on the same directory, so nothing is
#   packed twice, half-written or lost.
#
# Procedure:
#   1. Lay out a superseded generation with three logs, and put a "tar" on
#      PATH that packs and then lingers for two seconds, so the first
#      instance is still inside its pass when the second starts.
#   2. Start one "babysitd --once" in the background and a second one half a
#      second later.
#
# Fails if:
#   - either instance reports a failed pack.
#   - the packed generation does not hold all three logs.
#   - the directory or a .partial file is left behind.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

mkdir -p "$work/bin" "$work/ti/$stamp/1/9/10"
for n in 1 2 3; do
    mkdir -p "$work/ti/$stamp/0/1$n/2$n"
    echo "log $n" >"$work/ti/$stamp/0/1$n/2$n/log"
done
echo live >"$work/ti/$stamp/1/9/10/log"
touch -t "$old1m" "$work/ti/$stamp/0"
realtar=$(command -v tar)
printf '#!/bin/sh\n"%s" "$@" || exit 1\nsleep 2\n' "$realtar" >"$work/bin/tar"
chmod +x "$work/bin/tar"

PATH="$work/bin:$PATH" "$bsd" --once --log-dir "$work/ti" --config /dev/null -u 0 2>"$work/t1" &
sleep 0.5
PATH="$work/bin:$PATH" "$bsd" --once --log-dir "$work/ti" --config /dev/null -u 0 2>"$work/t2"
wait
check "neither instance failed to pack" \
    "$(cat "$work/t1" "$work/t2" | grep -c 'could not')" 0
check "the generation was packed once, by the first instance" \
    "$(cat "$work/t1" "$work/t2" | grep -c 'packed log generation 0')" 1
check "all three logs are in the packed generation" \
    "$(tar -tzf "$work/ti/$stamp/0.tar.gz" | grep -c '/log$')" 3
check "the directory and partial files are gone" \
    "$(ls "$work/ti/$stamp" | tr '\n' ' ')" "0.tar.gz 1 "

finish
