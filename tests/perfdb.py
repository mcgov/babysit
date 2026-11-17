#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Store the samples of a performance run in SQLite and report on them.

Used by tests/perf.sh; see there for what is measured. Two commands:

  perfdb.py record --db FILE --results FILE [--commit C] [--dirty]
                   [--host H] [--note TEXT]
      Add a run made of the sample lines in --results
      ("LOAD MODE SECONDS LINES BYTES") and print its number.

  perfdb.py report --db FILE [--run N] [--history N]
      Print the report for run N (default: the latest): each load's time
      on its own and under babysit, how that compares with the five runs
      before it and with the fastest run without babysit on record, and a
      rolling table of the last --history runs.

Only the Python standard library is used.
"""
import argparse
import datetime
import sqlite3
import statistics
import sys

SCHEMA = """
CREATE TABLE IF NOT EXISTS runs (
    id      INTEGER PRIMARY KEY,
    started TEXT NOT NULL,          -- local time, ISO 8601
    commit_ TEXT,                   -- git commit the binaries were built from
    dirty   INTEGER NOT NULL DEFAULT 0,  -- uncommitted changes at the time
    host    TEXT,
    note    TEXT
);
CREATE TABLE IF NOT EXISTS samples (
    run_id  INTEGER NOT NULL REFERENCES runs(id),
    load    TEXT NOT NULL,          -- light, medium, heavy
    mode    TEXT NOT NULL,          -- bare (the command alone) or babysit
    seconds REAL NOT NULL,
    lines   INTEGER NOT NULL,
    bytes   INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS samples_by_run ON samples (run_id, load, mode);
"""

LOADS = ("light", "medium", "heavy")
COMPARE_WITH = 5  # "the last five runs"


def connect(path):
    db = sqlite3.connect(path)
    db.executescript(SCHEMA)
    return db


def record(args):
    rows = []
    with open(args.results) as f:
        for n, line in enumerate(f, 1):
            parts = line.split()
            if not parts:
                continue
            if len(parts) != 5 or parts[1] not in ("bare", "babysit"):
                sys.exit("perfdb: %s:%d: not a sample line" % (args.results, n))
            rows.append((parts[0], parts[1], float(parts[2]), int(parts[3]),
                         int(parts[4])))
    if not rows:
        sys.exit("perfdb: no samples in %s" % args.results)
    db = connect(args.db)
    with db:
        cur = db.execute(
            "INSERT INTO runs (started, commit_, dirty, host, note) "
            "VALUES (?, ?, ?, ?, ?)",
            (datetime.datetime.now().isoformat(timespec="seconds"),
             args.commit, int(args.dirty), args.host, args.note))
        run = cur.lastrowid
        db.executemany(
            "INSERT INTO samples (run_id, load, mode, seconds, lines, bytes) "
            "VALUES (?, ?, ?, ?, ?, ?)", [(run,) + r for r in rows])
    print(run)


def median(db, run, load, mode):
    """Median seconds of one run's samples for a load and mode, or None."""
    values = [r[0] for r in db.execute(
        "SELECT seconds FROM samples WHERE run_id = ? AND load = ? AND mode = ?",
        (run, load, mode))]
    return statistics.median(values) if values else None


def secs(value):
    return "-" if value is None else "%.4f" % value


def ratio(value, base):
    return "" if not base else " (%.1fx)" % (value / base)


def percent(value, base):
    if not base:
        return "-"
    return "%+.1f%%" % ((value - base) / base * 100)


def loads_of(db, run):
    present = {r[0] for r in db.execute(
        "SELECT DISTINCT load FROM samples WHERE run_id = ?", (run,))}
    return [l for l in LOADS if l in present] + sorted(present - set(LOADS))


def table(rows):
    widths = [max(len(str(r[i])) for r in rows) for i in range(len(rows[0]))]
    for r in rows:
        print("  ".join(str(c).ljust(w) for c, w in zip(r, widths)).rstrip())


def report(args):
    db = connect(args.db)
    run = args.run
    if run is None:
        row = db.execute("SELECT max(id) FROM runs").fetchone()
        run = row[0]
    info = db.execute(
        "SELECT started, commit_, dirty, host, note FROM runs WHERE id = ?",
        (run,)).fetchone() if run is not None else None
    if info is None:
        sys.exit("perfdb: no such run in %s" % args.db)
    started, commit, dirty, host, note = info
    count = db.execute(
        "SELECT count(*) FROM samples WHERE run_id = ? GROUP BY load, mode "
        "LIMIT 1", (run,)).fetchone()
    print("babysit performance: run %d, %s" % (run, started.replace("T", " ")))
    print("commit %s%s, host %s, %d samples per measurement, medians shown%s"
          % (commit or "unknown", " (with uncommitted changes)" if dirty else "",
             host or "unknown", count[0] if count else 0,
             ", note: " + note if note else ""))
    print()

    rows = [("load", "lines", "MB", "alone s", "babysit s", "overhead",
             "vs last %d runs" % COMPARE_WITH, "vs fastest alone on record")]
    for load in loads_of(db, run):
        bare = median(db, run, load, "bare")
        sit = median(db, run, load, "babysit")
        lines, size = db.execute(
            "SELECT lines, bytes FROM samples WHERE run_id = ? AND load = ? "
            "LIMIT 1", (run, load)).fetchone()

        # The babysit medians of the runs before this one, same workload.
        earlier = [r[0] for r in db.execute(
            "SELECT DISTINCT run_id FROM samples WHERE run_id < ? AND load = ? "
            "AND mode = 'babysit' AND lines = ? AND bytes = ? "
            "ORDER BY run_id DESC LIMIT ?",
            (run, load, lines, size, COMPARE_WITH))]
        before = [median(db, r, load, "babysit") for r in earlier]
        if before and sit is not None:
            mean = statistics.mean(before)
            versus = "%s (was %s over %d)" % (percent(sit, mean), secs(mean),
                                              len(before))
        else:
            versus = "first run"

        # The fastest the command has ever run without babysit, any run.
        best = db.execute(
            "SELECT min(seconds), run_id FROM samples WHERE load = ? AND "
            "mode = 'bare' AND lines = ? AND bytes = ?",
            (load, lines, size)).fetchone()
        if best and best[0] is not None and sit is not None:
            fastest = "+%s%s; %s in run %d" % (
                secs(sit - best[0]), ratio(sit, best[0]), secs(best[0]),
                best[1])
        else:
            fastest = "-"

        overhead = "-"
        if bare is not None and sit is not None:
            overhead = "+%s%s" % (secs(sit - bare), ratio(sit, bare))
        rows.append((load, lines, "%.1f" % (size / 1e6), secs(bare), secs(sit),
                     overhead, versus, fastest))
    table(rows)

    print()
    print("The last %d runs (seconds under babysit, alone in brackets):"
          % args.history)
    recent = [r for r in db.execute(
        "SELECT id, started, commit_, dirty FROM runs WHERE id <= ? "
        "ORDER BY id DESC LIMIT ?", (run, args.history))]
    recent.reverse()
    all_loads = []
    for r in recent:
        for load in loads_of(db, r[0]):
            if load not in all_loads:
                all_loads.append(load)
    rows = [("run", "when", "commit") + tuple(all_loads)]
    for rid, when, commit, dirty in recent:
        cells = []
        for load in all_loads:
            sit = median(db, rid, load, "babysit")
            bare = median(db, rid, load, "bare")
            cells.append("-" if sit is None else
                         "%s [%s]" % (secs(sit), secs(bare)))
        rows.append((("*" if rid == run else " ") + str(rid),
                     when.replace("T", " ")[:16],
                     (commit or "unknown") + ("+" if dirty else ""))
                    + tuple(cells))
    table(rows)


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = p.add_subparsers(dest="command", required=True)
    r = sub.add_parser("record", help="add a run from a file of samples")
    r.add_argument("--db", required=True)
    r.add_argument("--results", required=True)
    r.add_argument("--commit")
    r.add_argument("--dirty", action="store_true")
    r.add_argument("--host")
    r.add_argument("--note")
    r.set_defaults(func=record)
    s = sub.add_parser("report", help="print the report for a run")
    s.add_argument("--db", required=True)
    s.add_argument("--run", type=int)
    s.add_argument("--history", type=int, default=10)
    s.set_defaults(func=report)
    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
