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
#   1. Lay out what a killed run leaves: a complete archive next to a
#      half-removed "<day>.removing" directory, and the same for a packed
#      hour in a day that cannot be archived yet.
#   2. Run "babysitd --once".
#   3. Let that day be archived and check nothing is left over.
#
# Fails if:
#   - the archive or the packed hour no longer holds both logs.
#   - a .removing directory is left behind, or a second archive is made.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

d="$work/kr"
plant "$d" 20230101/01/02/aaaa a
plant "$d" 20230101/03/04/bbbb b
mkdir -p "$d/archive"
tar -czf "$d/archive/babysit_logs_20230101.tar.gz" -C "$d" 20230101
mv "$d/20230101" "$d/20230101.removing"; rm -r "$d/20230101.removing/01"

plant "$d" 20230102/05/06/cccc one
plant "$d" 20230102/05/07/dddd two
plant "$d" 20230102/08/09/eeee live
tar -czf "$d/20230102/05.tar.gz" -C "$d/20230102" 05
mv "$d/20230102/05" "$d/20230102/05.removing"; rm -r "$d/20230102/05.removing/06"
"$lp" --hold 2 "$d/20230102/08/09/eeee/log" &
sleep 0.5

"$bsd" --once --log-dir "$d" --config /dev/null -u 0 2>"$work/kr.err"
check "the archive still holds both logs, and there is only one" \
    "$(tar -tzf "$d/archive/babysit_logs_20230101.tar.gz" | grep -c '/log$') $(ls "$d/archive" | count)" "2 1"
check "the packed hour still holds both logs" \
    "$(tar -tzf "$d/20230102/05.tar.gz" | grep -c '/log$')" 2
check "the half-removed directories are gone" \
    "$(find "$d" -name '*.removing' | count)" 0
wait
settle "$d/20230102"
"$bsd" --once --log-dir "$d" --config /dev/null -u 0 2>>"$work/kr.err"
check "a normal pack leaves the archives and nothing else" \
    "$(ls "$d" | tr '\n' ' '; ls "$d/archive" | tr '\n' ' ')" \
    "archive babysit_logs_20230101.tar.gz babysit_logs_20230102.tar.gz "

finish
