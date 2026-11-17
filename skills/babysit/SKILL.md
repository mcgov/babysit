---
name: babysit
description: Run long-running, slow or possibly-hanging shell commands under `babysit` so a tool call can never wait forever. Use this whenever you are about to start a dev server, database, watcher or any process that should keep running after you return; whenever you run a build, test suite, migration, download or script that may take more than a minute or might hang; whenever you need to wait for a "ready" line before carrying on; and whenever you need to check on, read the output of, or stop something you started earlier. Reach for it even if the user only says "start the server", "run the tests in the background" or "kick off the build" — if `babysit` is installed, prefer it over `&`, `nohup`, `sleep` loops, or `tail -f | grep`. In particular, use it anywhere you would otherwise start a process and then find it again with `pgrep`, `pidof`, `ps | grep` or `pkill`: the `babysit` pattern is likely to be more reliable in every such case. Also consult it when writing or editing a script that may take more than a minute to run, for how to make that script log so that it can be watched.
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
bbid=$(babysit start --idle 30s --timeout 20m -- make test)
rc=$?
babysit cat "$bbid" | tail -n 40
```

`start` does the waiting itself: it returns the command's own status when
it exits, 125 if it has printed nothing for thirty seconds, 124 after
twenty minutes. Set `--timeout` below the time limit of your own tool call,
so that it is `babysit` that returns and tells you where things stand,
rather than your tool call being cut off.

If it returns 124 or 125 the command is still running. You do not have to
sit and wait for it. Go and do something else, then come back and ask:

```sh
babysit info "$bbid"               # state, and exit status once it has one
babysit cat "$bbid" | tail -n 40   # what it has printed since
```

That is usually all you need, and it is cheaper than `babysit watch`:
`info` and `cat` answer at once and return, where `watch` holds a tool
call open and keeps looking at the log until something happens. When there
is nothing useful to do in the meantime, `babysit wait` is the way to
block until the command is done (next pattern); keep `watch` for when you
need to be told that it has gone quiet or printed a particular line (see
"Which operation to use" below).

### Start something that announces itself, then wait for it in silence

Many jobs print a few lines while they set up and then nothing until they
finish. Confirm the start with `--expect`, then `wait`:

```sh
bbid=$(babysit start --expect 'starting conversion' --timeout 2m -- ./convert.sh) || {
    babysit cat "$bbid" | tail -n 30     # it never got going: see why
    exit 1
}
babysit wait "$bbid" --timeout 30m
rc=$?                                    # its exit status, or 124: still running
babysit cat "$bbid" | tail -n 20
```

`wait` prints nothing and reads none of the output; it simply returns the
command's exit status when the command has exited, or 124 if the
`--timeout` passes first, in which case the command is still running and
you can `wait` again. Because the job is expected to be silent, `--idle`
would be the wrong test here, and `wait` does not take it.

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

## Which operation to use

Each of these answers a different question, and they do not cost the
same. Pick the cheapest one that answers the question you actually have.

**`start` with limits: when you are about to wait anyway.** Give
`--expect`, `--idle` and `--timeout` to `start` itself. One call starts
the command, waits for the first thing worth acting on, and returns the
identifier and a status. Do not `start --detach` and then immediately
`watch` with the same limits: that is the same wait in two calls.

**`start --detach`: when you are not going to wait.** Use it to start
several things at once, or to start something and get on with other work.
It returns as soon as the command has been executed, so its 0 says
"started", not "working"; follow up with `check` or `cat` before you
rely on it.

**`check` (or `status`): "is it still running?"** One word and an exit
status, immediately. This is the right call inside a condition
(`babysit check "$bbid" >/dev/null && ...`), and the right first question
about something you started a while ago.

**`info`: "what is its state, and how did it end?"** Also immediate. It
prints the log path, the state, the pid while it is running, and the exit
status once there is one. `info --exit-code` prints just the number and
fails until the command has exited, which makes it the direct way to
collect a result later: start, do other things, `info --exit-code`.

**`cat`: "what did it print?"** Immediate, and it works on a running
command as well as a finished one. Never read all of it by default: pipe
it through `tail -n`, `head` or `grep` for what you are looking for. When
something failed, `babysit cat stderr "$bbid" | tail -n 30` is usually
the fastest route to the reason. For a very large log, `tail -n 40
"$(babysit info --log "$bbid")"` reads only the end of the file, where
`cat` reads all of it to strip the labels (lines there start with `OUT: `
or `ERR: `).

**`wait`: "tell me when it has finished."** It blocks until the command
has exited and returns its exit status, printing nothing and reading
nothing; it costs next to nothing while it waits. Give it a `--timeout`
below your own tool call's limit; 124 means the command is still running.
This is the one to use for a command that is silent while it works, and
for collecting the result of something you started earlier.

**`watch`: "tell me when something changes."** Like `wait`, it blocks,
but it follows the log while it does, so that it can also return when the
command prints a particular line (`--expect`) or goes quiet (`--idle`).
It is not always necessary, and it is the most expensive of these: it
keeps a tool call open and looks at the log several times a second. Use
it only when one of those two conditions is what you are waiting for.
Always give it a `--timeout`, and send its stdout to `/dev/null` unless
you want every new line in your context. If all you want is the end of
the command, use `wait`; if you find yourself calling `watch` in a loop,
a single `start` with the right limits, or `info` when you next need the
answer, would have done.

**`signal`: "stop it", or anything else a signal can say.** Send `TERM`
first and give the command a moment; `check`; send `KILL` only if it is
still running. Use `INT` for programs that clean up on Ctrl-C. It exits 1
if the command has already gone, which is not an error for you: it is
the state you wanted.

**`start --follow`: not for you.** It is for a person at a terminal.

A typical exchange, then, has no `watch` in it at all:

```sh
bbid=$(babysit start --expect 'Listening on' --timeout 60s -- ./server)
# ... use the server ...
babysit check "$bbid" >/dev/null || babysit cat stderr "$bbid" | tail -n 20
# ... finished with it ...
babysit signal TERM "$bbid"
```

## Mistakes to avoid

### Do not run a silent wait loop under it

The loop you might write to wait for something is exactly the kind of
command `babysit` cannot help with:

```sh
# Don't: a loop that prints nothing until it is over
babysit start --idle 30s -- sh -c 'until grep -q "Listening on" server.log; do sleep 1; done'
```

`babysit` judges a command by its output, and this one has none. `--idle`
reports it as stalled after thirty seconds although it is doing just what
it was written to do; there is no line for `--expect` to match; and when
it does return, all you know is that the loop ended. The same goes for
`while ! curl -s ...; do sleep 1; done`, `sleep 600`, `wait`, and any
other command whose only job is to wait quietly.

Waiting for a line is what `--expect` is for. Put it on the command that
prints the line, not on a loop that looks for it:

```sh
# Do: let babysit watch the real command's output
bbid=$(babysit start --expect 'Listening on' --timeout 60s -- ./server)
```

If the command is already running under `babysit`, wait on that instance:
`babysit watch "$bbid" --expect 'Listening on' --timeout 60s`. If it was
not started by `babysit` and only writes to a file, a plain shell loop
with its own deadline is the honest tool; wrapping it in `babysit` adds
nothing.

### The command is run directly, not by a shell

`babysit start -- command args...` executes `command` itself. Nothing
between `--` and the end of the line is interpreted by a shell, so shell
syntax there does not do what it looks like:

- **`until`, `while`, `for`, `if`, `cd` and other shell words are not
  programs.** `babysit start -- until ...` fails with status 127,
  "cannot execute 'until'".
- **`VAR=value command` is not an assignment.** It fails with 127 as well.
  Use `env VAR=value command`.
- **`|`, `&&`, `;`, `>` and `2>&1` belong to your shell, not to the
  command.** In `babysit start -- make | tee out`, it is `babysit`'s own
  output, the identifier, that goes into the pipe, and `make` knows
  nothing of it.
- **`$VAR`, `*.txt` and `~` are expanded by your shell before `babysit`
  runs**, in your directory and with your variables.

When you need any of that, say so explicitly and quote it once:

```sh
bbid=$(babysit start --timeout 10m -- sh -c 'cd build && make 2>&1 | tee make.out')
```

Then remember what you have asked for: the status reported is the
shell's, which for a pipeline is that of its last command (`tee`, above)
unless you add `set -o pipefail` in a shell that has it.

### Other things that look reasonable and are not

- **Do not wrap `babysit` in `timeout`, `nohup`, `setsid` or `&`.** It
  already detaches the command and has its own limits. `timeout 60
  babysit start -- cmd` stops only the `babysit` that was waiting; the
  command carries on, and you have lost the identifier.
- **Do not combine `--detach` with `--expect`, `--idle` or `--timeout`.**
  `--detach` returns at once, so the others are never consulted. Leave
  `--detach` off when you want to wait.
- **`--expect` is a regular expression.** `.`, `[`, `(`, `*`, `+` and `?`
  mean something in it. Use `--fixed` for text such as `[::]:8080` or
  `(ready)`.
- **Several `--expect` options mean all of them, in that order.**
  `--expect 'migrations applied' --expect 'Listening on'` returns 0 only
  when both have appeared, the first before the second. It does not mean
  "either". Each is looked for after the text that matched the one before,
  so a line printed too early does not count. If the command exits first,
  stderr says how many were seen.
- **`--expect` sees one line at a time.** It cannot match text that the
  command prints across two lines, and it is not offered the command line
  or `babysit`'s own notes, only what the command wrote.
- **`babysit watch --expect` reads the log from the beginning.** A line
  printed before you called it still matches, which is what you want for
  "has it ever said ready", and not for "has it said it again".
- **A command that puts itself in the background is reported as exited.**
  Many daemons do that by default; `babysit` then follows the launcher,
  which exits at once with 0. Run such programs in the foreground
  (`--foreground`, `-F`, `--no-daemon`, or whatever the program calls it).
- **A limit that fires is not a failure of the command.** 124 and 125
  describe your patience, not its health. Look before you kill.

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

## Writing scripts that can be watched

`babysit` can only judge a command by what it prints. When you write or
edit a script that may run for more than a minute, shape its logging with
that in mind. It costs a few lines, and it is the difference between
finding out in the first minute that a run is doomed and finding out two
hours later.

**Say a lot before the long part starts.** Print each step of the setup as
it happens: which inputs were found, which configuration is in effect,
what is about to be run and with what arguments, and one unmistakable line
at the moment the long operation begins.

```sh
echo "config: $config ($(wc -l <"$config") lines)"
echo "input: $(ls "$input_dir" | wc -l) files in $input_dir"
echo "output: $out_dir (free: $(df -h --output=avail "$out_dir" | tail -n 1))"
echo "starting conversion of $total files"     # the line to --expect
```

Most failures of a long job happen here: a wrong path, a missing
credential, an empty input, a full disk. With this logging they are in
the first lines of the output, and `--expect 'starting conversion'` with a
short `--timeout` turns "the setup failed" into something you learn at
once, not when you come back to look.

**While the long part runs, print only when something has moved.** Log a
line when a unit of work completes ("converted 300 of 1200"), and nothing
in between. Do not add a timer that prints "still working..." every few
seconds, and do not leave a spinner or progress bar on. Output that
appears whether or not anything is happening tells `babysit`, and you,
nothing, and it defeats `--idle` completely: a script that is stuck but
still printing its heartbeat never looks idle. If every line means
progress, silence means something, and `--idle` becomes a real test.

**Make the last line say how it ended.** A final "done: 1200 converted, 0
failed" or "failed: ..." gives `--expect` something to wait for and the
reader something to check.

**Do not set the limits too tight.** The two failures are not equal: a
limit that is too long costs you some waiting, while one that is too short
reports a healthy run as stuck or late, and then you (or the user) stop a
job that was fine, or stop trusting the report. Set `--idle` comfortably
longer than the longest gap between two progress lines that a healthy run
can have, not the typical gap, and remember that output may arrive in
bursts (see "Silence is not a hang"). Set `--timeout` from how long the
whole thing is expected to take with room to spare, not from how long you
would like to wait. When you give both, `--timeout` must always be the
longer of the two, and `--idle` the shorter: `--idle` asks "has it gone
quiet?", which you want answered long before `--timeout` asks "has the
whole thing taken too long?". An `--idle` as long as the `--timeout`, or
longer, can never fire, because the `--timeout` always gets there first;
if you catch yourself writing `--idle 5m --timeout 2m`, one of the two
numbers is wrong. With nothing better to go on, `--idle 30s` is a sane default:
long enough that a command which logs as described above will not trip it,
short enough that a stuck one is noticed while you are still paying
attention. Raise it for a command you know to be quiet for longer, or whose
output arrives in bursts. If you do not know how long the whole thing
should take, say so, start with a generous `--timeout`, and tighten it
once you have seen one run. Remember
that 124 and 125 never stop the command: if a limit turns out to have been
too short, `babysit watch` it again for longer.

Scripts written this way pay off twice. A failure is caught early, at the
point where it costs a minute and not an afternoon. And the command you
end up running is short and says what it means:

```sh
bbid=$(babysit start --expect 'starting conversion' --timeout 2m -- ./convert.sh)
babysit watch "$bbid" --idle 10m --expect '^done: ' >/dev/null
```

A person reviewing your work can read those two lines and see what you
waited for and how long you were prepared to wait, which they cannot do
with a `sleep` loop wrapped around `pgrep` and `tail`. For a script that
prints nothing between its first line and its last, replace the second
line with `babysit wait "$bbid" --timeout 2h`.

## When not to use it

For a command that finishes in seconds and that you want the output of,
just run it. `babysit` earns its keep when a command might outlive your
tool call, might hang, or has to stay up while you do something else.
