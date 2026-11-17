---
name: babysit
description: Run long-running, slow or possibly-hanging shell commands under `babysit` so a tool call can never wait forever. Use this whenever you are about to start a dev server, database, watcher or any process that should keep running after you return; whenever you run a build, test suite, migration, download or script that may take more than a minute or might hang; whenever you need to wait for a "ready" line before carrying on; and whenever you need to check on, read the output of, or stop something you started earlier. Reach for it even if the user only says "start the server", "run the tests in the background" or "kick off the build" — if `babysit` is installed, prefer it over `&`, `nohup`, `sleep` loops, or `tail -f | grep`. In particular, use it anywhere you would otherwise start a process and then find it again with `pgrep`, `pidof`, `ps | grep` or `pkill`: the `babysit` pattern is likely to be more reliable in every such case.
---

# babysit

`babysit` starts a command detached from your shell, records everything it
prints, and returns to you as soon as something worth acting on has
happened: a line you were waiting for appeared, it went quiet, a time limit
passed, or it exited. Returning early never stops the command.

It exists because the usual ways of doing this go wrong for an agent in
predictable ways. A server started in the foreground never returns. One
started with `&` leaves you guessing when it is ready, where its output
went and which pid is still yours. `tail -f log | grep -q ready` waits
forever if the server died before it got that far.

## Instead of start-then-`pgrep`

The habit this skill most wants to replace is: start a process, then find
it again by name.

```sh
./server --port 8080 &          # or nohup, or setsid, or a second tool call
...
pgrep -f 'server --port 8080'   # is it still there? which one is it?
pkill -f 'server --port 8080'   # stop it
```

Wherever you would do that, the `babysit` pattern is likely to be more
reliable, because looking a process up by name is a guess, and a handle you
were given is not:

- **A name can match the wrong process.** `pgrep -f` matches anything whose
  command line contains the text: another instance from an earlier attempt,
  the user's own copy, an editor with the file open, sometimes the very
  shell running the `pgrep`. `pkill` then stops all of them.
- **A name can match nothing while the process is alive.** Wrappers and
  interpreters change what the command line looks like (`npm run dev`
  becomes `node ...`), so the process you started is there under a name you
  did not search for, and you start a second one.
- **"Not found" does not say how it ended.** With `pgrep` a crash and a
  clean exit look the same, and the exit status is gone. `babysit check`
  says `exited N`, and the output is still there to read.
- **A pid goes stale between finding it and using it.** `babysit signal`
  is delivered by the process that started the command, so it cannot land
  on something else.

```sh
bbid=$(babysit start --detach -- ./server --port 8080)
...
babysit check "$bbid"           # running | exited N | lost
babysit signal TERM "$bbid"     # stop that one, and only that one
```

The same goes for finding a process started by an earlier tool call: keep
the identifier from that call (say it in your notes or your reply) instead
of searching for the process afterwards.

`pgrep` is still the right tool for processes you did not start.

## Before you use it

Check that it is there first: `command -v babysit`. If it is not, fall back
to whatever you would otherwise do; do not try to install it unasked.

## The one idea to hold on to

`babysit start` prints exactly one thing on stdout: an identifier such as
`20261116/06/15/k3f9`. Capture it. Every later question about that run
(is it alive, what did it print, how did it end, stop it) is asked with
that identifier. Never keep or act on a pid instead: a pid can be handed to
an unrelated process once the command has exited, and the identifier
cannot.

```sh
bbid=$(babysit start [options] -- command args...)
rc=$?
```

Nothing of the command's output is printed by `start`. You read it
afterwards, and only as much as you need, which keeps a noisy build from
flooding your context.

## What `rc` tells you

| Status | Meaning | The command is |
|---|---|---|
| 0 | `--expect` matched, or `--detach` was given, or it exited 0 | running (or done, if it exited) |
| 124 | `--timeout` passed | still running |
| 125 | nothing was printed for `--idle` | still running |
| 1 | `--expect` was given, and it exited 0 without printing the line | gone |
| 126 / 127 | it could not be executed / was not found | never ran |
| 2 | you called `babysit` wrongly | never ran |
| anything else | the command's own exit status (128+N if killed by signal N) | gone |

A command can itself exit with 124, 125 or 1, so when the status alone
leaves you unsure, ask: `babysit check "$bbid"` prints `running`, or
`exited N`, and exits 0 only while it is running.

## The patterns

### Start something that should keep running, and wait until it is ready

```sh
bbid=$(babysit start --expect 'Listening on' --timeout 60s -- npm run dev)
case $? in
  0)   ;;                                   # ready; carry on
  124) babysit cat "$bbid" | tail -n 30     # not ready in time: look why
       babysit signal TERM "$bbid" ;;       # ...and do not leave it behind
  *)   babysit cat "$bbid" | tail -n 30 ;;  # it died during start-up
esac
```

Pick the `--expect` text from what the program really prints when it is
ready; if you do not know, start it once with `--timeout 15s` and read the
output. `--expect` is an extended regular expression matched against each
line; add `--fixed` to match a plain substring.

### Run something long, and come back when it is finished or stuck

```sh
bbid=$(babysit start --detach -- make test)
babysit watch "$bbid" --idle 3m --timeout 20m >/dev/null
rc=$?
babysit cat "$bbid" | tail -n 40
```

`watch` returns exactly as `start` would have: the command's own status
when it exits, 125 if it has printed nothing for three minutes, 124 after
twenty. It prints the lines it sees on stdout, hence the `>/dev/null`. Set
`--timeout` below the time limit of your own tool call, so that it is
`babysit` that returns and tells you where things stand, rather than your
tool call being cut off.

If it returns 124 or 125 the command is still running. Look at the tail of
its output and decide: `watch` it again for longer, or stop it.

### Look in on something you started earlier

```sh
babysit check "$bbid"              # running | exited N | lost
babysit cat "$bbid" | tail -n 40   # what it has printed so far
babysit cat stderr "$bbid"         # only its stderr
babysit info --exit-code "$bbid"   # its exit status, once it has one
```

`cat` gives back the command's output as the command wrote it. `cat stdout`
and `cat stderr` give one stream; plain `cat` gives both, in order.

### Stop it

```sh
babysit signal TERM "$bbid"
```

This signals the command and everything it started. Use it instead of
`kill`: it needs no pid and cannot hit the wrong process. Follow with
`babysit check "$bbid"`, and `babysit signal KILL "$bbid"` if it is still
running a few seconds later.

## Things that will otherwise trip you up

**Silence is not a hang.** `--idle` means "printed nothing", and many
programs save their output up when it is not going to a terminal, so a
healthy program can look silent for minutes and then deliver everything at
once. If a 125 surprises you, look at the output before you conclude
anything, and switch the buffering off when you start it: `python -u`,
`PYTHONUNBUFFERED=1`, `stdbuf -oL command`, `--line-buffered` for grep. The
same delay applies to `--expect`.

**A match is not health.** `--expect` returning 0 means one line was
printed. If the service matters, test it for real afterwards (a request, a
port check).

**The command has no terminal and no input.** Its stdin is `/dev/null`.
Anything that asks a question or wants a password will fail or give up, so
pass `--yes`-style flags and credentials the way you would in CI.

**Clean up what you start.** A command you started keeps running after
your work is done. Unless the user wants it left running, stop it with
`babysit signal TERM` before you finish, and say in your summary what you
left running and under which identifier.

**Leave `--follow` alone.** It streams the output while `start` waits and
is meant for a person at a terminal. For you it only costs context.

**Tell the user the identifier.** When you leave something running, give
the user the identifier and the command to look at it
(`babysit cat <id>`), so they can pick it up without you.

## When not to use it

For a command that finishes in seconds and that you want the output of,
just run it. `babysit` earns its keep when a command might outlive your
tool call, might hang, or has to stay up while you do something else.
