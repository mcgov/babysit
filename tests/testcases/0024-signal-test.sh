#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: signal
#
# Goal:
#   "babysit signal SIGNAL ID" has the instance's babysitter send the
#   signal to the command's process group. The babysitter is the command's
#   parent, so the signal cannot reach a process that has since been given
#   the same pid. Once the command has exited there is nobody to signal.
#
# Procedure:
#   1. Start a shell that traps USR1 and has a child of its own, which
#      ignores USR1.
#   2. Send it USR1 by name, then TERM.
#   3. Send a signal to the finished instance, and give bad arguments.
#
# Fails if:
#   - the trapped signal is not delivered, or the request is not noted in
#     the log.
#   - TERM does not end the command and the child it started.
#   - signalling a finished instance does not exit 1.
#   - an unknown signal or a missing identifier does not exit 2.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

sg() { "$bs" signal --log-dir "$work/s" "$@"; }

id=$("$bs" start --log-dir "$work/s" --expect ready -- \
    sh -c 'trap "echo got usr1" USR1; (trap "" USR1; exec sleep 30) & echo "kid $!"; echo ready; while :; do wait && break; done' 2>"$work/s1")
kid=$(sed -n 's/^OUT: kid //p' "$work/s1")
sg USR1 "$id"
check "signal by name: exit 0" $? 0
sleep 0.5
log="$work/s/$id/log"
has "the command received it" "$log" '^OUT: got usr1$'
has "the request is noted in the log" "$log" '^babysit: signal [0-9]* sent to the command on request$'
sg sigterm "$id"
check "signal as sigterm: exit 0" $? 0
sleep 0.5
check "the command is gone" "$("$bs" check --log-dir "$work/s" "$id" | cut -d ' ' -f 1)" exited
check "and so is the child it started: the whole group was signalled" \
    "$(kill -0 "$kid" 2>/dev/null && echo alive || echo gone)" gone

sg TERM "$id" 2>"$work/s2"
check "signalling a finished instance: exit 1" $? 1
has "which is explained" "$work/s2" 'is not running'
sg NOPE "$id" 2>/dev/null
check "unknown signal: exit 2" $? 2
sg TERM 2>/dev/null
check "missing identifier: exit 2" $? 2
sg 15 20200101/00/00/abcd 2>/dev/null
check "no such instance: exit 2" $? 2

finish
