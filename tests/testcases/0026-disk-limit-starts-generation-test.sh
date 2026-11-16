#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: compact_at_disk_percent starts a new generation
#
# Goal:
#   When the log filesystem is at least compact_at_disk_percent full,
#   babysitd starts a new generation so that the logs written so far can be
#   packed. It does nothing when the limit is 0 or when the current
#   generation is empty. A limit of 1 is used because any filesystem in use
#   is at least 1% full.
#
# Procedure:
#   1. Create one log, and a stray .partial file.
#   2. Run babysitd once with the limit at 0, then at 1.
#   3. Wait out the quiet period and run it again with the limit taken from a
#      config file.
#
# Fails if:
#   - a limit of 0 changes anything.
#   - a limit of 1 does not create generation 1 and say why.
#   - generation 0 is then not packed with its log inside, or the empty
#     generation 1 is itself superseded.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

"$bs" --log-dir "$work/w" -- echo kept >/dev/null 2>&1
: >"$work/w/$stamp/9.tar.gz.partial"; rm -f "$work/w/$stamp/9.tar.gz.partial"
: >"$work/w/$stamp/junk.partial"
"$bsd" --once --log-dir "$work/w" --compact-at-disk-percent 0 --config /dev/null 2>/dev/null
check "babysitd: disk limit 0 does nothing" "$(ls "$work/w/$stamp" | tr '\n' ' ')" "0 "
"$bsd" --once --log-dir "$work/w" --compact-at-disk-percent 1 --config /dev/null 2>"$work/e13"
check "babysitd: disk limit reached starts a generation" "$(ls "$work/w/$stamp" | tr '\n' ' ')" "0 1 "
has "babysitd: disk limit is reported" "$work/e13" 'full (limit 1%)'
sleep 2.5
printf 'compact_at_disk_percent = 1\ncheck_interval = 6h\n' >"$work/conf2"
"$bsd" --once --log-dir "$work/w" --config "$work/conf2" 2>/dev/null
check "babysitd: old generation packed, empty one not superseded" \
    "$(ls "$work/w/$stamp" | tr '\n' ' ')" "0.tar.gz 1 "
check "babysitd: packed generation holds the log" \
    "$(tar -xzOf "$work/w/$stamp/0.tar.gz" 2>/dev/null | grep -c '^OUT: kept$')" 1

finish
