#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: a request on the FIFO triggers a full pass
#
# Goal:
#   Writing anything to <log-dir>/.request wakes a running babysitd at
#   once, and it then does everything it would do at a scheduled wake-up.
#
# Procedure:
#   1. Start babysitd with its default interval of one day.
#   2. Lay out a past day and an archive that is past the age limit.
#   3. Write to the FIFO and give it two seconds.
#
# Fails if:
#   - the past day is not archived, or the old archive not deleted, within
#     that time.
#   - the request is not mentioned in the service's messages.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

"$bsd" --log-dir "$work/k" --max-age-days 5 --config /dev/null -D 0 -u 0 2>"$work/e15" &
kd=$!
sleep 0.5
plant "$work/k" 20240101/11/12/aaaa earlier
mkdir -p "$work/k/archive"
: >"$work/k/archive/babysit_logs_20200101.tar.gz"
touch -t "$old10d" "$work/k/archive/babysit_logs_20200101.tar.gz"
echo 1 >"$work/k/.request"
sleep 2
kill "$kd"; wait "$kd" 2>/dev/null
check "request: past day archived, expired archive deleted" \
    "$(ls "$work/k/archive" | tr '\n' ' ')" "babysit_logs_20240101.tar.gz "
check "request: the archive holds the log" \
    "$(tar -xzOf "$work/k/archive/babysit_logs_20240101.tar.gz" 20240101/11/12/aaaa/log 2>/dev/null)" earlier
has "request: it is logged as one" "$work/e15" 'woken by a request'

finish
