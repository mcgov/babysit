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
#   2. Run "babysitd --once", and once more when the day has settled.
#   3. Let the rest of that day be archived and check nothing is left over.
#
# Fails if:
#   - the archive or the packed hour no longer holds both logs.
#   - the instance still being written in that day is disturbed.
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
"$lp" --hold 5 "$d/20230102/08/09/eeee/log" &
sleep 0.5

# The first run clears the leftovers, which touches the day; the day is
# packed by the next pass that finds it quiet.
"$bsd" --once --log-dir "$d" --config /dev/null -D 0 -u 0 2>"$work/kr.err"
settle "$d/20230102"
"$bsd" --once --log-dir "$d" --config /dev/null -D 0 -u 0 2>>"$work/kr.err"
check "the archive still holds both logs, and that day has only the one" \
    "$(tar -tzf "$d/archive/babysit_logs_20230101.tar.gz" | grep -c '/log$') $(ls -d "$d/archive"/*20230101* | count)" "2 1"
check "the packed hour, now inside its day's archive, still holds both logs" \
    "$(tar -xzOf "$d/archive/babysit_logs_20230102.tar.gz" 20230102/05.tar.gz | tar -tzf - | grep -c '/log$')" 2
check "the half-removed directories are gone" \
    "$(find "$d" -name '*.removing' | count)" 0
check "the instance being written was left where it is" "$(cat "$d/20230102/08/09/eeee/log")" live
wait
settle "$d/20230102"
"$bsd" --once --log-dir "$d" --config /dev/null -D 0 -u 0 2>>"$work/kr.err"
check "in the end there are the archives and nothing else" \
    "$(ls "$d" | tr '\n' ' '; ls "$d/archive" | tr '\n' ' ')" \
    "archive babysit_logs_20230101.tar.gz babysit_logs_20230102.2.tar.gz babysit_logs_20230102.tar.gz "

finish
