"""
datagen.py - run self-play on every core, and report how fast it went.

    python tools/datagen.py --out external/data/gen11/6s/gen11 --games 100000 --generation 11

The games are played by `yapper datagen`, one process a job, each with its own seed and file, so
nothing is shared and nothing waits. A file is a list of whole games, so the parts of a run can
be read one by one or simply joined. docs/DATA.md has the format.

Progress goes to external/games/live/datagen-<name>.json as well, for tools/progress.py, from the
lines each job prints as it plays.

Stdlib only.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

LIVE = Path(__file__).resolve().parent.parent / "external" / "games" / "live"
PLAYED_RE = re.compile(r"^(\d+) games (\d+) positions")


def write_status(path: Path, status: dict) -> None:
    """A reader holding the file must never stop the run, so a write that fails is skipped."""
    status["updated"] = time.time()
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        temp = path.with_suffix(".tmp")
        temp.write_text(json.dumps(status), encoding="utf-8")
        os.replace(temp, path)
    except OSError:
        pass


def follow(process: subprocess.Popen, job: dict) -> None:
    for line in process.stdout:
        job["last"] = line.strip()
        if m := PLAYED_RE.match(line):
            job["games"], job["positions"] = int(m[1]), int(m[2])


def main() -> int:
    default_engine = "./yapper.exe" if os.name == "nt" else "./yapper"
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--engine", default=default_engine)
    parser.add_argument("--out", required=True, help="file names start with this")
    parser.add_argument("--games", type=int, required=True, help="in total, across all jobs")
    parser.add_argument("--nodes", type=int, default=5000, help="search budget per move")
    parser.add_argument("--size", type=int, default=6)
    parser.add_argument("--generation", type=int, default=0)
    parser.add_argument("--seed", type=int, default=1, help="jobs use this and the next ones")
    # A budget in nodes cannot flag, so unlike a match this can use every logical core.
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    parser.add_argument("--openings", help="a book from tools/mkopenings.py, in place of random plies")
    parser.add_argument("--label", help="what tools/progress.py calls the run")
    parser.add_argument("--status", help="progress as JSON; default external/games/live/datagen-<name>.json")
    args = parser.parse_args()

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    engine = str(Path(args.engine).resolve())
    started = time.monotonic()
    status_path = Path(args.status) if args.status else LIVE / f"datagen-{out.name}.json"
    status = {"kind": "datagen", "label": args.label or f"datagen {out.name}", "pid": os.getpid(),
              "out": str(out.resolve()), "nodes": args.nodes, "size": args.size,
              "jobs": args.jobs, "cap": args.games, "games": 0, "positions": 0,
              "started": time.time(), "finished": False}

    jobs = []
    for job in range(args.jobs):
        games = args.games // args.jobs + (job < args.games % args.jobs)
        path = out.with_name(f"{out.name}-{args.seed + job}.ydg")
        command = [engine, "datagen", str(path), str(games), str(args.seed + job),
                   str(args.nodes), str(args.size), str(args.generation)]
        if args.openings:
            command.append(str(Path(args.openings).resolve()))
        process = subprocess.Popen(command, stdout=subprocess.PIPE, text=True)
        state = {"path": path, "process": process, "games": 0, "positions": 0, "last": ""}
        state["reader"] = threading.Thread(target=follow, args=(process, state), daemon=True)
        state["reader"].start()
        jobs.append(state)

    while any(job["process"].poll() is None for job in jobs):
        status.update(games=sum(j["games"] for j in jobs),
                      positions=sum(j["positions"] for j in jobs))
        write_status(status_path, status)
        time.sleep(5)

    failed = False
    for job in jobs:
        job["reader"].join()
        if job["process"].returncode != 0:
            print(f"{job['path'].name} failed: {job['last'] or 'no output'}", file=sys.stderr)
            failed = True

    seconds = time.monotonic() - started
    bytes_written = sum(j["path"].stat().st_size for j in jobs if j["path"].exists())
    status.update(games=sum(j["games"] for j in jobs), positions=sum(j["positions"] for j in jobs),
                  finished=True, code=1 if failed else 0)
    write_status(status_path, status)
    print(f"{args.games} games, {bytes_written / 1e6:.1f} MB, {seconds:.1f} seconds, "
          f"{args.games / seconds:.1f} games/s on {args.jobs} jobs")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
