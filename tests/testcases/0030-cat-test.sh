#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: cat
#
# Goal:
#   "babysit cat" gives back what the command wrote, as it wrote it: the
#   lines of its stdout, of its stderr, or of both, without the "OUT: " and
#   "ERR: " labels and without anything else that is in the log.
#
# Procedure:
#   1. Run a command that writes to both streams, including a line that
#      itself starts with a label, an empty line and a NUL byte, under a
#      command line that starts with a label too.
#   2. Print its stdout, its stderr, and both.
#   3. Do the same for a command that is still running.
#   4. Give a stream name twice, an unknown identifier, and too much.
#
# Fails if:
#   - "cat stdout" or "cat stderr" prints anything but that stream's lines.
#   - "cat" alone, or with both names, does not print both streams in log
#     order.
#   - a label, the command line, or a line about the process is printed.
#   - output written so far by a running command is not printed.
#   - a bad invocation does not exit 2.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

c() { "$bs" cat --log-dir "$work/c" "$@"; }

printf '#!/bin/sh\necho one\necho "OUT: not a label"\necho\nsleep 0.2\necho oops >&2\nsleep 0.2\nprintf "a\\0b\\n"\necho last >&2\n' >"$work/OUT: tricky"
chmod +x "$work/OUT: tricky"
id=$(cd "$work" && "$bs" start --log-dir "$work/c" -- "./OUT: tricky" 2>/dev/null)

check "cat stdout prints its stdout, labels off" "$(c stdout "$id" | tr '\0' '@')" "one
OUT: not a label

a@b"
check "cat stderr prints its stderr" "$(c stderr "$id")" "oops
last"
check "cat alone prints both, in log order" "$(c "$id" | tr '\0' '@')" "one
OUT: not a label

oops
a@b
last"
check "naming both is the same" "$(c stderr stdout "$id" | cksum)" "$(c "$id" | cksum)"
check "the identifier may come first" "$(c "$id" stderr)" "oops
last"
check "nothing about the process is printed" "$(c "$id" | grep -c -e 'Process' -e 'tricky')" 0

id2=$("$bs" start --log-dir "$work/c" --expect ready -- sh -c 'echo ready; sleep 30' 2>/dev/null)
check "a running command: what it has written so far" "$(c stdout "$id2")" ready
check "and nothing on the stream it has not used" "$(c stderr "$id2" | count)" 0
"$bs" signal --log-dir "$work/c" TERM "$id2"

c stdout 20200101/00/00/abcd >/dev/null 2>&1
check "no such instance: exit 2" $? 2
c stdout >/dev/null 2>&1
check "no identifier: exit 2" $? 2
c stdout "$id" extra >/dev/null 2>&1
check "too many arguments: exit 2" $? 2

finish
