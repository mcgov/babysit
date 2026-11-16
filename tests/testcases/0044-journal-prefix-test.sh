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
#   1. Run "babysitd --once" on an earlier boot with JOURNAL_STREAM naming
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

mkdir -p "$work/jp/20230101-000000/0/1/2"; echo x >"$work/jp/20230101-000000/0/1/2/log"
: >"$work/j1"; : >"$work/other"
JOURNAL_STREAM=$(devino "$work/other") "$bsd" --once --log-dir "$work/jp" --config /dev/null -u 0 2>>"$work/j1"
has "no prefix when stderr is not the journal stream" "$work/j1" '^babysitd: archived'

mkdir -p "$work/jp/20230102-000000/0/1/2"; echo x >"$work/jp/20230102-000000/0/1/2/log"
: >"$work/j2"
JOURNAL_STREAM=$(devino "$work/j2") "$bsd" --once --log-dir "$work/jp" --config /dev/null -u 0 2>>"$work/j2"
has "prefix when it is" "$work/j2" '^<6>babysitd: archived'

finish
