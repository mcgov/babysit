#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: babysitd: --help, --version and usage errors
#
# Goal:
#   Informational options exit 0; every malformed invocation exits 2.
#
# Procedure:
#   1. Run --help and --version.
#   2. Run with an unknown option, an option missing its value, invalid
#      numbers and durations, a named config file that does not exist, no
#      HOME and no --log-dir, and a log directory that cannot be created.
#
# Fails if:
#   - --help or --version does not exit 0 or prints the wrong text.
#   - any malformed invocation exits with a status other than 2.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

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

finish
