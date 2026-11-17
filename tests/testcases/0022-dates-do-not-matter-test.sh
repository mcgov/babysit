#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: the dates are only how the files are organised
#
# Goal:
#   Identifiers are unique because of their salt, not their date, so a
#   clock that has been set back is no obstacle: babysit starts commands
#   whatever dates the log directory already holds. Exit status 75 means
#   one thing only, that the command itself exited with 75. babysitd treats
#   a day dated after today like any other day that is not due yet.
#
# Procedure:
#   1. Plant finished instances dated in the year 2099 and later today.
#   2. Run a command, and one that exits 75.
#   3. Run "babysitd --once".
#
# Fails if:
#   - babysit refuses to start, or exits 75 for a command that exited 0.
#   - a command exiting 75 is not reported as 75, with that status recorded.
#   - babysitd archives or removes the later-dated records.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

d="$work/dd"
plant "$d" 20991231/01/02/aaaa future
plant "$d" "$today/23/59/bbbb" tonight

"$bs" start --log-dir "$d" -- sh -c 'echo ran' >"$work/o1" 2>/dev/null
check "later-dated records do not stop a command from starting" "$?" 0
check "and it ran" "$("$bs" cat --log-dir "$d" "$(cat "$work/o1")")" ran

"$bs" start --log-dir "$d" -- sh -c 'exit 75' >"$work/o2" 2>/dev/null
check "a command that exits 75 is reported as 75" $? 75
check "which is what was recorded" "$("$bs" info --log-dir "$d" --exit-code "$(cat "$work/o2")")" 75

"$bsd" --once --log-dir "$d" --config /dev/null -D 0 -u 0 2>"$work/dd.err"
check "babysitd leaves a day dated after today alone" "$(cat "$d/20991231/01/02/aaaa/log")" future
check "and what is dated later today" "$(cat "$d/$today/23/59/bbbb/log")" tonight
check "and archives nothing" "$([ -e "$d/archive" ] && echo archived || echo none)" none

finish
