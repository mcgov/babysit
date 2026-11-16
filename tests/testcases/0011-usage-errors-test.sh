#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: babysit: --help, --version and usage errors
#
# Goal:
#   Informational options exit 0; every malformed invocation exits 2 before
#   anything is started.
#
# Procedure:
#   1. Run --help and --version.
#   2. Run with no command, an unknown option, an option missing its value, a
#      series of invalid intervals, an invalid regular expression, and with
#      HOME unset and no --log-dir.
#
# Fails if:
#   - --help or --version does not exit 0 or prints the wrong text.
#   - any malformed invocation exits with a status other than 2, or does not
#     explain itself.
#   - the log directory was created, which would mean something was started
#     despite the error.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

"$bs" --help >"$work/h1" 2>&1
check "--help exits 0" $? 0
has "--help prints usage" "$work/h1" '^Usage: babysit'
check "--version" "$("$bs" --version)" "babysit $version"
"$bs" >/dev/null 2>"$work/h0"
check "no subcommand is a usage error" $? 2
has "and prints usage on stderr" "$work/h0" '^Usage: babysit start'
"$bs" --log-dir "$work/n" -- true >/dev/null 2>"$work/h0"
check "the subcommand cannot be left out" $? 2
"$bs" frobnicate >/dev/null 2>"$work/h0"
check "an unknown subcommand is a usage error" $? 2
has "and is named" "$work/h0" "unknown command 'frobnicate'"
"$bs" start >/dev/null 2>"$work/h2"
check "no command is a usage error" $? 2
has "usage error prints usage on stderr" "$work/h2" '^Usage: babysit'
"$bs" start --bogus -- true >/dev/null 2>&1
check "unknown option is a usage error" $? 2
"$bs" start --idle >/dev/null 2>&1
check "option without its value is a usage error" $? 2
for bad in nope 0 -1 5x 1e99 ''; do
    "$bs" start --log-dir "$work/n" --idle "$bad" -- true >/dev/null 2>&1
    check "--idle '$bad' is rejected" $? 2
done
"$bs" start --log-dir "$work/n" --timeout soon -- true >/dev/null 2>&1
check "bad --timeout is rejected" $? 2
"$bs" start --log-dir "$work/n" --expect '(' -- true >/dev/null 2>"$work/h3"
check "bad --expect regex is rejected" $? 2
has "bad regex is explained" "$work/h3" 'bad --expect pattern'
check "nothing is started on a usage error" "$([ -e "$work/n" ] && echo started || echo no)" no
env -u HOME "$bs" start -- true >/dev/null 2>"$work/h4"
check "no HOME and no --log-dir is an error" $? 2
has "missing HOME is explained" "$work/h4" 'HOME is not set'

finish
