#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: a write to the FIFO triggers the whole housekeeping pass
#
# Goal:
#   Anything written to <log-dir>/.request makes a running babysitd do
#   everything it would do at a scheduled wake-up: archive earlier boots,
#   pack superseded generations and expire old archives. This is how
#   housekeeping can be tested against a hand-made directory.
#
# Procedure:
#   1. Start babysitd with a 5-day age limit.
#   2. Lay out by hand: a directory for an earlier boot, a superseded
#      generation 0 (backdated past the quiet period) next to generation 1,
#      and a 10-day-old archive.
#   3. Write to the FIFO, wait, stop babysitd.
#
# Fails if:
#   - the earlier boot is not archived, or the old archive is not deleted.
#   - generation 0 is not packed, or its tarball does not contain the
#     planted log.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

"$bsd" --log-dir "$work/k" --max-age-days 5 --config /dev/null 2>"$work/e15" &
kd=$!
sleep 0.5
mkdir -p "$work/k/20240101-000000/0/11/12" "$work/k/$stamp/0/21/22" "$work/k/$stamp/1" "$work/k/archive"
echo earlier-boot >"$work/k/20240101-000000/0/11/12/log"
echo superseded >"$work/k/$stamp/0/21/22/log"
: >"$work/k/archive/babysit_logs_20200101-000000.tar.gz"
touch -t "$old10d" "$work/k/archive/babysit_logs_20200101-000000.tar.gz"
touch -t "$old1m" "$work/k/$stamp/0"      # past the quiet period
echo 1 >"$work/k/.request"
sleep 2
kill "$kd"; wait "$kd" 2>/dev/null
check "request: earlier boot archived, expired archive deleted" \
    "$(ls "$work/k/archive" | tr '\n' ' ')" "babysit_logs_20240101-000000.tar.gz "
check "request: superseded generation packed" "$(ls "$work/k/$stamp" | tr '\n' ' ')" "0.tar.gz 1 "
check "request: packed generation holds the log" \
    "$(tar -xzOf "$work/k/$stamp/0.tar.gz" 0/21/22/log 2>/dev/null)" superseded

finish
