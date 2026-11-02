"""
datagen.py - run self-play on every core, and report how fast it went.

    python tools/datagen.py --out external/data/gen1 --games 100000 --nodes 5000 --generation 1

The games are played by `yapper datagen`, one process a job, each with its own seed and file, so
nothing is shared and nothing waits. A file is a list of whole games, so the parts of a run can
be read one by one or simply joined. docs/DATA.md has the format.

Stdlib only.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path


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
    args = parser.parse_args()

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    engine = str(Path(args.engine).resolve())
    started = time.monotonic()

    jobs = []
    for job in range(args.jobs):
        games = args.games // args.jobs + (job < args.games % args.jobs)
        path = out.with_name(f"{out.name}-{args.seed + job}.ydg")
        command = [engine, "datagen", str(path), str(games), str(args.seed + job),
                   str(args.nodes), str(args.size), str(args.generation)]
        jobs.append((path, subprocess.Popen(command, stdout=subprocess.PIPE, text=True)))

    failed = False
    for path, process in jobs:
        output = process.communicate()[0]
        lines = output.strip().splitlines()
        if process.returncode != 0:
            print(f"{path.name} failed: {lines[-1] if lines else 'no output'}", file=sys.stderr)
            failed = True

    seconds = time.monotonic() - started
    bytes_written = sum(path.stat().st_size for path, _ in jobs if path.exists())
    print(f"{args.games} games, {bytes_written / 1e6:.1f} MB, {seconds:.1f} seconds, "
          f"{args.games / seconds:.1f} games/s on {args.jobs} jobs")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
