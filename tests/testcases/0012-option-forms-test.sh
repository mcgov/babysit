#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: babysit: the forms options can take
#
# Goal:
#   Options work as "--opt value", "--opt=value" and "-o value"; intervals
#   accept s, m and h suffixes and fractions; a relative --log-dir is
#   resolved against the current directory; options stop at the first word
#   that is not one.
#
# Procedure:
#   1. Run with a relative --log-dir from a known directory.
#   2. Run with --opt=value forms and m/h suffixes.
#   3. Run with short options, an s suffix and a fractional timeout against a
#      command that outlasts it.
#   4. Run with the command directly after the options, without "--".
#
# Fails if:
#   - the relative directory is not resolved against the current directory.
#   - any accepted form is rejected, or the fractional timeout does not
#     expire with status 124.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

(cd "$work" && "$bs" start --log-dir rel -- true >"$work/h5" 2>/dev/null)
check "relative --log-dir is resolved against the cwd" \
    "$(logof "$work/h5")" "$work/rel/$(cat "$work/h5")/log"
"$bs" start --log-dir="$work/eq" --idle=1m --timeout=1h -- true >/dev/null 2>&1
check "--opt=value form, m and h suffixes" $? 0
"$bs" start -l "$work/eq" -i 2s -t 0.5 -e x -F -- sh -c 'sleep 2' >/dev/null 2>&1
check "short options, s suffix, fractional seconds" $? 124
"$bs" start --log-dir "$work/eq" true >/dev/null 2>&1
check "options end at the first non-option" $? 0

finish
