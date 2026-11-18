#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Test: bash completion
#
# Goal:
#   completions/babysit.bash completes what can follow on a babysit or
#   babysitd command line: the commands, the options of each, signal names,
#   and the identifiers of the instances in the log directory named on the
#   line, newest first.
#
# Procedure:
#   1. Start two instances in a log directory.
#   2. Source the completion file and ask its functions, as bash would,
#      what may come next on a series of partial command lines.
#
# Fails if:
#   - a command, an option or a signal name is not offered where it belongs,
#     or an option of one command is offered for another.
#   - the two identifiers are not offered where an identifier is due, or
#     are offered from the wrong log directory.
#   - every option the programs accept is not also known to the completion.

# shellcheck source=tests/lib.sh
. "$(dirname "$0")/../lib.sh"

"$bs" start --log-dir "$work/c" -- true >"$work/id1" 2>/dev/null
"$bs" start --log-dir "$work/c" -- true >"$work/id2" 2>/dev/null
ids=$(LC_ALL=C sort "$work/id1" "$work/id2" | tr '\n' ' ')

# shellcheck source=completions/babysit.bash
. "$srcdir/completions/babysit.bash"
complete_line() { # the words typed so far; prints what is offered, sorted
    COMP_WORDS=("$@")
    COMP_CWORD=$((${#COMP_WORDS[@]} - 1))
    COMPREPLY=()
    "_${COMP_WORDS[0]}"
    printf '%s\n' "${COMPREPLY[@]}" | LC_ALL=C sort -u | tr '\n' ' '
}

check "the commands" "$(complete_line babysit "")" \
    "--help --version cat check follow-all info signal start status wait watch "
check "commands by their beginning" "$(complete_line babysit wa)" "wait watch "
check "options of wait" "$(complete_line babysit wait --)" "--log-dir --timeout "
check "options of start" "$(complete_line babysit start --f)" "--fixed --follow "
check "options of info, and not those of start" "$(complete_line babysit info --)" \
    "--exit-code --log --log-dir --pid "
check "identifiers for watch, from the --log-dir on the line" \
    "$(complete_line babysit watch --log-dir "$work/c" "")" "$ids"
check "identifiers for check, by their beginning" \
    "$(complete_line babysit check -l "$work/c" "$(cut -c 1-8 "$work/id1")")" "$ids"
check "no identifiers from a log directory that has none" \
    "$(complete_line babysit check -l "$work/none" "")" " "
check "signal: the signal first" "$(complete_line babysit signal T)" "TERM "
check "signal: then the identifier" "$(complete_line babysit signal TERM -l "$work/c" "")" "$ids"
check "--onfail takes a signal" "$(complete_line babysit watch --onfail K)" "KILL "
check "cat offers the streams as well" "$(complete_line babysit cat -l "$work/c" std)" "stderr stdout "
check "start completes a command name after --" \
    "$(complete_line babysit start -- tru | tr ' ' '\n' | grep -cx true)" 1
check "babysitd options" "$(complete_line babysitd --arch)" \
    "--archive-after-days --archive-on-usage-percentage "

# Every long option in the programs' own help is one the completion knows.
missing=""
for opt in $("$bs" --help | grep -o -- '--[a-z][a-z-]*' | sort -u); do
    grep -q -- "$opt" "$srcdir/completions/babysit.bash" || missing="$missing $opt"
done
for opt in $("$bsd" --help | grep -o -- '--[a-z][a-z-]*' | sort -u); do
    grep -q -- "$opt" "$srcdir/completions/babysit.bash" || missing="$missing $opt"
done
check "the completion knows every option in --help" "$missing" ""

finish
