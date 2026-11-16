#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: babysitd leaves alone what is not babysit's
#
# Goal:
#   A log directory may hold things babysit did not put there, for instance
#   because --log-dir was pointed at a directory already in use. babysitd
#   archives, expires and deletes only what carries babysit's own names.
#
# Procedure:
#   1. Put an unrelated directory in the log directory, and old unrelated
#      files and tarballs in the archive and in the current boot directory,
#      next to one real earlier boot.
#   2. Run "babysitd --once" with a one-day age limit and disk-pressure
#      deletion forced on.
#
# Fails if:
#   - the unrelated directory is archived or removed.
#   - any unrelated file or tarball is deleted.
#   - the real earlier boot is not archived.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

d="$work/ff"
mkdir -p "$d/myproject/src" "$d/2024-notes" "$d/archive" "$d/$stamp/0/1/2" "$d/20230101-000000/0/3/4"
echo precious >"$d/myproject/src/main.c"
echo note >"$d/2024-notes/todo"
echo live >"$d/$stamp/0/1/2/log"
echo old >"$d/20230101-000000/0/3/4/log"
: >"$d/archive/holiday.tar.gz"; : >"$d/archive/notes.txt"
: >"$d/$stamp/backup.tar.gz"; : >"$d/$stamp/notes.txt"
touch -t "$old10d" "$d/archive/holiday.tar.gz" "$d/archive/notes.txt" \
    "$d/$stamp/backup.tar.gz" "$d/$stamp/notes.txt"

"$bsd" --once --log-dir "$d" --config /dev/null -u 0 --max-age-days 1 \
    --delete-at-disk-percent 1 2>"$work/ff.err"
check "unrelated directories are still there" \
    "$(cat "$d/myproject/src/main.c" "$d/2024-notes/todo" | tr '\n' ' ')" "precious note "
check "unrelated files in the archive are still there" \
    "$(ls "$d/archive" | tr '\n' ' ')" "holiday.tar.gz notes.txt "
check "unrelated files in the boot directory are still there" \
    "$(ls "$d/$stamp" | tr '\n' ' ')" "0 backup.tar.gz notes.txt "
has "the real earlier boot was archived" "$work/ff.err" 'archived the logs of boot 20230101-000000'
check "its archive then went under disk pressure, and only it" \
    "$(grep -c ': deleted ' "$work/ff.err")" 1

finish
