# SPDX-License-Identifier: MIT
# shellcheck shell=bash disable=SC2034
#
# Shared setup and helpers for the test cases in tests/testcases/.
#
# Every test case sources this file first and calls "finish" last. A test
# case is an ordinary script and can be run on its own:
#
#     tests/testcases/0006-idle-stall-test.sh --bindir bin
#
# What sourcing this file provides:
#
#   $bs, $bsd, $lp   paths of babysit, babysitd and the lockprobe helper
#   $work            a scratch directory, removed when the test exits
#   $stamp           the name of the current boot's log directory
#   $version         the version defined in babysit.h
#   $old1m, $old10d  "touch -t" timestamps one minute and ten days ago
#
#   check DESC ACTUAL EXPECTED   pass if the two strings are equal
#   has DESC FILE PATTERN        pass if grep finds PATTERN in FILE
#   logof FILE                   the log path babysit printed into FILE
#   ago SECONDS                  a "touch -t" timestamp that long ago
#   count                        count lines on stdin (no padding)
#   quick START LIMIT            print "y" if fewer than LIMIT seconds have
#                                passed since START (from "date +%s")
#   finish                       exit 0 if every check passed, else 1
set -u

tests_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
srcdir="$(cd "$tests_dir/.." && pwd)"
bindir="$srcdir/bin"
keep=0

lib_usage() {
    cat <<EOF
Usage: $0 [--bindir DIR] [--keep]

Options:
  --bindir DIR   directory holding babysit, babysitd and lockprobe
                 (default: $srcdir/bin)
  --keep         do not remove the scratch directory; print its path instead
  --help         show this help
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --bindir) bindir="$2"; shift 2 ;;
        --keep) keep=1; shift ;;
        --help) lib_usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; lib_usage >&2; exit 2 ;;
    esac
done

bindir="$(cd "$bindir" && pwd)" || exit 2
bs="$bindir/babysit"
bsd="$bindir/babysitd"
lp="$bindir/lockprobe"
for tool in "$bs" "$bsd" "$lp"; do
    [ -x "$tool" ] || { echo "missing $tool: build it first (make all bin/lockprobe)" >&2; exit 2; }
done
version=$(sed -n 's/^#define BABYSIT_VERSION "\(.*\)"/\1/p' "$srcdir/babysit.h")

work="$(mktemp -d)"
lib_cleanup() {
    if [ "$keep" -eq 1 ]; then
        echo "scratch directory kept: $work"
    else
        rm -rf "$work"
    fi
}
trap lib_cleanup EXIT
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
        echo "FAIL - $1: '$3' not in $2:"
        sed 's/^/     | /' "$2"
        fails=$((fails + 1))
    fi
}

logof() { sed -n 's/^Log file: //p' "$1"; }

# touch -t stamp for N seconds ago (GNU date, then BSD date)
ago() {
    local t=$(( $(date +%s) - $1 ))
    date -d "@$t" +%Y%m%d%H%M.%S 2>/dev/null || date -r "$t" +%Y%m%d%H%M.%S
}

count() { wc -l | tr -d ' '; }      # BSD wc pads its output

elapsed() { echo $(( $(date +%s) - $1 )); }
quick() { [ "$(elapsed "$1")" -lt "$2" ] && echo y; }

finish() {
    [ "$fails" -eq 0 ]
    exit $?
}

old10d=$(ago 864000)
old1m=$(ago 60)

# Every log of this boot goes under <log-dir>/<boot timestamp>/; learn the
# name once by letting babysit create it in a directory of its own.
"$bs" --log-dir "$work/.stamp" -- true >/dev/null 2>&1
stamp=$(ls "$work/.stamp")
