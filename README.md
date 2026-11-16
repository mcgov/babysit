# babysit

Run a command detached, log it, and return early if it stalls.

`babysit` is for scripts and agents that need to start a process and then
decide what to do about it: wait for a "ready" line, notice that it has gone
quiet, or just let it run. Returning early never stops the command; it keeps
running and its output keeps going to a log.

```sh
# return once the server is listening (or after 30s), leave it running
babysit --expect 'Listening on' --timeout 30 -- ./server --port 8080

# run a build, but come back if it prints nothing for two minutes
babysit --idle 2m -- make -j8
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
babysit --expect 'Listening on' --idle 30s -- ./server --port 8080 &
ppid=$!
wait "$ppid"; rc=$?

bbid=$(babysit --get-child-pid "$ppid")
log=$(babysit --get-child-log "$ppid")

case $rc in
  0)   echo "server is up as bbid $bbid" ;;
  125) echo "no output for 30s; bbid $bbid is still running, maybe stuck" ;;
  *)   echo "server died during start-up (status $rc):"; tail -n 20 "$log" ;;
esac
tail -f "$log"
```

`--get-child-pid` and `--get-child-log` take the pid of the `babysit` you
started and print the command's pid and the path of its log. They print
nothing and exit 1 if the command was never started (status 2, 126 or 127).

Each command is logged to

    ~/.local/var/log/babysit/<boot time>/<generation>/<babysit pid>/<command pid>/log

with stdout lines prefixed `OUT: ` and stderr lines prefixed `ERR: `. The
path is printed as the first line of output.

`babysitd` is an optional systemd user service that archives the logs of
earlier boots, packs superseded log generations and expires old logs.

Full documentation is in the man pages: `babysit(1)`, `babysitd(8)` and
`babysit.conf(5)`.

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
The code is C99 and POSIX.1-2008, plus `sysctl` for the boot time on the
BSDs and macOS and `/proc` files on Linux.

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

`check-freebsd-vm` downloads the official FreeBSD cloud image once (about
670 MB, kept in `~/.cache/babysit/freebsd-vm`), boots a fresh copy of it,
and runs the suite, the sanitizers and the package build inside it. See
`tests/freebsd-vm.sh --help`.

## Packaging

```sh
make dist            # build/babysit-VERSION.tar.gz (source)
make bindist         # build/babysit-VERSION-linux-ARCH.tar.gz
make rpm             # needs rpmbuild
make deb             # needs dpkg-buildpackage and debhelper
gmake freebsd-pkg    # on FreeBSD; needs pkg
```

The version is defined once, as `BABYSIT_VERSION` in `babysit.h`;
`debian/changelog` must start with the same version.

## License

MIT. See [LICENSE](LICENSE).
