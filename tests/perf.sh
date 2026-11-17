#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Measure what babysit costs, and keep score.
#
# Each case in tests/perfcases/ times one workload, a command that prints a
# set number of lines, both on its own and under "babysit start": a light
# load (the fixed cost of starting), a medium one and a heavy one (the cost
# per line of output). Every timing is stored in an SQLite database, so
# that each run can be set against the ones before it.
#
# After the cases, a report is printed for the run: for each load, the time
# alone and under babysit and the difference; how the babysit time compares
# with the five runs before; how it compares with the fastest the command
# has ever run without babysit; and a rolling table of the recent runs.
#
# Timings differ between machines, and on one machine with what else it is
# doing. The database is therefore kept per user, outside the repository,
# and the cases never fail on speed: a case fails only if a run goes wrong.
set -u

tests_dir="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$tests_dir/.." && pwd)"
bindir="bin"
db="${XDG_DATA_HOME:-$HOME/.local/share}/babysit/perf.sqlite"
repeat=5
history=10
filter=""
note=""
report_only=0

usage() {
    cat <<USAGE
Usage: $0 [options]

Run the performance cases in tests/perfcases/, store their timings, and
print the report.

Options:
  --bindir DIR    directory holding babysit and perftool (default: $bindir)
  --db FILE       SQLite database the timings are kept in
                  (default: $db)
  --repeat N      take each measurement N times; the report shows the
                  median (default: $repeat)
  --filter TEXT   only run cases whose file name contains TEXT
  --history N     how many runs the rolling table shows (default: $history)
  --note TEXT     a remark to store with this run and show in its report
  --report-only   run nothing: print the report for the latest stored run
  --help          show this help

Needs: python3 (its sqlite3 module holds the database and makes the
report), git (optional: to record which commit was measured).
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --bindir) bindir="$2"; shift 2 ;;
        --db) db="$2"; shift 2 ;;
        --repeat) repeat="$2"; shift 2 ;;
        --filter) filter="$2"; shift 2 ;;
        --history) history="$2"; shift 2 ;;
        --note) note="$2"; shift 2 ;;
        --report-only) report_only=1; shift ;;
        --help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

command -v python3 >/dev/null || { echo "perf: python3 is needed" >&2; exit 2; }
mkdir -p "$(dirname "$db")" || exit 2

if [ "$report_only" -eq 1 ]; then
    exec python3 "$tests_dir/perfdb.py" report --db "$db" --history "$history"
fi

bindir="$(cd "$bindir" && pwd)" || exit 2
results="$(mktemp)"
trap 'rm -f "$results"' EXIT

ran=0
failed=()
for t in "$tests_dir"/perfcases/*-test.sh; do
    name="$(basename "$t")"
    case "$name" in
        *"$filter"*) ;;
        *) continue ;;
    esac
    echo "== $name"
    ran=$((ran + 1))
    if ! bash "$t" --bindir "$bindir" --results "$results" --repeat "$repeat"; then
        failed+=("$name")
    fi
done
if [ "$ran" -eq 0 ]; then
    echo "no performance cases matched"
    exit 2
fi
if [ "${#failed[@]}" -gt 0 ]; then
    echo
    echo "${#failed[@]} of $ran performance cases failed; nothing was stored:"
    printf '  %s\n' "${failed[@]}"
    exit 1
fi

# Which commit was measured, and whether the tree had changes on top of it.
commit="$(git -C "$root" rev-parse --short HEAD 2>/dev/null || echo unknown)"
record=(--db "$db" --results "$results" --commit "$commit" --host "$(uname -n)")
[ -n "$(git -C "$root" status --porcelain 2>/dev/null)" ] && record+=(--dirty)
[ -n "$note" ] && record+=(--note "$note")
run="$(python3 "$tests_dir/perfdb.py" record "${record[@]}")" || exit 2

echo
python3 "$tests_dir/perfdb.py" report --db "$db" --run "$run" --history "$history"
