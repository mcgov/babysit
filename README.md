# babysit

*A baby monitor for your child (processes).*

`babysit` is primarily a tool for agent assited coding sessions. The actual program is designed to be used by coding agents; since it offloads their need to manage the file descriptors, redirections, return codes, pids, and output. Because of this: *Most `babysit` commands do not block; most do not stop the child process when they return.*

`babysit` aims to be readable at a glance for easy human review; the design allows your agent to request approval for a small set of well defined process operations.

`babysit` provides a set of commands to:

- Start a child process.
- Observe its stdout and stderr.
- Identify ominous silences (quickly!).
- `--expect` a line of output, or expect multiple lines in a specific order across the entire log file.
- Send a signal to the child.
- Resolve logs, pids, and exit status for a child process. `babysit` can do this even after the child process exits.

---

## Quickstart
```sh
make                 # build bin/babysit and bin/babysitd
make test            # end-to-end test suite
make install-user    # for yourself, under ~/.local; no sudo
                     # optional: install the 'babysit' skill for your AI agent
make install-skill SKILL_DIR=/$agent_dir/skills

```

## Example: waiting for a server that might never come up

```sh
# start always prints a child_id once there is an instance
child_id=$(babysit start --expect 'Listening on' --timeout 30s -- ./server --port 8080)
rc=$?
case $rc in
  0)   echo "server is up" ;;
  124) echo "no sign of life after 30s, stopping it" >&2
       babysit signal TERM "$child_id"; exit $rc ;;
  *)   echo "server died during start-up (status $rc)" >&2; exit $rc ;;
esac

# keep waiting, and return early if it goes quiet for 30 seconds.
# send SIGINT if the child is spinning with no output.
# return an error code if it stopped but didn't print a specific string.
babysit watch "$child_id" --idle 30s --onfail INT --expect "$success_condition" || {
    echo "success condition not seen and no output for 30s"
}

# is it still there?
babysit check "$child_id" || echo "server has stopped"

# what was the exit code?
babysit check "$child_id"
```

---

## Why not `timeout` and _____?

The `babysit` agent skill allows agents to ask you to approve this:

```sh
child_id=$(babysit start --idle 2m -- make -j8)
babysit wait "$child_id" || echo "build failed"
echo "$(babysit info "$child_id")"
```

instead of this:

```sh
! Starting the build and wait for it to finish.
    timeout 1200 make -j8 1> build.log 2>err.log &
    build=$!
    [[ -f build.log ]] || pkill make;
    timeout 1200 bash -c "until tail -f build.log | grep -q finished; do sleep 1; done"
    (($?)) && echo "build complete! starting interesting work now..." \
    ls **/run.log && 
 > ctrl + b to run in the background
! Approve access to path '**/'?

> ( •_•) shimmy shamming ....            updated! run /restart to update
```

---

## Commands

| Command | What it does |
|---|---|
| `babysit start [options] -- command` | run a command, print its `child_id` |
| `babysit watch CHILD_ID [options]` | attach to a running instance and return as `start` would |
| `babysit wait CHILD_ID [--timeout SECS]` | block silently until the command exits; its exit status, or 124 |
| `babysit check CHILD_ID` | exit 0 and print `running`, or exit 1 and print `exited N` (`status` is another name for it) |
| `babysit info CHILD_ID [--pid\|--log\|--exit-code]` | where the log is, the state, the pid while it runs |
| `babysit signal SIGNAL CHILD_ID` | have the babysitter signal the command |
| `babysit cat [stdout] [stderr] CHILD_ID` | print the command's output so far, without the `OUT:`/`ERR:` labels |

When the command exits, its status is written to `exit_code` next to the
log.

## Child process ID

When `babysit` starts a child, the initial invocation will return a `child_id` rather than the pid of the child process. The `child_id` directly corresponds to the log directory for that child.

Instead of a pid, `babysit` uses a `child_id` that encodes the start time along with a small salt value. This enables you to check for logs before and after the process lifetime without the fear of race conditions. The scheme also results in a nicely organized log directory which can be archived and saved for later reference.

Each child process is assigned a `child_id` like:

```
<YYYYmmdd>/<HH>/<MM>/<salt>
```

The default output directory for a given run will look like:

```sh
~/.local/var/log/babysit/<YYYYmmdd>/<HH>/<MM>/<salt>/
# or, given a child_id of:
child_id=$(babysit start -- $some_binary)
# the logs will end up in:
~/.local/var/log/babysit/$child_id
```

## Child process log file format

`babysit` combines stdout and stderr into a single file; each line is prefixed with `OUT: ` or `ERR: `.
`babysit cat` removes the prefix, and can print the streams together or separately.

## babysitd and babysit.conf

`babysit` includes an optional quality-of-life service to make managing automation easier for the (human) user.

`babysit.conf` defines variables for configuring the log rollover, date format, etc.

`babysitd` is an optional systemd user service that can archive and delete old logs automatically after a configurable window of time.

Full documentation is in the man pages, in `docs/`: `babysit(1)`,
`babysitd(8)` and `babysit.conf(5)`.

---

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
make install-user    # for yourself, under ~/.local; no sudo
sudo make install    # or for all users, under /usr/local; pick one
systemctl --user enable --now babysitd
```

## Testing

```sh
make test            # plain build; tests/run.sh --filter NAME runs a subset
make check-asan      # AddressSanitizer (with leak detection on Linux)
make check-ubsan     # UndefinedBehaviorSanitizer
make check-tsan      # ThreadSanitizer
make coverage        # LLVM source-based coverage, report in build/coverage/
make fuzz            # libFuzzer with AddressSanitizer, a minute per target
make lint            # shellcheck and man page warnings
./precommit.sh       # clang-format (LLVM style), clang-tidy, -Werror build
make check-freebsd-vm   # all of the above on FreeBSD, in a throwaway QEMU VM
```

`./precommit.sh --install` makes it the repository's git pre-commit hook, and
`./precommit.sh --fix` reformats the sources. It needs `clang-format` and
`clang-tidy`.

### Running CI locally

The CI jobs can be run on your own machine, each in a fresh container:

```sh
tests/ci-local.sh --list              # the jobs, and which can run here
tests/ci-local.sh --job test-debian   # one job
tests/ci-local.sh                     # every job that needs no special runner
```

It reads the jobs from `.gitlab-ci.yml`, so it runs what GitLab would run,
against your working tree including uncommitted changes. It needs podman or
docker, and python3 with the yaml module. See `tests/ci-local.sh --help`.

### FreeBSD VM

`check-freebsd-vm` downloads the official FreeBSD cloud image once (about
670 MB, kept in `~/.cache/babysit/freebsd-vm`), boots a fresh copy of it,
and runs the suite, the sanitizers and the package build inside it. See
`tests/freebsd-vm.sh --help`.

## Tab completion

`make install` also installs bash completion for `babysit` and `babysitd`:
commands, options, signal names, and the `child_id`s of your runs, newest
first. It needs the bash-completion package, and takes effect in a new
shell. To try it without installing: `source completions/babysit.bash`.

## For coding agents

`skills/babysit/` is a skill that teaches a coding agent (Claude Code, for
one) when and how to use `babysit`: start servers and long commands under
it, keep the `child_id`, read output with `babysit cat`, stop things with
`babysit signal`.

```sh
make install-skill                 # into ~/.claude/skills, for every project
make install-skill SKILLDIR=/path/to/project/.claude/skills   # one project
make install-skill SKILL_LINK=1    # link instead of copy, to work on the skill
```

Start a new agent session afterwards; skills are read when a session starts.
The skill only helps where `babysit` itself is installed and on `PATH`.

## License

MIT. See [LICENSE](LICENSE).
