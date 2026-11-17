#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: library calls that fail
#
# Goal:
#   Running out of memory, processes or descriptors at any single point
#   costs nothing but the operation in hand. "babysit start" either runs
#   the command properly or refuses with exit 2 and leaves no instance
#   behind; "babysitd" never loses a log, whatever fails under it, and the
#   next pass finishes the job.
#
#   The failures are made by tests/faultinject.c, a library preloaded into
#   the two programs that makes the Nth call of one function fail. Each
#   scenario is run for N = 1, 2, 3... until a run goes by with no call
#   left to fail, so every call of that function is visited once.
#
# Procedure:
#   1. For each of malloc, realloc, fork, pipe, mkfifo, localtime: run
#      "babysit start -- sh -c 'echo out; echo err >&2'" with the Nth call
#      failing, in a fresh log directory, for every N.
#   2. For each of malloc, realloc, fork, statvfs, fsync, opendir, mkfifo,
#      localtime: lay out three finished instances on two past days, and
#      run "babysitd --once" with the Nth call failing, for every N. Then
#      run it once more with nothing failing.
#
# Fails if:
#   - babysit ends with a status other than 0 or 2 (a crash is 128+signal),
#     or does not end within ten seconds.
#   - it exits 2 without a reason on stderr, or without an identifier but
#     with something left in the log directory.
#   - it exits 0 without the command's output in the log and its exit
#     status recorded.
#   - babysitd crashes or hangs, or afterwards a planted log is neither
#     where it was nor in an archive.
#   - the pass after the failure does not archive all three logs.
#   - a function is never called at all (the test would prove nothing).
#
# Skipped where the library cannot be preloaded. In ASan and TSan builds,
# which bring their own malloc, malloc and realloc are left out.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

so="$bindir/faultinject.so"
flog="$work/faults"
limit=10
command -v timeout >/dev/null || limit=""

# run FUNCTION N COMMAND...: the command with that call failing
run() {
    local f=$1 n=$2
    shift 2
    : >"$flog"
    LD_PRELOAD="$so" BABYSIT_FAULT="$f:$n" BABYSIT_FAULT_LOG="$flog" \
        ${limit:+timeout "$limit"} "$@"
}
struck() { [ -s "$flog" ]; }

if [ -f "$so" ]; then
    run pipe 1 "$bs" start --log-dir "$work/probe" -- true >/dev/null 2>&1
fi
if [ ! -f "$so" ] || ! struck; then
    echo "ok   - skipped: faultinject.so cannot be preloaded into this build"
    finish
fi

# ASan and TSan have their own malloc, which the library cannot get in
# front of; the other functions can still be made to fail there.
mem="malloc realloc"
run malloc 1 "$bs" start --log-dir "$work/probe" -- true >/dev/null 2>&1
if ! struck; then
    echo "ok   - skipped for malloc and realloc: this build has its own"
    mem=""
fi

# ---- babysit start ----
for f in $mem fork pipe mkfifo localtime; do
    bad=""
    n=0
    while [ "$n" -lt 500 ]; do
        n=$((n + 1))
        L="$work/s-$f-$n"
        run "$f" "$n" "$bs" start --log-dir "$L" -- sh -c 'echo out; echo err >&2' \
            >"$work/o" 2>"$work/e"
        rc=$?
        struck || break
        id=$(idof "$work/o")
        case "$rc" in
            0)  [ "$(cat "$L/$id/exit_code" 2>/dev/null)" = 0 ] &&
                    grep -q '^OUT: out$' "$L/$id/log" &&
                    grep -q '^ERR: err$' "$L/$id/log" ||
                    bad="$bad call $n: exit 0 but the run is not all there;" ;;
            2)  grep -q '^babysit' "$work/e" || bad="$bad call $n: refused without a reason;"
                [ -n "$id" ] || [ "$(find "$L" -mindepth 1 2>/dev/null | count)" -eq 0 ] ||
                    bad="$bad call $n: refused, no identifier, but files left;" ;;
            *)  bad="$bad call $n: exit $rc;" ;;
        esac
    done
    check "babysit start, each $f failing in turn ($((n - 1)) calls)" "$bad" ""
    case "$f" in
        realloc) ;; # start may well never call it
        *) check "  and $f is called at all" "$([ "$n" -gt 1 ] && echo y)" y ;;
    esac
done

# ---- babysitd ----
d1=$(daysago 2)
d2=$(daysago 3)
ids="$d1/00/01/aaaa $d1/05/30/bbbb $d2/23/59/cccc"
layout() { # DIR: three finished instances on two past days
    local i
    for i in $ids; do plant "$1" "$i" "log of $i"; done
}
kept() { # DIR ID: is the log where it was, or in an archive?
    [ "$(cat "$1/$2/log" 2>/dev/null)" = "log of $2" ] && return 0
    local a
    for a in "$1"/archive/*.tar.gz; do
        [ -f "$a" ] && tar -xzOf "$a" 2>/dev/null | grep -q "^log of $2\$" && return 0
    done
    return 1
}
pass=("$bsd" --once --config /dev/null -u 100 -D 100 --log-dir)

for f in $mem fork statvfs fsync opendir mkfifo localtime; do
    bad=""
    n=0
    while [ "$n" -lt 500 ]; do
        n=$((n + 1))
        L="$work/d-$f-$n"
        layout "$L"
        run "$f" "$n" "${pass[@]}" "$L" >/dev/null 2>&1
        rc=$?
        struck || break
        [ "$rc" -eq 0 ] || [ "$rc" -eq 2 ] || bad="$bad call $n: exit $rc;"
        for i in $ids; do
            kept "$L" "$i" || bad="$bad call $n: lost $i;"
        done
        "${pass[@]}" "$L" >/dev/null 2>&1
        for i in $ids; do
            [ ! -e "$L/$i/log" ] && kept "$L" "$i" || bad="$bad call $n: $i not archived by the next pass;"
        done
    done
    check "babysitd, each $f failing in turn ($((n - 1)) calls)" "$bad" ""
    case "$f" in
        localtime | mkfifo) ;; # a single pass need not call these
        *) check "  and $f is called at all" "$([ "$n" -gt 1 ] && echo y)" y ;;
    esac
done

finish
