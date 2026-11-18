# SPDX-License-Identifier: MIT
#
# Bash completion for babysit and babysitd.
#
# Installed by "make install" as both "babysit" and "babysitd" in the
# bash-completion directory, where it is loaded the first time either
# command is completed. To use it without installing:
#
#     source completions/babysit.bash
#
# Besides commands and options it completes instance identifiers, newest
# first, from the log directory: the one given with --log-dir on the line
# being completed, or the default.

# The log directory the command line being completed refers to.
_babysit_logdir() {
    local i
    for ((i = 1; i < COMP_CWORD; i++)); do
        case "${COMP_WORDS[i]}" in
            -l | --log-dir)
                printf '%s\n' "${COMP_WORDS[i + 1]}"
                return
                ;;
            --log-dir=*)
                printf '%s\n' "${COMP_WORDS[i]#--log-dir=}"
                return
                ;;
        esac
    done
    printf '%s\n' "$HOME/.local/var/log/babysit"
}

# Instance identifiers (YYYYmmdd/HH/MM/salt) under the log directory, the
# most recent first, and no more than a screenful or two.
_babysit_ids() {
    local dir
    dir="$(_babysit_logdir)"
    dir="${dir/#\~/$HOME}"
    [ -d "$dir" ] || return
    (cd "$dir" 2>/dev/null &&
        find . -mindepth 4 -maxdepth 4 -type d \
            -path './[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]/[0-9][0-9]/[0-9][0-9]/????*' \
            2>/dev/null | sed 's|^\./||' | sort -r | head -n 200)
}

_babysit_signals="HUP INT QUIT KILL TERM USR1 USR2 STOP CONT"

_babysit() {
    local cur prev sub="" i dashdash=0 opts
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD - 1]}"
    COMPREPLY=()

    if [ "$COMP_CWORD" -eq 1 ]; then
        mapfile -t COMPREPLY < <(compgen -W "start watch wait check status info signal cat follow-all --help --version" -- "$cur")
        return
    fi
    sub="${COMP_WORDS[1]}"

    # The value of an option that takes one.
    case "$prev" in
        -l | --log-dir)
            mapfile -t COMPREPLY < <(compgen -d -- "$cur")
            return
            ;;
        -c | --config)
            [ "$sub" = start ] && {
                mapfile -t COMPREPLY < <(compgen -f -- "$cur")
                return
            }
            ;;
        --onfail)
            mapfile -t COMPREPLY < <(compgen -W "$_babysit_signals" -- "$cur")
            return
            ;;
        -e | --expect | -i | --idle | -t | --timeout | -s | --since)
            case "$sub" in start | watch | wait | follow-all) return ;; esac
            ;;
    esac

    case "$sub" in
        start)
            # After "--", or after the first word that is not an option, it
            # is the command to run that is being typed.
            for ((i = 2; i < COMP_CWORD; i++)); do
                case "${COMP_WORDS[i]}" in
                    --)
                        dashdash=$i
                        break
                        ;;
                    -e | --expect | -i | --idle | -t | --timeout | -l | --log-dir | -c | --config) i=$((i + 1)) ;;
                    -*) ;;
                    *)
                        dashdash=$((i - 1))
                        break
                        ;;
                esac
            done
            if [ "$dashdash" -gt 0 ]; then
                if [ "$COMP_CWORD" -eq $((dashdash + 1)) ]; then
                    mapfile -t COMPREPLY < <(compgen -c -- "$cur")
                else
                    mapfile -t COMPREPLY < <(compgen -f -- "$cur")
                fi
                return
            fi
            opts="--expect --fixed --idle --timeout --detach --follow --config --log-dir --help --"
            if [[ "$cur" == -* ]]; then
                mapfile -t COMPREPLY < <(compgen -W "$opts" -- "$cur")
            else
                mapfile -t COMPREPLY < <(compgen -c -- "$cur")
            fi
            ;;
        watch)
            if [[ "$cur" == -* ]]; then
                mapfile -t COMPREPLY < <(compgen -W "--expect --fixed --idle --timeout --onfail --log-dir" -- "$cur")
            else
                mapfile -t COMPREPLY < <(compgen -W "$(_babysit_ids)" -- "$cur")
            fi
            ;;
        follow-all)
            mapfile -t COMPREPLY < <(compgen -W "--since --idle --log-dir" -- "$cur")
            ;;
        wait)
            if [[ "$cur" == -* ]]; then
                mapfile -t COMPREPLY < <(compgen -W "--timeout --log-dir" -- "$cur")
            else
                mapfile -t COMPREPLY < <(compgen -W "$(_babysit_ids)" -- "$cur")
            fi
            ;;
        check | status)
            if [[ "$cur" == -* ]]; then
                mapfile -t COMPREPLY < <(compgen -W "--log-dir" -- "$cur")
            else
                mapfile -t COMPREPLY < <(compgen -W "$(_babysit_ids)" -- "$cur")
            fi
            ;;
        info)
            if [[ "$cur" == -* ]]; then
                mapfile -t COMPREPLY < <(compgen -W "--pid --log --exit-code --log-dir" -- "$cur")
            else
                mapfile -t COMPREPLY < <(compgen -W "$(_babysit_ids)" -- "$cur")
            fi
            ;;
        signal)
            if [[ "$cur" == -* ]]; then
                mapfile -t COMPREPLY < <(compgen -W "--log-dir" -- "$cur")
            else
                # The signal comes first, then the identifier.
                for ((i = 2; i < COMP_CWORD; i++)); do
                    case "${COMP_WORDS[i]}" in
                        -l | --log-dir) i=$((i + 1)) ;;
                        -*) ;;
                        *)
                            mapfile -t COMPREPLY < <(compgen -W "$(_babysit_ids)" -- "$cur")
                            return
                            ;;
                    esac
                done
                mapfile -t COMPREPLY < <(compgen -W "$_babysit_signals" -- "$cur")
            fi
            ;;
        cat)
            if [[ "$cur" == -* ]]; then
                mapfile -t COMPREPLY < <(compgen -W "--log-dir" -- "$cur")
            else
                mapfile -t COMPREPLY < <(compgen -W "stdout stderr $(_babysit_ids)" -- "$cur")
            fi
            ;;
    esac
}

_babysitd() {
    local cur prev
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD - 1]}"
    COMPREPLY=()
    case "$prev" in
        -l | --log-dir)
            mapfile -t COMPREPLY < <(compgen -d -- "$cur")
            return
            ;;
        -c | --config)
            mapfile -t COMPREPLY < <(compgen -f -- "$cur")
            return
            ;;
        -a | --max-age-days | -i | --check-interval | -A | --archive-after-days | -u | --archive-on-usage-percentage | -D | --delete-at-disk-percent)
            return
            ;;
    esac
    mapfile -t COMPREPLY < <(compgen -W "--log-dir --config --max-age-days --check-interval --archive-after-days --archive-on-usage-percentage --delete-at-disk-percent --once --version --help" -- "$cur")
}

complete -F _babysit babysit
complete -F _babysitd babysitd
