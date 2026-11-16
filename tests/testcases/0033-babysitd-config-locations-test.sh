#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: babysitd: where the config and the logs are looked for
#
# Goal:
#   Without --config, the file is $XDG_CONFIG_HOME/babysit/babysit.conf,
#   else ~/.config/babysit/babysit.conf, and it may be absent. Without
#   --log-dir, the log directory is under HOME. A relative --log-dir is
#   resolved against the current directory. Each config holds an unknown key
#   so that the warning shows which file was read.
#
# Procedure:
#   1. Run with XDG_CONFIG_HOME and HOME both pointing at configs.
#   2. Run with only HOME.
#   3. Run with a HOME that has no config.
#   4. Run with a relative --log-dir.
#
# Fails if:
#   - the wrong file is read in either case.
#   - the default log directory is not created under HOME.
#   - a missing default config is treated as an error.
#   - the relative directory is not resolved against the current directory.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

mkdir -p "$work/xdg/babysit" "$work/dhome/.config/babysit"
echo 'from_xdg = 1' >"$work/xdg/babysit/babysit.conf"
echo 'from_home = 1' >"$work/dhome/.config/babysit/babysit.conf"
XDG_CONFIG_HOME="$work/xdg" HOME="$work/dhome" "$bsd" --once 2>"$work/d5"
has "config: \$XDG_CONFIG_HOME is used first" "$work/d5" "unknown key 'from_xdg'"
env -u XDG_CONFIG_HOME HOME="$work/dhome" "$bsd" --once 2>"$work/d6"
has "config: ~/.config is the fallback" "$work/d6" "unknown key 'from_home'"
check "babysitd: default log dir is under HOME" "$([ -e "$work/dhome/.local/var/log/babysit/.lock" ] && echo y)" y
env -u XDG_CONFIG_HOME HOME="$work/dhome2" "$bsd" --once 2>/dev/null
check "babysitd: a missing default config is fine" $? 0
(cd "$work" && "$bsd" --once --log-dir reld --config /dev/null 2>/dev/null)
check "babysitd: relative --log-dir is resolved against the cwd" "$([ -e "$work/reld/.lock" ] && echo y)" y

finish
