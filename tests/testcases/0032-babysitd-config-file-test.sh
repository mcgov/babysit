#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: babysitd: parsing babysit.conf
#
# Goal:
#   Good lines of the configuration file are applied silently; each bad line
#   is reported with its file and line number and skipped; babysitd still
#   runs.
#
# Procedure:
#   1. Write a config with comments, blank lines, valid settings in several
#      spellings, a bad duration, a bad number, a line with no "=", and an
#      unknown key.
#   2. Run babysitd once with it.
#
# Fails if:
#   - babysitd does not exit 0.
#   - any of the four bad lines is not reported with the right line number.
#   - a valid line is reported.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

cat >"$work/messy.conf" <<'CONF'
# a comment
   # an indented comment

max_age_days=7
check_interval = 2d
check_interval = 90
check_interval = 5m
check_interval = soon
archive_on_usage_percentage = many
no equals sign here
surprise = 1
timezone = utc
timezone = mars
CONF
"$bsd" --once --log-dir "$work/dc" --config "$work/messy.conf" --check-interval 1h -a 3 -u 0 2>"$work/d4"
check "babysitd: a config with bad lines still runs" $? 0
has "config: bad duration reported with its line" "$work/d4" 'messy.conf:8: expected a duration'
has "config: bad number reported" "$work/d4" 'messy.conf:9: value must be a non-negative number'
has "config: missing = reported" "$work/d4" "messy.conf:10: expected 'key = value'"
has "config: unknown key reported" "$work/d4" "messy.conf:11: unknown key 'surprise'"
has "config: bad timezone reported" "$work/d4" 'messy.conf:13: timezone must be local or utc'
check "config: good lines are not reported" "$(grep -c 'messy.conf' "$work/d4")" 5

finish
