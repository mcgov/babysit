#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Run the end-to-end test cases in tests/testcases/, in order.
#
# Each NNNN-name-test.sh there is a self-contained script with a header that
# states its goal, its procedure and what makes it fail. They are run one at
# a time: some of them depend on pids being handed out in sequence, which
# running them in parallel would disturb.
set -u

tests_dir="$(cd "$(dirname "$0")" && pwd)"
bindir="bin"
filter=""

usage() {
    cat <<EOF
Usage: $0 [--bindir DIR] [--filter TEXT]

Options:
  --bindir DIR    directory holding babysit, babysitd and lockprobe
                  (default: bin)
  --filter TEXT   only run test cases whose file name contains TEXT
  --help          show this help
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --bindir) bindir="$2"; shift 2 ;;
        --filter) filter="$2"; shift 2 ;;
        --help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

bindir="$(cd "$bindir" && pwd)" || exit 2

ran=0
failed=()
for t in "$tests_dir"/testcases/*-test.sh; do
    name="$(basename "$t")"
    case "$name" in
        *"$filter"*) ;;
        *) continue ;;
    esac
    echo "== $name"
    ran=$((ran + 1))
    if ! bash "$t" --bindir "$bindir"; then
        failed+=("$name")
    fi
done

echo
if [ "$ran" -eq 0 ]; then
    echo "no test cases matched"
    exit 2
fi
if [ "${#failed[@]}" -eq 0 ]; then
    echo "all tests passed ($ran test cases)"
    exit 0
fi
echo "${#failed[@]} of $ran test cases failed:"
printf '  %s\n' "${failed[@]}"
exit 1
