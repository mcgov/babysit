#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# End-to-end tests for babysit.
set -u

bindir="bin"

usage() {
    cat <<EOF
Usage: $0 [--bindir DIR]

Options:
  --bindir DIR   directory holding the babysit binary (default: bin)
  --help         show this help
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --bindir) bindir="$2"; shift 2 ;;
        --help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

bindir="$(cd "$bindir" && pwd)" || exit 2
bs="$bindir/babysit"
bsd="$bindir/babysitd"
lp="$bindir/lockprobe"
srcdir="$(cd "$(dirname "$0")/.." && pwd)"
version=$(sed -n 's/^#define BABYSIT_VERSION "\(.*\)"/\1/p' "$srcdir/babysit.h")
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
fails=0

check() { # description, actual, expected
    if [ "$2" = "$3" ]; then
        echo "ok   - $1"
    else
        echo "FAIL - $1: got '$2', expected '$3'"
        fails=$((fails + 1))
    fi
}
has() { # description, file, pattern
    if grep -q -- "$3" "$2"; then
        echo "ok   - $1"
    else
        echo "FAIL - $1: '$3' not in $2:"; sed 's/^/     | /' "$2"
        fails=$((fails + 1))
    fi
}
logof() { sed -n 's/^Log file: //p' "$1"; }
elapsed() { echo $(( $(date +%s) - $1 )); }
quick() { [ "$(elapsed "$1")" -lt "$2" ] && echo y; }

# Every log of this boot goes under <log-dir>/<boot timestamp>/.
"$bs" --log-dir "$work/z" -- true >/dev/null 2>&1
stamp=$(ls "$work/z")
check "boot directory is a timestamp" "$(echo "$stamp" | grep -cE '^[0-9]{8}-[0-9]{6}$')" 1

# --- default: wait, echo, return the exit code ---
"$bs" --log-dir "$work/a" -- sh -c 'echo hi; echo oops >&2; exit 3' >"$work/o1" 2>"$work/e1"
check "default returns the exit code" $? 3
log=$(logof "$work/o1")
check "log path is dir/boot/gen/ppid/pid/log" \
    "$(echo "$log" | grep -cE "^$work/a/$stamp/0/[0-9]+/[0-9]+/log$")" 1
check "log line 1 is the command line" "$(sed -n 1p "$log")" \
    "sh -c 'echo hi; echo oops >&2; exit 3'"
has "log has pid line" "$log" '^Process created with pid [0-9]'
has "log has stdout" "$log" '^OUT: hi$'
has "log has stderr" "$log" '^ERR: oops$'
has "log has exit code" "$log" '^Process exited with code 3$'
has "output is echoed" "$work/o1" '^OUT: hi$'
has "exit is reported on stderr" "$work/e1" 'exited with code 3'
check "echo equals the log" "$(grep -v '^Log file: ' "$work/o1")" "$(cat "$log")"

# --- log name carries babysit's own pid and the child's pid ---
sh -c 'echo $$; exec "$0" --log-dir "$1" -- sh -c "echo \$\$"' "$bs" "$work/g" >"$work/o1b" 2>/dev/null
ppid=$(sed -n 1p "$work/o1b")
cpid=$(sed -n 's/^OUT: //p' "$work/o1b")
check "log is at <babysit pid>/<child pid>/log" \
    "$(logof "$work/o1b")" "$work/g/$stamp/0/$ppid/$cpid/log"
check "babysit pid resolves to the child pid" "$(ls "$work/g/$stamp"/*/"$ppid")" "$cpid"

# --- default log dir is under HOME ---
HOME="$work/home" "$bs" -- true >"$work/o1c" 2>/dev/null
check "default dir is ~/.local/var/log/babysit" \
    "$(logof "$work/o1c" | grep -cE "^$work/home/.local/var/log/babysit/$stamp/0/[0-9]+/[0-9]+/log$")" 1

# --- --detach: return at once, the command keeps going and logging ---
t=$(date +%s)
"$bs" --log-dir "$work/b" --detach -- sh -c 'sleep 2; echo bye' >"$work/o2" 2>&1
check "--detach exits 0" $? 0
check "--detach returns immediately" "$(quick "$t" 2)" y
has "--detach prints pid line" "$work/o2" '^Process created with pid [0-9]'
sleep 3
has "detached command finished" "$(logof "$work/o2")" '^OUT: bye$'

# --- --idle: detect a stall, the command still finishes ---
t=$(date +%s)
"$bs" --log-dir "$work/c" --idle 1 -- sh -c 'echo a; sleep 4; echo b' >"$work/o3" 2>&1
check "--idle reports stall" $? 125
check "--idle returns early" "$(quick "$t" 3)" y
has "--idle says stalled" "$work/o3" 'stalled'
"$bs" --log-dir "$work/c" --idle 2 -- sh -c 'for i in 1 2 3 4; do echo $i; sleep 1; done' >/dev/null 2>&1
check "--idle lets a progressing command finish" $? 0
"$bs" --log-dir "$work/c" --idle 0.5 -- sh -c 'printf "Continue? "; sleep 3' >"$work/o3c" 2>&1
has "stall shows an unterminated prompt" "$work/o3c" '^OUT: Continue? $'
sleep 3
has "stalled command finished in background" "$(logof "$work/o3")" '^OUT: b$'

# --- --expect: return on the line, the command still finishes ---
t=$(date +%s)
"$bs" --log-dir "$work/d" --expect '^ready$' -- sh -c 'echo boot; sleep 1; echo ready; sleep 2; echo done' >"$work/o4" 2>&1
check "--expect matched" $? 0
check "--expect returns early" "$(quick "$t" 3)" y
has "--expect echoes the line" "$work/o4" '^OUT: ready$'
sleep 3
has "expected command finished in background" "$(logof "$work/o4")" '^OUT: done$'
"$bs" --log-dir "$work/d" --expect never -- sh -c 'echo x' >/dev/null 2>&1
check "--expect without match, exit 0 -> 1" $? 1
"$bs" --log-dir "$work/d" --fixed --expect '[::]:80' -- sh -c 'echo "on [::]:80"; sleep 1' >/dev/null 2>&1
check "--fixed matches literally" $? 0

# --- --timeout ---
"$bs" --log-dir "$work/e" --timeout 1 -- sh -c 'for i in 1 2 3 4 5 6 7 8 9 10; do echo x; sleep 0.3; done' >/dev/null 2>&1
check "--timeout" $? 124

# --- failures ---
"$bs" --log-dir "$work/f" -- /no/such/program >/dev/null 2>&1
check "missing command exits 127" $? 127
"$bs" --log-dir "$work/f" -- sh -c 'kill -9 $$' >/dev/null 2>&1
check "signal death is 128+sig" $? 137

# --- the caller's pipe is released on early return and if babysit is killed ---
t=$(date +%s)
: "$("$bs" --log-dir "$work/h" --idle 1 -- sh -c 'echo a; sleep 5' 2>/dev/null)"
check "pipe released after early return" "$(quick "$t" 4)" y
t=$(date +%s)
{ "$bs" --log-dir "$work/h" -- sleep 5 2>/dev/null & echo $! >"$work/pp"; } |
    { sleep 1; kill -9 "$(cat "$work/pp")"; cat >/dev/null; }
check "pipe released when babysit is killed" "$(quick "$t" 4)" y

# --- babysit: command line handling ---
"$bs" --help >"$work/h1" 2>&1
check "--help exits 0" $? 0
has "--help prints usage" "$work/h1" '^Usage: babysit'
check "--version" "$("$bs" --version)" "babysit $version"
"$bs" >/dev/null 2>"$work/h2"
check "no command is a usage error" $? 2
has "usage error prints usage on stderr" "$work/h2" '^Usage: babysit'
"$bs" --bogus -- true >/dev/null 2>&1
check "unknown option is a usage error" $? 2
"$bs" --idle >/dev/null 2>&1
check "option without its value is a usage error" $? 2
for bad in nope 0 -1 5x 1e99 ''; do
    "$bs" --log-dir "$work/n" --idle "$bad" -- true >/dev/null 2>&1
    check "--idle '$bad' is rejected" $? 2
done
"$bs" --log-dir "$work/n" --timeout soon -- true >/dev/null 2>&1
check "bad --timeout is rejected" $? 2
"$bs" --log-dir "$work/n" --expect '(' -- true >/dev/null 2>"$work/h3"
check "bad --expect regex is rejected" $? 2
has "bad regex is explained" "$work/h3" 'bad --expect pattern'
check "nothing is started on a usage error" "$([ -e "$work/n" ] && echo started || echo no)" no
env -u HOME "$bs" -- true >/dev/null 2>"$work/h4"
check "no HOME and no --log-dir is an error" $? 2
has "missing HOME is explained" "$work/h4" 'HOME is not set'
(cd "$work" && "$bs" --log-dir rel -- true >"$work/h5" 2>/dev/null)
check "relative --log-dir is resolved against the cwd" \
    "$(logof "$work/h5" | grep -cE "^$work/rel/$stamp/0/[0-9]+/[0-9]+/log$")" 1
"$bs" --log-dir="$work/eq" --idle=1m --timeout=1h -- true >/dev/null 2>&1
check "--opt=value form, m and h suffixes" $? 0
"$bs" -l "$work/eq" -i 2s -t 0.5 -e x -F -- sh -c 'sleep 2' >/dev/null 2>&1
check "short options, s suffix, fractional seconds" $? 124
"$bs" --log-dir "$work/eq" true >/dev/null 2>&1
check "options end at the first non-option" $? 0
"$bs" --log-dir "$work/eq" -- sh -c 'exit 0' "it's" 'two
lines' '' >"$work/h6" 2>/dev/null
check "command line is shell-quoted on one line" "$(sed -n 1p "$(logof "$work/h6")")" \
    "sh -c 'exit 0' 'it'\\''s' 'two lines' ''"
: >"$work/noexec"
"$bs" --log-dir "$work/eq" -- "$work/noexec" >/dev/null 2>&1
check "non-executable command exits 126" $? 126
check "failed exec leaves no log" "$(find "$work/eq" -name log -exec grep -l noexec {} + | wc -l)" 0
: >"$work/notdir"
"$bs" --log-dir "$work/notdir/x" -- true >/dev/null 2>"$work/h7"
check "uncreatable log directory is an error" $? 2
has "uncreatable log directory is explained" "$work/h7" 'cannot create log'

# --- babysit: output handling ---
"$bs" --log-dir "$work/oh" -- sh -c 'head -c 5000 /dev/zero | tr "\0" x; echo' >"$work/h8" 2>/dev/null
check "long lines are split at 4096 bytes" \
    "$(grep '^OUT: ' "$(logof "$work/h8")" | awk '{ print length($0) - 5 }' | tr '\n' ' ')" "4096 904 "
"$bs" --log-dir "$work/oh" -- sh -c 'printf tail; printf etail >&2' >"$work/h9" 2>/dev/null
check "unterminated last lines are logged" \
    "$(grep -c -e '^OUT: tail$' -e '^ERR: etail$' "$(logof "$work/h9")")" 2
"$bs" --log-dir "$work/oh" -- sh -c 'echo; echo x' >"$work/h10" 2>/dev/null
check "empty lines are kept" "$(sed -n 3p "$(logof "$work/h10")")" "OUT: "
t=$(date +%s)
"$bs" --log-dir "$work/oh" -- sh -c '(sleep 2; echo late) & echo early' >"$work/h11" 2>/dev/null
check "exit is reported while a grandchild holds the pipes" "$?$(quick "$t" 2)" 0y
sleep 3
check "a grandchild's later output still reaches the log" \
    "$(tail -n 2 "$(logof "$work/h11")" | tr '\n' '|')" "Process exited with code 0|OUT: late|"
"$bs" --log-dir "$work/oh" -- sh -c 'echo a; sleep 1; echo b; sleep 1; echo c' 2>/dev/null | head -n 1 >"$work/h12"
check "caller that stops reading lets babysit return 0" "${PIPESTATUS[0]}" 0
sleep 2
has "command keeps logging after the caller stopped reading" "$(logof "$work/h12")" '^OUT: c$'
"$bs" --log-dir "$work/oh" --idle 5 --timeout 5 --expect never -- sh -c 'echo x; exit 9' >/dev/null 2>"$work/h13"
check "all watches armed, command exits first" $? 9
has "early exit names the unmet expectation" "$work/h13" 'exited with code 9 before the expected output'

# --- log locks: a log is locked exactly while its babysitter is writing it ---
"$bs" --log-dir "$work/lk" --detach -- sleep 2 >"$work/l1" 2>/dev/null
check "lock: held while the command runs, after babysit returned" "$("$lp" "$(logof "$work/l1")")" locked
sleep 3
check "lock: released when the command finishes" "$("$lp" "$(logof "$work/l1")")" unlocked
"$bs" --log-dir "$work/lk" --detach -- sleep 30 >"$work/l2" 2>/dev/null
"$bs" --log-dir "$work/lk" --detach -- sleep 2 >"$work/l3" 2>/dev/null
cpid=$(basename "$(dirname "$(logof "$work/l2")")")
sitter=$(ps -o ppid= -p "$cpid" | tr -d ' ')
kill -9 "$sitter"; sleep 0.3
check "lock: released when the babysitter is killed" "$("$lp" "$(logof "$work/l2")")" unlocked
check "lock: not inherited by the command" "$(kill -0 "$cpid" 2>/dev/null && echo alive)" alive
check "lock: each log has its own" "$("$lp" "$(logof "$work/l3")")" locked
kill "$cpid"
check "lock: a missing log counts as unlocked" "$("$lp" "$work/lk/none")" unlocked
"$bs" --log-dir "$work/lk2" --detach -- sleep 3 >"$work/l4" 2>/dev/null
mkdir "$work/lk2/$stamp/1"; touch -d '1 minute ago' "$work/lk2/$stamp/0"
"$bsd" --once --log-dir "$work/lk2" --max-age-days 1 --config /dev/null 2>/dev/null
find "$work/lk2/$stamp/0" -exec touch -d '10 days ago' {} + 2>/dev/null
"$bsd" --once --log-dir "$work/lk2" --max-age-days 1 --config /dev/null 2>/dev/null
check "lock: a locked log is neither packed nor expired" "$("$lp" "$(logof "$work/l4")")" locked
sleep 3.5

# --- pid reuse: our own pid is already filed -> start a new generation ---
sh -c 'mkdir -p "$2/0/$$/999"; echo old >"$2/0/$$/999/log"; echo $$; exec "$0" --log-dir "$1" -- true' \
    "$bs" "$work/r" "$work/r/$stamp" >"$work/o7" 2>/dev/null
check "reused babysit pid: runs anyway" $? 0
rp=$(sed -n 1p "$work/o7")
check "reused babysit pid: logs to generation 1" \
    "$(logof "$work/o7" | grep -cE "^$work/r/$stamp/1/$rp/[0-9]+/log$")" 1
check "reused babysit pid: old log untouched" "$(cat "$work/r/$stamp/0/$rp/999/log")" old
"$bs" --log-dir "$work/r" -- true >"$work/o7b" 2>/dev/null
check "later runs use the newest generation" \
    "$(logof "$work/o7b" | grep -cE "^$work/r/$stamp/1/[0-9]+/[0-9]+/log$")" 1

# --- pid reuse: the child's pid is already filed -> start a new generation ---
# (child pids cannot be chosen, so plant a log for every pid the child could get)
mkdir -p "$work/u/$stamp/0/1"
"$bs" --log-dir "$work/u" -- true >"$work/o9" 2>/dev/null
cp=$(basename "$(dirname "$(logof "$work/o9")")")
lo=$((cp + 1)); hi=$((cp + 400))
eval "mkdir \"$work/u/$stamp/0/1/\"{$lo..$hi}"   # one process: the loop must not use up pids
for ((p = lo; p <= hi; p++)); do echo old >"$work/u/$stamp/0/1/$p/log"; done
"$bs" --log-dir "$work/u" -- true >"$work/o9" 2>/dev/null
cp=$(basename "$(dirname "$(logof "$work/o9")")")
check "child pid landed on a planted log" "$([ "$cp" -ge "$lo" ] && [ "$cp" -le "$hi" ] && echo y)" y
check "reused child pid: logs to generation 1" \
    "$(logof "$work/o9" | grep -cE "^$work/u/$stamp/1/[0-9]+/$cp/log$")" 1
check "reused child pid: old logs untouched" "$(cat "$work/u/$stamp/0/1/$cp/log"; ls "$work/u/$stamp/0/1" | wc -l)" "old
400"

# --- babysitd: pack a superseded generation once it has no writer ---
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

# --- babysit asks a running babysitd to pack as soon as it starts a generation ---
"$bsd" --log-dir "$work/q" --config /dev/null 2>"$work/e14" &   # default interval: 6h
qd=$!
sleep 0.5
check "babysitd: listens on a FIFO" "$([ -p "$work/q/.request" ] && echo fifo)" fifo
sh -c 'mkdir -p "$2/0/$$/999"; echo old >"$2/0/$$/999/log"; exec "$0" --log-dir "$1" -- true' \
    "$bs" "$work/q" "$work/q/$stamp" >/dev/null 2>&1
sleep 5
kill "$qd"; wait "$qd" 2>/dev/null
has "babysitd: woken by babysit" "$work/e14" 'woken by a request'
check "babysitd: packs on request, without waiting for the interval" \
    "$(ls "$work/q/$stamp" | tr '\n' ' ')" "0.tar.gz 1 "
"$bs" --log-dir "$work/q" -- true >/dev/null 2>&1
check "babysit is unaffected by a FIFO nobody reads" $? 0

# --- a hand-made directory layout plus a write to the FIFO drives a full pass ---
"$bsd" --log-dir "$work/k" --max-age-days 5 --config /dev/null 2>"$work/e15" &
kd=$!
sleep 0.5
mkdir -p "$work/k/20240101-000000/0/11/12" "$work/k/$stamp/0/21/22" "$work/k/$stamp/1" "$work/k/archive"
echo earlier-boot >"$work/k/20240101-000000/0/11/12/log"
echo superseded >"$work/k/$stamp/0/21/22/log"
: >"$work/k/archive/babysit_logs_20200101-000000.tar.gz"
touch -d '10 days ago' "$work/k/archive/babysit_logs_20200101-000000.tar.gz"
touch -d '1 minute ago' "$work/k/$stamp/0"      # past the quiet period
echo 1 >"$work/k/.request"
sleep 2
kill "$kd"; wait "$kd" 2>/dev/null
check "request: earlier boot archived, expired archive deleted" \
    "$(ls "$work/k/archive" | tr '\n' ' ')" "babysit_logs_20240101-000000.tar.gz "
check "request: superseded generation packed" "$(ls "$work/k/$stamp" | tr '\n' ' ')" "0.tar.gz 1 "
check "request: packed generation holds the log" \
    "$(tar -xzOf "$work/k/$stamp/0.tar.gz" 0/21/22/log 2>/dev/null)" superseded

# --- babysitd: disk usage limit starts a new generation ---
"$bs" --log-dir "$work/w" -- echo kept >/dev/null 2>&1
: >"$work/w/$stamp/9.tar.gz.partial"; rm -f "$work/w/$stamp/9.tar.gz.partial"
: >"$work/w/$stamp/junk.partial"
"$bsd" --once --log-dir "$work/w" --compact-at-disk-percent 0 --config /dev/null 2>/dev/null
check "babysitd: disk limit 0 does nothing" "$(ls "$work/w/$stamp" | tr '\n' ' ')" "0 "
"$bsd" --once --log-dir "$work/w" --compact-at-disk-percent 1 --config /dev/null 2>"$work/e13"
check "babysitd: disk limit reached starts a generation" "$(ls "$work/w/$stamp" | tr '\n' ' ')" "0 1 "
has "babysitd: disk limit is reported" "$work/e13" 'full (limit 1%)'
sleep 2.5
printf 'compact_at_disk_percent = 1\ncheck_interval = 6h\n' >"$work/conf2"
"$bsd" --once --log-dir "$work/w" --config "$work/conf2" 2>/dev/null
check "babysitd: old generation packed, empty one not superseded" \
    "$(ls "$work/w/$stamp" | tr '\n' ' ')" "0.tar.gz 1 "
check "babysitd: packed generation holds the log" \
    "$(tar -xzOf "$work/w/$stamp/0.tar.gz" 2>/dev/null | grep -c '^OUT: kept$')" 1

# --- babysitd: archive earlier boots as tarballs, expire, rollover warning ---
oldboot=20250101-080000
mkdir -p "$work/d2/$oldboot/0/123/456"
echo x >"$work/d2/$oldboot/0/123/456/log"
"$bs" --log-dir "$work/d2" --detach -- sleep 3 >"$work/o10" 2>/dev/null
live=$(logof "$work/o10")
"$bs" --log-dir "$work/d2" -- true >"$work/o10b" 2>/dev/null
done_log=$(logof "$work/o10b")
"$bsd" --once --log-dir "$work/d2" --config /dev/null 2>/dev/null
tarball="$work/d2/archive/babysit_logs_$oldboot.tar.gz"
check "babysitd: earlier boot packed as babysit_logs_<boot>.tar.gz" \
    "$(tar -xzOf "$tarball" "$oldboot/0/123/456/log" 2>/dev/null)" x
check "babysitd: archived boot directory removed" "$([ -e "$work/d2/$oldboot" ] && echo present || echo gone)" gone
check "babysitd: current boot left in place" "$([ -e "$live" ] && [ -e "$done_log" ] && echo kept)" kept
mkdir -p "$work/d2/$oldboot/0/5/6"; echo again >"$work/d2/$oldboot/0/5/6/log"
"$bsd" --once --log-dir "$work/d2" --config /dev/null 2>/dev/null
check "babysitd: never overwrites an archive" \
    "$(ls "$work/d2/archive" | tr '\n' ' ')" "babysit_logs_$oldboot.2.tar.gz babysit_logs_$oldboot.tar.gz "
"$bsd" --once --log-dir "$work/d2" --max-age-days 5 --config /dev/null 2>/dev/null
check "babysitd: keeps recent archives" "$(ls "$work/d2/archive" | wc -l)" 2
touch -d '10 days ago' "$work/d2"/archive/* "$done_log" "$(dirname "$done_log")" "$(dirname "$(dirname "$done_log")")"
find "$(dirname "$(dirname "$live")")" -exec touch -d '10 days ago' {} +
printf 'max_age_days = 5\n' >"$work/conf"
"$bsd" --once --log-dir "$work/d2" --config "$work/conf" 2>/dev/null
check "babysitd: expires old archives (from config)" "$(ls "$work/d2/archive" | wc -l)" 0
check "babysitd: expires old finished logs" "$([ -e "$done_log" ] && echo present || echo gone)" gone
check "babysitd: expiry leaves active logs" "$([ -e "$live" ] && echo kept)" kept
echo 5000 >"$work/lastpid"
"$bsd" --log-dir "$work/d2" --last-pid-file "$work/lastpid" \
    --check-interval 1s --config /dev/null 2>"$work/e11" &
dpid=$!
sleep 1.5; echo 300 >"$work/lastpid"; sleep 1.5; kill "$dpid"; wait "$dpid" 2>/dev/null
has "babysitd: warns on pid rollover" "$work/e11" 'pid rollover detected (last pid went from 5000 to 300)'

# --- babysitd: command line and configuration ---
"$bsd" --help >"$work/d1" 2>&1
check "babysitd --help exits 0" $? 0
has "babysitd --help prints usage" "$work/d1" '^Usage: babysitd'
check "babysitd --version" "$("$bsd" --version)" "babysitd $version"
"$bsd" --bogus >/dev/null 2>&1
check "babysitd: unknown option is a usage error" $? 2
"$bsd" --log-dir >/dev/null 2>&1
check "babysitd: option without its value is a usage error" $? 2
"$bsd" --once --log-dir "$work/dc" --config /dev/null --max-age-days x 2>/dev/null
check "babysitd: bad --max-age-days is rejected" $? 2
"$bsd" --once --log-dir "$work/dc" --config /dev/null --check-interval soon 2>/dev/null
check "babysitd: bad --check-interval is rejected" $? 2
"$bsd" --once --log-dir "$work/dc" --config /dev/null --compact-at-disk-percent 101 2>/dev/null
check "babysitd: disk percent over 100 is rejected" $? 2
"$bsd" --once --log-dir "$work/dc" --config /dev/null --compact-at-disk-percent x 2>/dev/null
check "babysitd: bad disk percent is rejected" $? 2
"$bsd" --once --log-dir "$work/dc" --config "$work/no-such.conf" 2>"$work/d2e"
check "babysitd: a named config file must exist" $? 2
has "babysitd: missing config is explained" "$work/d2e" 'cannot read'
env -u HOME -u XDG_CONFIG_HOME "$bsd" --once 2>"$work/d3"
check "babysitd: no HOME and no --log-dir is an error" $? 2
: >"$work/blocker"
"$bsd" --once --log-dir "$work/blocker/x" --config /dev/null 2>/dev/null
check "babysitd: uncreatable log directory is an error" $? 2
cat >"$work/messy.conf" <<'CONF'
# a comment
   # an indented comment

max_age_days=7
check_interval = 2d
check_interval = 90
check_interval = 5m
check_interval = soon
compact_at_disk_percent = many
no equals sign here
surprise = 1
CONF
"$bsd" --once --log-dir "$work/dc" --config "$work/messy.conf" --check-interval 1h -a 3 -u 0 2>"$work/d4"
check "babysitd: a config with bad lines still runs" $? 0
has "config: bad duration reported with its line" "$work/d4" 'messy.conf:8: expected a duration'
has "config: bad number reported" "$work/d4" 'messy.conf:9: value must be a non-negative number'
has "config: missing = reported" "$work/d4" "messy.conf:10: expected 'key = value'"
has "config: unknown key reported" "$work/d4" "messy.conf:11: unknown key 'surprise'"
check "config: good lines are not reported" "$(grep -c 'messy.conf' "$work/d4")" 4
mkdir -p "$work/xdg/babysit" "$work/dhome/.config/babysit"
echo 'from_xdg = 1' >"$work/xdg/babysit/babysit.conf"
echo 'from_home = 1' >"$work/dhome/.config/babysit/babysit.conf"
XDG_CONFIG_HOME="$work/xdg" HOME="$work/dhome" "$bsd" --once 2>"$work/d5"
has "config: \$XDG_CONFIG_HOME is used first" "$work/d5" "unknown key 'from_xdg'"
env -u XDG_CONFIG_HOME HOME="$work/dhome" "$bsd" --once 2>"$work/d6"
has "config: ~/.config is the fallback" "$work/d6" "unknown key 'from_home'"
check "babysitd: default log dir is under HOME" "$([ -e "$work/dhome/.local/var/log/babysit/.boot" ] && echo y)" y
env -u XDG_CONFIG_HOME HOME="$work/dhome2" "$bsd" --once 2>/dev/null
check "babysitd: a missing default config is fine" $? 0
(cd "$work" && "$bsd" --once --log-dir reld --config /dev/null 2>/dev/null)
check "babysitd: relative --log-dir is resolved against the cwd" "$([ -e "$work/reld/.boot" ] && echo y)" y

# --- babysitd: failures leave everything in place ---
mkdir -p "$work/tf/20230101-000000/0/1/2" "$work/tf/$stamp/0/3/4" "$work/tf/$stamp/1" "$work/tf/archive"
echo a >"$work/tf/20230101-000000/0/1/2/log"; echo b >"$work/tf/$stamp/0/3/4/log"
: >"$work/tf/$stamp/7.tar.gz.partial"; : >"$work/tf/archive/babysit_logs_x.tar.gz.partial"
touch -d '1 minute ago' "$work/tf/$stamp/0"
PATH=/nonexistent "$bsd" --once --log-dir "$work/tf" --config /dev/null 2>"$work/d7"
check "tar missing: babysitd still exits 0" $? 0
has "tar missing: earlier boot left in place, with a warning" "$work/d7" 'could not archive'
has "tar missing: generation left in place, with a warning" "$work/d7" 'could not pack'
check "tar missing: nothing lost, nothing half-written" \
    "$(cat "$work/tf/20230101-000000/0/1/2/log" "$work/tf/$stamp/0/3/4/log" | tr '\n' ' '; find "$work/tf" -name '*.partial' -o -name '*.tar.gz' | wc -l)" "a b 0"
"$bsd" --once --log-dir "$work/tf" --config /dev/null 2>/dev/null
check "recovers on the next run once tar works" \
    "$(ls "$work/tf/archive" | tr '\n' ' '; ls "$work/tf/$stamp" | tr '\n' ' ')" "babysit_logs_20230101-000000.tar.gz 0.tar.gz 1 "

# --- babysitd: service behaviour ---
mkdir -p "$work/sv/.request"                      # a directory where the FIFO should be
"$bsd" --log-dir "$work/sv" --config /dev/null --last-pid-file "$work/no-such-file" 2>"$work/d8" &
sv=$!
sleep 0.7; kill "$sv"; wait "$sv"
check "babysitd: SIGTERM is a clean exit" $? 0
has "babysitd: unusable FIFO is reported" "$work/d8" 'is not usable'
has "babysitd: unreadable last-pid file is reported" "$work/d8" 'pid rollover is not monitored'
"$bsd" --log-dir "$work/sv2" --config /dev/null 2>/dev/null &
sv=$!
sleep 0.5; kill -INT "$sv"; wait "$sv"
check "babysitd: SIGINT is a clean exit" $? 0
"$bsd" --log-dir "$work/sv3" --config /dev/null --check-interval 1s --max-age-days 5 2>"$work/d9" &
sv=$!
sleep 0.5
mkdir -p "$work/sv3/20220101-000000/0/1/2"; echo z >"$work/sv3/20220101-000000/0/1/2/log"
sleep 2; kill "$sv"; wait "$sv"
check "babysitd: scheduled wake-up does the full pass unprompted" \
    "$(ls "$work/sv3/archive" | tr '\n' ' ')" "babysit_logs_20220101-000000.tar.gz "
check "babysitd: scheduled wake-up is not logged as a request" "$(grep -c 'woken by a request' "$work/d9")" 0

echo
if [ "$fails" -eq 0 ]; then echo "all tests passed"; else echo "$fails test(s) failed"; fi
exit "$fails"
