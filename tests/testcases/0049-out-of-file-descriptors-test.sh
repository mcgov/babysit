#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: out of file descriptors
#
# Goal:
#   "babysit start" that cannot get the pipes it needs, in the parent or
#   in the babysitter, refuses with exit 2 and a reason, and leaves no
#   instance behind: no identifier on stdout, no directory under the log
#   directory for babysitd to puzzle over. It either starts the command
#   properly or does not start it at all.
#
# Procedure:
#   1. Run "babysit start -- true" under every open-file limit from 4 to
#      24, each with its own log directory. The low limits stop the parent,
#      the middle ones the babysitter, the high ones nothing.
#
# Fails if:
#   - any run ends with a status other than 0 or 2.
#   - a run that exits 2 prints an identifier, gives no reason, or leaves
#     anything in its log directory.
#   - a run that exits 0 has no finished instance to show for it.
#   - the limits tried do not produce at least one refusal and one success
#     (the test would then prove nothing).

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

refused=0
worked=0
bad=""
for n in $(seq 4 24); do
    L="$work/l$n"
    (ulimit -n "$n" && exec "$bs" start --log-dir "$L" -- true) >"$work/o" 2>"$work/e"
    rc=$?
    left=$(find "$L" -mindepth 1 2>/dev/null | count)
    if [ "$rc" -eq 2 ]; then
        refused=$((refused + 1))
        [ "$left" -eq 0 ] || bad="$bad limit $n: refused but left $left files;"
        [ -s "$work/o" ] && bad="$bad limit $n: refused but printed an identifier;"
        grep -q '^babysit: ' "$work/e" || bad="$bad limit $n: refused without a reason;"
    elif [ "$rc" -eq 0 ]; then
        worked=$((worked + 1))
        id=$(idof "$work/o")
        [ "$(cat "$L/$id/exit_code" 2>/dev/null)" = 0 ] ||
            bad="$bad limit $n: exit 0 but no finished instance;"
    else
        bad="$bad limit $n: exit $rc;"
    fi
done
check "every limit: started properly, or refused and left nothing" "$bad" ""
check "some limits were too low" "$([ "$refused" -gt 0 ] && echo y)" y
check "and some were enough" "$([ "$worked" -gt 0 ] && echo y)" y

finish
