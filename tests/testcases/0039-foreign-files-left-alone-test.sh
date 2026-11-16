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
#      files and tarballs in the archive and in today's directory, next to
#      one real past day.
#   2. Run "babysitd --once" with a one-day age limit and disk-pressure
#      deletion forced on.
#
# Fails if:
#   - the unrelated directory is archived or removed.
#   - any unrelated file or tarball is deleted.
#   - the real past day is not archived.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

d="$work/ff"
mkdir -p "$d/myproject/src" "$d/2024-notes" "$d/archive" "$d/$today/05/06/aaaa"
echo precious >"$d/myproject/src/main.c"
echo note >"$d/2024-notes/todo"
echo live >"$d/$today/05/06/aaaa/log"
plant "$d" 20230101/03/04/bbbb old
: >"$d/archive/holiday.tar.gz"; : >"$d/archive/notes.txt"
: >"$d/$today/backup.tar.gz"; : >"$d/$today/notes.txt"
touch -t "$old10d" "$d/archive/holiday.tar.gz" "$d/archive/notes.txt" \
    "$d/$today/backup.tar.gz" "$d/$today/notes.txt"

"$bsd" --once --log-dir "$d" --config /dev/null -u 0 --max-age-days 1 \
    --delete-at-disk-percent 1 2>"$work/ff.err"
check "unrelated directories are still there" \
    "$(cat "$d/myproject/src/main.c" "$d/2024-notes/todo" | tr '\n' ' ')" "precious note "
check "unrelated files in the archive are still there" \
    "$(ls "$d/archive" | tr '\n' ' ')" "holiday.tar.gz notes.txt "
check "unrelated files in a day directory are still there" \
    "$(ls "$d/$today" | tr '\n' ' ')" "05 backup.tar.gz notes.txt "
has "the real past day was archived" "$work/ff.err" 'archived the logs of 20230101'
check "its archive then went under disk pressure, and only it" \
    "$(grep -c ': deleted ' "$work/ff.err")" 1

finish
