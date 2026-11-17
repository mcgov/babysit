#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: timezone = utc
#
# Goal:
#   Identifiers are dated in local time unless babysit.conf says
#   "timezone = utc". The setting is read from --config, or from the default
#   configuration file.
#
# Procedure:
#   1. Run babysit in a time zone fourteen hours ahead of UTC, where the
#      local hour never equals the UTC hour: with no configuration, with
#      "timezone = utc" given through --config, and with the same file in
#      the default place.
#   2. Give a file with a bad value, and name one that does not exist.
#
# Fails if:
#   - the identifier's hour is not the local hour without the setting, or
#     not the UTC hour with it.
#   - a bad value is not reported, or stops the command from running.
#   - a --config file that cannot be read is not a usage error.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

z="TZ=Etc/GMT-14"   # POSIX sign: this is UTC+14
hour() { cut -d / -f 2 "$1"; }
near() { # is hour $1 the hour named by $2, or the one before it?
    local a=$((10#$1)) b=$((10#$2))
    [ "$a" -eq "$b" ] || [ "$a" -eq $(((b + 23) % 24)) ] && echo y
}

env "$z" "$bs" start --log-dir "$work/tz" -- true >"$work/o1" 2>/dev/null
check "local time by default" "$(near "$(hour "$work/o1")" "$(env "$z" date +%H)")" y

echo 'timezone = utc' >"$work/utc.conf"
env "$z" "$bs" start --log-dir "$work/tz2" --config "$work/utc.conf" -- true >"$work/o2" 2>/dev/null
check "UTC with timezone = utc" "$(near "$(hour "$work/o2")" "$(date -u +%H)")" y
check "which is not the local hour" "$(near "$(hour "$work/o2")" "$(env "$z" date +%H)")" ""

mkdir -p "$XDG_CONFIG_HOME/babysit"
echo 'timezone = UTC' >"$XDG_CONFIG_HOME/babysit/babysit.conf"
env "$z" "$bs" start --log-dir "$work/tz2" -- true >"$work/o3" 2>/dev/null
check "the default configuration file is read too" "$(near "$(hour "$work/o3")" "$(date -u +%H)")" y
rm "$XDG_CONFIG_HOME/babysit/babysit.conf"

echo 'timezone = mars' >"$work/bad.conf"
"$bs" start --log-dir "$work/tz3" --config "$work/bad.conf" -- true >/dev/null 2>"$work/e4"
check "a bad value does not stop the command" $? 0
has "but is reported" "$work/e4" 'timezone must be local or utc'
"$bs" start --log-dir "$work/tz" --config "$work/none.conf" -- true >/dev/null 2>&1
check "an unreadable --config is a usage error" $? 2

finish
