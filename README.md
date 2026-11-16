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

Each command is logged to

    ~/.local/var/log/babysit/<boot time>/<generation>/<babysit pid>/<command pid>/log

with stdout lines prefixed `OUT: ` and stderr lines prefixed `ERR: `. The
path is printed as the first line of output.

`babysitd` is an optional systemd user service that archives the logs of
earlier boots, packs superseded log generations and expires old logs.

Full documentation is in the man pages: `babysit(1)`, `babysitd(8)` and
`babysit.conf(5)`.

## Building

clang/LLVM is the supported toolchain. The code is C99 and POSIX.1-2008;
`babysitd` additionally uses Linux `/proc` files where they exist.

```sh
make                 # build bin/babysit and bin/babysitd
make test            # end-to-end test suite
make install         # PREFIX=/usr/local by default
systemctl --user enable --now babysitd
```

## Testing

```sh
make test            # plain build
make check-asan      # AddressSanitizer + LeakSanitizer
make check-ubsan     # UndefinedBehaviorSanitizer
make check-tsan      # ThreadSanitizer
make coverage        # LLVM source-based coverage, report in build/coverage/
make lint            # shellcheck and man page warnings
```

## Packaging

```sh
make dist            # build/babysit-VERSION.tar.gz (source)
make bindist         # build/babysit-VERSION-linux-ARCH.tar.gz
make rpm             # needs rpmbuild
make deb             # needs dpkg-buildpackage and debhelper
```

The version is defined once, as `BABYSIT_VERSION` in `babysit.h`;
`debian/changelog` must start with the same version.

## License

MIT. See [LICENSE](LICENSE).
