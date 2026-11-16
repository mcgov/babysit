#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: babysitd packs a superseded generation once it is idle
#
# Goal:
#   A generation that is no longer the newest is replaced by
#   <generation>.tar.gz, but only after its last writer has finished; a
#   packed generation's number is never reused.
#
# Procedure:
#   1. Start a 5-second command with --detach in generation 0, then create
#      generation 1.
#   2. Run babysitd once while the command is still running, and again after
#      it has finished.
#   3. Remove generation 1 and run babysit, to see where it logs.
#
# Fails if:
#   - generation 0 is packed while the command is still writing to it.
#   - after the command ends, 0.tar.gz does not contain its complete log, or
#     the directory is still there.
#   - babysit logs into generation 0 again although 0.tar.gz exists.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

"$bs" --log-dir "$work/v" --detach -- sh -c 'sleep 5; echo late' >"$work/o12" 2>/dev/null
mkdir "$work/v/$stamp/1"
sleep 2.5
"$bsd" --once --log-dir "$work/v" --config /dev/null 2>/dev/null
check "babysitd: generation with a writer is not packed" \
    "$([ -d "$work/v/$stamp/0" ] && [ ! -e "$work/v/$stamp/0.tar.gz" ] && echo waiting)" waiting
sleep 3.5
"$bsd" --once --log-dir "$work/v" --config /dev/null 2>/dev/null
check "babysitd: finished generation packed as <boot>/0.tar.gz" \
    "$(tar -xzOf "$work/v/$stamp/0.tar.gz" 2>/dev/null | grep -c '^OUT: late$')" 1
check "babysitd: packed generation directory removed" "$(ls "$work/v/$stamp" | tr '\n' ' ')" "0.tar.gz 1 "
rm -rf "$work/v/$stamp/1"
"$bs" --log-dir "$work/v" -- true >"$work/o12b" 2>/dev/null
check "generation numbers are not reused after packing" \
    "$(logof "$work/o12b" | grep -cE "^$work/v/$stamp/0/")" 0

finish
