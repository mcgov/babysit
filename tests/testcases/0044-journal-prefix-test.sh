#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: journal priority prefixes only for the journal
#
# Goal:
#   babysitd marks its messages with "<6>" and "<4>" priorities when its
#   stderr is the stream systemd set up, and at no other time. The
#   JOURNAL_STREAM variable is inherited by everything a service starts, so
#   its presence alone must not switch the prefixes on.
#
# Procedure:
#   1. Run "babysitd --once" on a past day with JOURNAL_STREAM naming
#      some other file than its stderr.
#   2. Do it again with JOURNAL_STREAM giving the device and inode of the
#      file that really is its stderr.
#
# Fails if:
#   - the message is prefixed in the first run.
#   - it is not prefixed in the second.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

devino() { stat -c '%d:%i' "$1" 2>/dev/null || stat -f '%d:%i' "$1"; }

plant "$work/jp" 20230101/00/01/aaaa x
: >"$work/j1"; : >"$work/other"
JOURNAL_STREAM=$(devino "$work/other") "$bsd" --once --log-dir "$work/jp" --config /dev/null -D 0 -u 0 2>>"$work/j1"
has "no prefix when stderr is not the journal stream" "$work/j1" '^babysitd: archived'

plant "$work/jp" 20230102/00/01/aaaa x
: >"$work/j2"
JOURNAL_STREAM=$(devino "$work/j2") "$bsd" --once --log-dir "$work/jp" --config /dev/null -D 0 -u 0 2>>"$work/j2"
has "prefix when it is" "$work/j2" '^<6>babysitd: archived'

n=3
for js in junk 12 12: 12:34x; do
    plant "$work/jp" "2023010$n/00/01/aaaa" x
    : >"$work/j$n"
    JOURNAL_STREAM=$js "$bsd" --once --log-dir "$work/jp" --config /dev/null -D 0 -u 0 2>>"$work/j$n"
    has "no prefix when JOURNAL_STREAM is '$js'" "$work/j$n" '^babysitd: archived'
    n=$((n + 1))
done
plant "$work/jp" 20230107/00/01/aaaa x
env -u JOURNAL_STREAM "$bsd" --once --log-dir "$work/jp" --config /dev/null -D 0 -u 0 2>"$work/j7"
has "no prefix when JOURNAL_STREAM is not set" "$work/j7" '^babysitd: archived'

finish
