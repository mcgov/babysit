#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: babysitd does its housekeeping at each scheduled wake-up
#
# Goal:
#   With no request at all, babysitd still does the full pass every
#   check_interval.
#
# Procedure:
#   1. Start babysitd with a 1-second interval.
#   2. Plant a past day's directory after it has started, wait two
#      seconds, stop it.
#
# Fails if:
#   - the past day is not archived.
#   - babysitd logs that it was woken by a request, which would mean the
#     pass was not the scheduled one.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

"$bsd" --log-dir "$work/sv3" --config /dev/null -D 0 --check-interval 1s --max-age-days 5 2>"$work/d9" &
sv=$!
sleep 0.5
plant "$work/sv3" 20220101/00/01/aaaa z
sleep 2; kill "$sv"; wait "$sv"
check "babysitd: scheduled wake-up does the full pass unprompted" \
    "$(ls "$work/sv3/archive" | tr '\n' ' ')" "babysit_logs_20220101.tar.gz "
check "babysitd: scheduled wake-up is not logged as a request" "$(grep -c 'woken by a request' "$work/d9")" 0

finish
