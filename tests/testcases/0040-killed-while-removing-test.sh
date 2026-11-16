#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: a run killed while removing a packed directory
#
# Goal:
#   Once a tarball is complete, nothing a later run does can make it worse.
#   A directory that was being removed when babysitd was killed is finished
#   off, not packed again over the complete tarball.
#
# Procedure:
#   1. Lay out what a killed run leaves: a complete packed generation next
#      to a half-removed "0.removing" directory, and the same for an earlier
#      boot whose archive already exists.
#   2. Run "babysitd --once".
#   3. Pack a fresh generation and check nothing is left over from that.
#
# Fails if:
#   - the packed generation or the archive no longer holds both logs.
#   - a .removing directory is left behind, or a second archive is made.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

d="$work/kr"
mkdir -p "$d/$stamp/0/100/101" "$d/$stamp/0/200/201" "$d/$stamp/1/300/301" \
    "$d/20230101-000000/0/1/2" "$d/20230101-000000/0/3/4" "$d/archive"
echo one >"$d/$stamp/0/100/101/log"; echo two >"$d/$stamp/0/200/201/log"
echo live >"$d/$stamp/1/300/301/log"
echo a >"$d/20230101-000000/0/1/2/log"; echo b >"$d/20230101-000000/0/3/4/log"
tar -czf "$d/$stamp/0.tar.gz" -C "$d/$stamp" 0
tar -czf "$d/archive/babysit_logs_20230101-000000.tar.gz" -C "$d" 20230101-000000
mv "$d/$stamp/0" "$d/$stamp/0.removing"; rm -r "$d/$stamp/0.removing/100"
mv "$d/20230101-000000" "$d/20230101-000000.removing"; rm -r "$d/20230101-000000.removing/0/1"

"$bsd" --once --log-dir "$d" --config /dev/null -u 0 2>"$work/kr.err"
check "the packed generation still holds both logs" \
    "$(tar -tzf "$d/$stamp/0.tar.gz" | grep -c '/log$')" 2
check "the archive still holds both logs, and there is only one" \
    "$(tar -tzf "$d/archive/babysit_logs_20230101-000000.tar.gz" | grep -c '/log$') $(ls "$d/archive" | count)" "2 1"
check "the half-removed directories are gone" \
    "$(find "$d" -name '*.removing' | count)" 0

mkdir -p "$d/$stamp/2/400/401"; echo newer >"$d/$stamp/2/400/401/log"
touch -t "$old1m" "$d/$stamp/1"
"$bsd" --once --log-dir "$d" --config /dev/null -u 0 2>>"$work/kr.err"
check "a normal pack leaves the tarball and nothing else" \
    "$(ls "$d/$stamp" | tr '\n' ' ')" "0.tar.gz 1.tar.gz 2 "

finish
