#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: delete_at_disk_percent deletes packed logs, oldest first
#
# Goal:
#   With delete_at_disk_percent set and the filesystem that full, babysitd
#   deletes packed logs (archives and packed hours) oldest first, and
#   nothing else. It is off by default.
#
# Procedure:
#   1. Lay out two archives and two packed hours with distinct ages, a
#      live log, and an unrelated file in the archive directory.
#   2. Run babysitd with the option unset, then with a limit of 100 (never
#      reached).
#   3. Run it with a limit of 1 (always reached) from a config file, twice.
#   4. Run it with invalid values.
#
# Fails if:
#   - anything is deleted while the option is off or the limit is not
#     reached.
#   - the packed logs are not deleted in order of age.
#   - the live log or the unrelated file is deleted.
#   - babysitd does not say that deleting everything was not enough, or, on
#     the second run, that there was nothing left.
#   - invalid percentages are accepted.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

mkdir -p "$work/dp/archive" "$work/dp/$today/02/05/aaaa"
echo live >"$work/dp/$today/02/05/aaaa/log"
: >"$work/dp/archive/babysit_logs_20200101.tar.gz"
: >"$work/dp/archive/babysit_logs_20210101.tar.gz"
: >"$work/dp/$today/00.tar.gz"; : >"$work/dp/$today/01.tar.gz"
: >"$work/dp/archive/notes.txt"
touch -t "$(ago 400)" "$work/dp/archive/babysit_logs_20200101.tar.gz"
touch -t "$(ago 300)" "$work/dp/$today/00.tar.gz"
touch -t "$(ago 200)" "$work/dp/archive/babysit_logs_20210101.tar.gz"
touch -t "$(ago 100)" "$work/dp/$today/01.tar.gz"
"$bsd" --once --log-dir "$work/dp" --config /dev/null -u 0 2>"$work/dp1"
check "delete: off by default" "$(find "$work/dp" -name '*.tar.gz' | count)" 4
"$bsd" --once --log-dir "$work/dp" --config /dev/null -u 0 --delete-at-disk-percent 100 2>"$work/dp2"
check "delete: nothing is deleted below the limit" "$(find "$work/dp" -name '*.tar.gz' | count)" 4
printf 'delete_at_disk_percent = 1\ncompact_at_disk_percent = 0\n' >"$work/dp.conf"
"$bsd" --once --log-dir "$work/dp" --config "$work/dp.conf" 2>"$work/dp3"
check "delete: packed logs go, oldest first" \
    "$(sed -n 's/.*deleted .*\/\([^/]*\/[^/]*\.tar\.gz\)$/\1/p' "$work/dp3" | tr '\n' ' ')" \
    "archive/babysit_logs_20200101.tar.gz $today/00.tar.gz archive/babysit_logs_20210101.tar.gz $today/01.tar.gz "
check "delete: live logs and other files are never deleted" \
    "$(cat "$work/dp/$today/02/05/aaaa/log"; ls "$work/dp/archive")" "live
notes.txt"
has "delete: says when deleting everything was not enough" "$work/dp3" 'still .* full after deleting every packed log'
"$bsd" --once --log-dir "$work/dp" --config "$work/dp.conf" 2>"$work/dp4"
has "delete: says when there is nothing left to delete" "$work/dp4" 'no packed logs to delete'
"$bsd" --once --log-dir "$work/dp" --config /dev/null --delete-at-disk-percent 101 2>/dev/null
check "delete: percent over 100 is rejected" $? 2
"$bsd" --once --log-dir "$work/dp" --config /dev/null -D x 2>/dev/null
check "delete: bad percent is rejected" $? 2

finish
