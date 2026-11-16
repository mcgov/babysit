#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: the default log directory is under HOME
#
# Goal:
#   Without --log-dir, logs go to $HOME/.local/var/log/babysit.
#
# Procedure:
#   1. Run babysit with HOME pointing at a scratch directory and no --log-
#      dir.
#
# Fails if:
#   - the reported log path is not under
#     $HOME/.local/var/log/babysit/<boot>/0/.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

HOME="$work/home" "$bs" -- true >"$work/o1c" 2>/dev/null
check "default dir is ~/.local/var/log/babysit" \
    "$(logof "$work/o1c" | grep -cE "^$work/home/.local/var/log/babysit/$stamp/0/[0-9]+/[0-9]+/log$")" 1

finish
