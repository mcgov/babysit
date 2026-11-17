# SPDX-License-Identifier: MIT
# shellcheck shell=bash disable=SC2034
#
# Shared setup and helpers for the performance cases in tests/perfcases/.
#
# A performance case sources this file, calls "measure" once, and calls
# "finish" last. It is normally run by tests/perf.sh, which collects the
# samples of all cases into the database and prints the report, but it can
# be run on its own to see the raw samples:
#
#     tests/perfcases/0002-medium-load-test.sh --bindir bin
#
# What sourcing this file provides:
#
#   $bs, $pt   paths of babysit and the perftool helper
#   $work      a scratch directory, removed when the case exits
#   $repeat    how many times each measurement is taken
#
#   measure LOAD LINES WIDTH   time "perftool gen LINES WIDTH" REPEAT times
#                              on its own and REPEAT times under babysit,
#                              and record one sample line for each
#   finish                     exit 0 if every check passed, else 1
#
# A sample line is "LOAD MODE SECONDS LINES BYTES", MODE being "bare" or
# "babysit". They go to the file named by --results, or to stdout.
set -u

tests_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
srcdir="$(cd "$tests_dir/.." && pwd)"
bindir="$srcdir/bin"
results=""
repeat=5
keep=0

perflib_usage() {
    cat <<USAGE
Usage: $0 [--bindir DIR] [--results FILE] [--repeat N] [--keep]

Options:
  --bindir DIR     directory holding babysit and perftool
                   (default: $srcdir/bin)
  --results FILE   append the samples to FILE (default: print them)
  --repeat N       take each measurement N times (default: $repeat)
  --keep           do not remove the scratch directory; print its path
  --help           show this help
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --bindir) bindir="$2"; shift 2 ;;
        --results) results="$2"; shift 2 ;;
        --repeat) repeat="$2"; shift 2 ;;
        --keep) keep=1; shift ;;
        --help) perflib_usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; perflib_usage >&2; exit 2 ;;
    esac
done

bindir="$(cd "$bindir" && pwd)" || exit 2
bs="$bindir/babysit"
pt="$bindir/perftool"
for tool in "$bs" "$pt"; do
    [ -x "$tool" ] || { echo "missing $tool: build it first (make all bin/perftool)" >&2; exit 2; }
done

work="$(mktemp -d)"
perflib_cleanup() {
    if [ "$keep" -eq 1 ]; then
        echo "scratch directory kept: $work"
    else
        rm -rf "$work"
    fi
}
trap perflib_cleanup EXIT
fails=0

# Measure in local time whatever the invoking user's babysit.conf says.
export XDG_CONFIG_HOME="$work/.config"

sample() { # load mode seconds lines bytes
    if [ -n "$results" ]; then
        echo "$*" >>"$results"
    else
        echo "sample $*"
    fi
}

problem() {
    echo "FAIL - $*"
    fails=$((fails + 1))
}

# The two things being compared write the same bytes to the same
# filesystem: on its own, the command's output is redirected to a file;
# under babysit it goes to the log. babysit's echo of the output goes to
# /dev/null, as does the identifier.
measure() { # load lines width
    local load="$1" lines="$2" width="$3" bytes i id got alone
    bytes=$((lines * (width + 1)))
    for i in $(seq 1 "$repeat"); do
        "$pt" time "$work/t" -- sh -c 'exec "$1" gen "$2" "$3" >"$4"' sh "$pt" "$lines" "$width" "$work/bare.out" ||
            problem "$load: the command failed on its own"
        alone=$(cat "$work/t")
        sample "$load" bare "$alone" "$lines" "$bytes"
        got=$(wc -l <"$work/bare.out" | tr -d ' ')
        [ "$got" -eq "$lines" ] || problem "$load: bare run wrote $got lines, not $lines"
        rm -f "$work/bare.out"

        "$pt" time "$work/t" -- sh -c 'exec "$1" start --log-dir "$2" -- "$3" gen "$4" "$5" >"$6" 2>/dev/null' \
            sh "$bs" "$work/logs" "$pt" "$lines" "$width" "$work/id" ||
            problem "$load: babysit start failed"
        sample "$load" babysit "$(cat "$work/t")" "$lines" "$bytes"
        # A line per sample: whoever watches this run sees that it is alive.
        echo "   $load $i/$repeat: alone ${alone}s, under babysit $(cat "$work/t")s"
        id=$(cat "$work/id")
        got=$(grep -c '^OUT: ' "$work/logs/$id/log")
        [ "$got" -eq "$lines" ] || problem "$load: the log holds $got lines of output, not $lines"
        rm -rf "$work/logs"
    done
}

finish() {
    [ "$fails" -eq 0 ]
    exit $?
}
