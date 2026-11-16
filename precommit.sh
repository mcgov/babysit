#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Checks to pass before committing:
#   1. clang-format   every C source matches the LLVM style (.clang-format)
#   2. clang-tidy     static analysis, every finding an error (.clang-tidy)
#   3. build          everything compiles with warnings as errors
set -u

fix=0
install=0
cc="clang"

usage() {
    cat <<USAGE
Usage: $0 [--fix] [--install] [--cc COMPILER]

Options:
  --fix          reformat the sources in place instead of only checking them
  --install      install this script as the repository's git pre-commit hook
  --cc COMPILER  compiler for the warnings-as-errors build (default: $cc)
  --help         show this help

Needs clang-format and clang-tidy (package clang-tools-extra on Fedora,
clang-format and clang-tidy on Debian).
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --fix) fix=1; shift ;;
        --install) install=1; shift ;;
        --cc) cc="$2"; shift 2 ;;
        --help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

root="$(cd "$(dirname "$0")" && pwd)"
cd "$root" || exit 2

if [ "$install" -eq 1 ]; then
    hook="$(git rev-parse --git-path hooks/pre-commit)" || exit 2
    printf '#!/bin/sh\nexec "%s/precommit.sh"\n' "$root" >"$hook" && chmod +x "$hook" || exit 2
    echo "installed $hook"
    exit 0
fi

for tool in clang-format clang-tidy "$cc" make; do
    command -v "$tool" >/dev/null || { echo "precommit: missing required tool: $tool" >&2; exit 2; }
done

sources=(babysit.c babysit_*.c babysit_internal.h babysitd.c babysit.h tests/lockprobe.c)
cflags=(-std=c99 -Wall -Wextra -pedantic -Werror)
status=0

echo "== clang-format (LLVM style)"
if [ "$fix" -eq 1 ]; then
    clang-format -i "${sources[@]}" || status=1
elif ! clang-format --dry-run --Werror "${sources[@]}"; then
    echo "precommit: formatting differs; run '$0 --fix'" >&2
    status=1
fi

echo "== clang-tidy"
for f in babysit.c babysit_*.c babysitd.c tests/lockprobe.c; do
    clang-tidy --quiet "$f" -- "${cflags[@]}" || status=1
done

echo "== build with warnings as errors"
if ! make -s CC="$cc" BINDIR=build/precommit \
        CFLAGS="-O2 -g -Wall -Wextra -pedantic -Werror" all build/precommit/lockprobe; then
    status=1
fi

if [ "$status" -eq 0 ]; then
    echo "precommit: ok"
else
    echo "precommit: FAILED" >&2
fi
exit "$status"
