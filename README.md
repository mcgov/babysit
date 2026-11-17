# babysit

Run a command detached, log it, and return early if it stalls.

`babysit` is for scripts and agents that need to start a process and then
decide what to do about it: wait for a "ready" line, notice that it has gone
quiet, or just let it run. Returning early never stops the command; it keeps
running and its output keeps going to a log.

`babysit start` prints one thing on stdout: an identifier for the run, such as
`20261116/06/15/k3f9`. The command's output is not copied to the caller; it
is read from the log, with `babysit cat`, `babysit watch` or `tail -f`.

```sh
# return once the server is listening (or after 30s), leave it running
babysit start --expect 'Listening on' --timeout 30 -- ./server --port 8080

# run a build, but come back if it prints nothing for two minutes
babysit start --idle 2m -- make -j8
```

| Exit status | Meaning |
|---|---|
| *N* | the command exited with status *N* (128+signal if killed) |
| 0 | `--expect` matched, or `--detach` |
| 1 | `--expect` given, command exited 0 without a match |
| 124 | `--timeout` expired; command still running |
| 125 | no output for `--idle` seconds; command still running |
| 126 / 127 | command could not be executed / was not found |
| 2 | usage or internal error |

## Example: waiting for a server that might never come up

The usual way to wait for a server to be ready looks reasonable:

```sh
./server --port 8080 > server.log 2>&1 &
tail -f server.log | grep -q 'Listening on'   # wait until it's ready...
./run-tests
```

If the server crashes or hangs before it prints that line, `tail -f` never
ends and this waits forever. With `babysit`:

```sh
# start always prints an identifier once there is an instance
bbid=$(babysit start --expect 'Listening on' --timeout 30s -- ./server --port 8080)
rc=$?
case $rc in
  0)   echo "server is up" ;;
  124) echo "no sign of life after 30s, stopping it" >&2
       babysit signal TERM "$bbid"; exit $rc ;;
  *)   echo "server died during start-up (status $rc)" >&2; exit $rc ;;
esac

# later: come back if it goes quiet for 30 seconds, and interrupt it then
babysit watch "$bbid" --idle 30s --onfail INT --expect "$success_condition" || {
    echo "success condition not seen and no output for 30s"
}

# is it still there?
babysit check "$bbid" || echo "server is dead"
```

| Command | What it does |
|---|---|
| `babysit start [options] -- command` | run a command, print its identifier |
| `babysit watch CHILD_ID [options]` | attach to a running instance and return as `start` would |
| `babysit check CHILD_ID` | exit 0 and print `running`, or exit 1 and print `exited N` (`status` is another name for it) |
| `babysit info CHILD_ID [--pid\|--log\|--exit-code]` | where the log is, the state, the pid while it runs |
| `babysit signal SIGNAL CHILD_ID` | have the babysitter signal the command |
| `babysit cat [stdout] [stderr] CHILD_ID` | print the command's output so far, without the `OUT:`/`ERR:` labels |

When the command exits, its status is written to `exit_code` next to the
log. A pid is only ever given out while the command is running, and
`babysit signal` avoids needing one at all: the babysitter is the command's
parent, so its signal cannot land on a process that has since been given
the same pid.

Each run is logged to

    ~/.local/var/log/babysit/<YYYYmmdd>/<HH>/<MM>/<salt>/log

which is the log directory followed by the identifier, so a log can be
opened straight from an identifier. Stdout lines are prefixed `OUT: ` and
stderr lines `ERR: `. Dates are local time; set `timezone = utc` in
`babysit.conf` when driving `babysit` from automation.

`babysitd` is an optional systemd user service that archives the logs of
past days, packs sooner when the disk fills up, and expires old logs.

Full documentation is in the man pages, in `docs/`: `babysit(1)`,
`babysitd(8)` and `babysit.conf(5)`.

## Platforms

| Platform | Status |
|---|---|
| Linux, x86_64 and aarch64 | supported; tested in CI |
| FreeBSD, x86_64 | supported; tested with `make check-freebsd-vm` |
| FreeBSD, aarch64 | expected to work; not tested |
| macOS, other BSDs | best effort, untested |
| Windows | not supported; a port would be a separate project |

Without systemd, run `babysitd` from cron: see `babysitd(8)`, section
WITHOUT SYSTEMD.

## Building

clang/LLVM is the supported toolchain, with GNU make (`gmake` on the BSDs).
The code is C99 and POSIX.1-2008.

```sh
make                 # build bin/babysit and bin/babysitd
make test            # end-to-end test suite
make install         # PREFIX=/usr/local by default
systemctl --user enable --now babysitd
```

## Testing

```sh
make test            # plain build; tests/run.sh --filter NAME runs a subset
make check-asan      # AddressSanitizer (with leak detection on Linux)
make check-ubsan     # UndefinedBehaviorSanitizer
make check-tsan      # ThreadSanitizer
make coverage        # LLVM source-based coverage, report in build/coverage/
make lint            # shellcheck and man page warnings
./precommit.sh       # clang-format (LLVM style), clang-tidy, -Werror build
make check-freebsd-vm   # all of the above on FreeBSD, in a throwaway QEMU VM
```

`./precommit.sh --install` makes it the repository's git pre-commit hook, and
`./precommit.sh --fix` reformats the sources. It needs `clang-format` and
`clang-tidy`.

The test cases are in `tests/testcases/`, one file each, numbered in the
order they run. Every file starts with a comment giving its goal, its
procedure and what makes it fail, and can be run on its own.

`make perf` measures what `babysit` costs. It times a command that prints
200, 200,000 and 2,000,000 lines (a light, a medium and a heavy load), on
its own and under `babysit start`, and stores every timing in an SQLite
database under `~/.local/share/babysit/`. The report it prints sets each
run against the five before it and against the fastest the command has
ever run without `babysit`, and ends with a rolling table of recent runs:

```sh
make perf                         # measure, store, report
tests/perf.sh --report-only       # the latest report again
tests/perf.sh --note "before the output rewrite" --repeat 9
```

The performance cases never fail on speed, only if a run goes wrong, and
they are not part of `make test` or of CI: timings from shared runners say
little. It needs python3. See `tests/perf.sh --help`.

The CI jobs can be run on your own machine, each in a fresh container:

```sh
tests/ci-local.sh --list              # the jobs, and which can run here
tests/ci-local.sh --job test-debian   # one job
tests/ci-local.sh                     # every job that needs no special runner
```

It reads the jobs from `.gitlab-ci.yml`, so it runs what GitLab would run,
against your working tree including uncommitted changes. It needs podman or
docker, and python3 with the yaml module. See `tests/ci-local.sh --help`.

`check-freebsd-vm` downloads the official FreeBSD cloud image once (about
670 MB, kept in `~/.cache/babysit/freebsd-vm`), boots a fresh copy of it,
and runs the suite, the sanitizers and the package build inside it. See
`tests/freebsd-vm.sh --help`.

## Tab completion

`make install` also installs bash completion for `babysit` and `babysitd`:
commands, options, signal names, and the identifiers of your runs, newest
first. It needs the bash-completion package, and takes effect in a new
shell. To try it without installing: `source completions/babysit.bash`.

## For coding agents

`skills/babysit/` is a skill that teaches a coding agent (Claude Code, for
one) when and how to use `babysit`: start servers and long commands under
it, keep the identifier, read output with `babysit cat`, stop things with
`babysit signal`.

```sh
make install-skill                 # into ~/.claude/skills, for every project
make install-skill SKILLDIR=/path/to/project/.claude/skills   # one project
make install-skill SKILL_LINK=1    # link instead of copy, to work on the skill
```

Start a new agent session afterwards; skills are read when a session starts.
The skill only helps where `babysit` itself is installed and on `PATH`.

## Packaging

```sh
make dist            # build/babysit-VERSION.tar.gz (source)
make bindist         # build/babysit-VERSION-linux-ARCH.tar.gz
make rpm             # needs rpmbuild; Fedora and openSUSE
make deb             # needs dpkg-buildpackage and debhelper
make arch-pkg        # on Arch Linux; needs makepkg, not as root
gmake freebsd-pkg    # on FreeBSD; needs pkg
```

The version is defined once, as `BABYSIT_VERSION` in `src/babysit.h`;
`debian/changelog` must start with the same version.

## License

MIT. See [LICENSE](LICENSE).
