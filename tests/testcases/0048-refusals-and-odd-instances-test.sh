#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: refusals and odd instances
#
# Goal:
#   The less-travelled answers are right too: what the look-at-it commands
#   say about an instance that has exited or was left behind by a killed
#   babysitter, and how every command refuses arguments, paths and logs it
#   cannot use. A refusal is exit 2 with a reason, never a hang or a crash.
#
# Procedure:
#   1. Run a command that exits 3; ask info for the whole picture.
#   2. Lay out by hand an instance with a log but no babysitter and no
#      exit status, whose log is one very long line; ask info, check and
#      watch about it; then give it an empty exit_code file and ask again.
#   3. Give watch a bad time, a bad pattern, a pattern that is only valid
#      as a fixed string, and an unknown option; give cat an unknown
#      option, check one argument too many, signal an overlong name.
#   4. Start with a time limit far below a millisecond.
#   5. Name a log directory whose path is too long, and a relative one
#      from a working directory that no longer exists, to both programs.
#      Give babysitd a log directory where ".lock" is a directory.
#   6. Make a log unreadable; cat it and watch it (not as root).
#   7. Set timezone to "local", and to something with words after it.
#   8. Write lines that are not signal numbers to an instance's control
#      FIFO, then ask for a real signal.
#
# Fails if:
#   - info does not report "exited" with the status, or "lost".
#   - a lost instance is not exit 1 from check and exit 2 from watch, or
#     watch gets stuck on its endless line.
#   - any of the bad arguments is not refused with exit 2 and a reason.
#   - a fixed-string pattern is compiled as a regular expression.
#   - a tiny time limit is treated as no limit: start must return 124.
#   - an overlong or unresolvable log directory is not refused by both.
#   - babysitd runs a pass without being able to hold its lock.
#   - an unreadable log is not reported as such.
#   - "timezone = local" is not accepted quietly, or a bad value stops
#     babysit from running the command.
#   - nonsense on the control FIFO harms the command, is logged as a signal
#     sent, or stops later requests from working.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

L="$work/l"
b() { local c=$1; shift; "$bs" "$c" --log-dir "$L" "$@"; }

id=$(b start -- sh -c 'echo hi; exit 3' 2>/dev/null)
b info "$id" >"$work/i1"
has "info on a finished instance: its state" "$work/i1" '^state: exited$'
has "and its exit status" "$work/i1" '^exit_code: 3$'

lost=20200101/00/00/abcd
mkdir -p "$L/$lost"
head -c 300000 /dev/zero | tr '\0' x >"$L/$lost/log"
b info "$lost" >"$work/i2"
has "info on an instance with no babysitter and no status: lost" "$work/i2" '^state: lost$'
check "check says so and exits 1" "$(b check "$lost"; echo $?)" "lost
1"
t=$(date +%s)
b watch "$lost" >/dev/null 2>"$work/e1"
check "watch gives up on it at once: exit 2" "$? $(quick "$t" 3)" "2 y"
has "and says why" "$work/e1" 'left no exit status'
: >"$L/$lost/exit_code"
check "an empty exit_code file is no exit status" "$(b check "$lost"; echo $?)" "lost
1"

refused() { # description, pattern expected on stderr, command...
    local d=$1 p=$2
    shift 2
    "$@" >/dev/null 2>"$work/err"
    check "$d: exit 2" $? 2
    has "$d: explained" "$work/err" "$p"
}
refused "watch with a bad time" "invalid --timeout interval 'soon'" b watch "$id" --timeout soon
refused "watch with a bad pattern" 'bad --expect pattern' b watch "$id" --expect '('
refused "watch with an unknown option" "unknown option '--bogus'" b watch --bogus "$id"
refused "cat with an unknown option" "unknown option '--bogus'" b cat --bogus "$id"
refused "check with two identifiers" 'too many arguments' b check "$id" "$id"
refused "signal with an overlong name" 'unknown signal' b signal SIGABCDEFGHIJKLMNOPQRSTUVWXYZ "$id"
b watch "$id" --fixed --expect '(' >/dev/null 2>&1
check "the same pattern as a fixed string is accepted" $? 3

t=$(date +%s)
tid=$(b start --timeout 0.0001 -- sleep 5 2>/dev/null)
check "a limit below a millisecond is still a limit" "$? $(quick "$t" 3)" "124 y"
b signal KILL "$tid"

long="/$(printf 'x%.0s' $(seq 5000))"
refused "babysit, log directory path too long" 'log path too long' "$bs" check --log-dir "$long" "$id"
refused "babysitd, log directory path too long" 'log path too long' "$bsd" --once --log-dir "$long"
mkdir "$work/gone"
cd "$work/gone" && rmdir "$work/gone"
refused "babysit, relative log directory from a removed directory" 'getcwd' "$bs" check --log-dir rel "$id"
refused "babysitd, relative log directory from a removed directory" 'getcwd' "$bsd" --once --log-dir rel
cd "$work" || exit 2
mkdir -p "$work/nolock/.lock"
refused "babysitd, a directory where its lock file goes" 'cannot lock' "$bsd" --once --log-dir "$work/nolock"

if [ "$(id -u)" -eq 0 ]; then
    echo "ok   - skipped: root can open any file"
else
    chmod 000 "$L/$id/log"
    refused "cat of an unreadable log" 'cannot read' b cat "$id"
    refused "watch of an unreadable log" 'cannot read' b watch "$id"
    chmod 644 "$L/$id/log"
fi

mkdir -p "$XDG_CONFIG_HOME/babysit"
echo 'timezone = local' >"$XDG_CONFIG_HOME/babysit/babysit.conf"
b start -- true >"$work/o2" 2>"$work/e2"
check "timezone = local: runs" $? 0
check "with no complaint" "$(grep -c 'timezone' "$work/e2")" 0
check "and dates the instance in local time" "$(idof "$work/o2" | cut -d/ -f1)" "$(date +%Y%m%d)"
echo 'timezone = utc please' >"$XDG_CONFIG_HOME/babysit/babysit.conf"
b start -- true >/dev/null 2>"$work/e3"
check "a timezone with words after it: the command still runs" $? 0
has "and the setting is complained about" "$work/e3" 'timezone must be local or utc'

echo 'timezone = local' >"$XDG_CONFIG_HOME/babysit/babysit.conf"
sid=$(b start --detach -- sleep 30 2>/dev/null)
printf 'nonsense\n0\n9999\n' >"$L/$sid/ctl"
sleep 0.3
check "nonsense written to the control FIFO: the command runs on" "$(b check "$sid")" running
check "and nothing is logged as sent" "$(grep -c 'sent to the command' "$L/$sid/log")" 0
b signal TERM "$sid"
check "a real request after it is still delivered" "$(b wait "$sid" --timeout 5; echo $?)" 143

finish
