"""
datagen.py - run self-play on every core, and report how fast it went.

    python tools/datagen.py --out external/data/gen11/6s/gen11 --games 100000 --generation 11
    python tools/datagen.py --plan external/data/gen14/plan.json
    python tools/datagen.py --plan external/data/gen14/plan.json --pause    # from another terminal

The games are played by `yapper datagen`, one process a job, each with its own seed and file, so
nothing is shared and nothing waits. A file is a list of whole games, so the parts of a run can
be read one by one or simply joined. docs/DATA.md has the format.

The files are a run's state. --pause, given what names the run, stops every job at once and loses
only the games being played; the same command run again carries on, and writes the same bytes as
a run that never stopped. A job's file that ends part way through a game, from a pause, a crash or
a reboot, is cut back to its last whole game first. The jobs die with the run however it ends, so
none is left writing to a file that a run started again would write to as well.

A plan is runs played one after another under one name, for a run of a day or more. Its counts
are written down before it starts, so starting it again never changes them, and the engine's
bench is checked at every start, so a rebuild cannot change the games part way:

    {"label": "gen 14", "engine": "external/data/gen14/e70.exe", "bench": 1357917,
     "jobs": 15, "nodes": 20000, "size": 6, "generation": 14,
     "cells": [{"out": "external/data/gen14/6s/r1-self", "games": 21000, "seed": 140000,
                "label": "round 1 self"}, ...]}

A cell can set its own `openings`, `nodes`, `size` and `generation`, but not `jobs`: a job's
games and seed follow from the count of jobs.

Progress goes to external/games/live/datagen-<name>.json as well, for tools/progress.py, from the
lines each job prints as it plays.

Stdlib only.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import datacheck
import progress

LIVE = Path(__file__).resolve().parent.parent / "external" / "games" / "live"
PLAYED_RE = re.compile(r"^(\d+) games (\d+) positions")
BENCH_RE = re.compile(r"^(\d+) nodes")
PAUSED = 3
_job_object = None


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


def read_status(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def die_with_us(process: subprocess.Popen) -> None:
    """Windows leaves a job running when the run that started it is killed or its window closed.
    A job object closes with this process, and takes every job in it along."""
    global _job_object
    if os.name != "nt":
        return
    kernel = ctypes.WinDLL("kernel32")
    kernel.CreateJobObjectW.restype = ctypes.c_void_p
    kernel.SetInformationJobObject.argtypes = (ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p,
                                               ctypes.c_ulong)
    kernel.AssignProcessToJobObject.argtypes = (ctypes.c_void_p, ctypes.c_void_p)
    if _job_object is None:
        class Limits(ctypes.Structure):  # JOBOBJECT_EXTENDED_LIMIT_INFORMATION
            _fields_ = [("user_time", ctypes.c_int64 * 2), ("flags", ctypes.c_uint32),
                        ("working_set", ctypes.c_size_t * 2), ("processes", ctypes.c_uint32),
                        ("affinity", ctypes.c_size_t), ("priority", ctypes.c_uint32),
                        ("scheduling", ctypes.c_uint32), ("io", ctypes.c_uint64 * 6),
                        ("memory", ctypes.c_size_t * 4)]
        limits = Limits(flags=0x2000)  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        _job_object = kernel.CreateJobObjectW(None, None)
        kernel.SetInformationJobObject(_job_object, 9, ctypes.byref(limits),
                                       ctypes.sizeof(limits))
    kernel.AssignProcessToJobObject(_job_object, int(process._handle))


def whole_games(path: Path) -> tuple[int, int]:
    """The games and positions a job's file holds, once a game it left half written is cut off."""
    if not path.exists():
        return 0, 0
    data = path.read_bytes()
    offset = games = positions = 0
    while offset + datacheck.HEADER.size <= len(data):
        header = datacheck.HEADER.unpack_from(data, offset)
        end = offset + datacheck.HEADER.size + 4 * header[5]
        if header[0] != datacheck.MAGIC or end > len(data):
            break
        offset, games, positions = end, games + 1, positions + header[5]
    if offset < len(data) and len(data) - offset < datacheck.HEADER.size + 4 * 512:
        os.truncate(path, offset)
    return games, positions


def follow(process: subprocess.Popen, job: dict) -> None:
    for line in process.stdout:
        job["last"] = line.strip()
        if m := PLAYED_RE.match(line):
            job["games"], job["positions"] = int(m[1]), int(m[2])


def job_files(cell: dict, jobs: int) -> list[tuple[Path, int]]:
    out = Path(cell["out"])
    return [(out.with_name(f"{out.name}-{cell['seed'] + job}.ydg"),
             cell["games"] // jobs + (job < cell["games"] % jobs)) for job in range(jobs)]


def play_cell(engine: str, cell: dict, jobs: int, held: tuple[int, int], status: dict,
              status_path: Path, pause_path: Path) -> tuple[str, int, int]:
    """Plays one cell to its count: "done", "paused" or "failed", and the cell's games and
    positions. `held` is the plan's games and positions outside this cell, for the status."""
    states = []
    for path, games in job_files(cell, jobs):
        path.parent.mkdir(parents=True, exist_ok=True)
        had, positions = whole_games(path)
        state = {"path": path, "games": had, "positions": positions, "last": "", "process": None}
        if had < games:
            command = [engine, "datagen", str(path), str(games),
                       str(cell["seed"] + len(states)), str(cell["nodes"]), str(cell["size"]),
                       str(cell["generation"])]
            if cell.get("openings"):
                command.append(str(Path(cell["openings"]).resolve()))
            state["process"] = subprocess.Popen(command, stdout=subprocess.PIPE, text=True)
            die_with_us(state["process"])
            state["reader"] = threading.Thread(target=follow, args=(state["process"], state),
                                               daemon=True)
            state["reader"].start()
        states.append(state)

    def tally() -> tuple[int, int]:
        games, positions = sum(s["games"] for s in states), sum(s["positions"] for s in states)
        status.update(games=held[0] + games, positions=held[1] + positions)
        return games, positions

    running = [s for s in states if s["process"]]
    written = 0.0
    while any(s["process"].poll() is None for s in running):
        if pause_path.exists():
            for s in running:
                s["process"].kill()
            for s in running:
                s["process"].wait()
            return ("paused", *tally())
        if time.monotonic() - written >= 5:
            tally()
            write_status(status_path, status)
            written = time.monotonic()
        time.sleep(1)

    failed = False
    for s in running:
        s["reader"].join()
        if s["process"].returncode != 0:
            print(f"{s['path'].name} failed: {s['last'] or 'no output'}", file=sys.stderr)
            failed = True
    return ("failed" if failed else "done", *tally())


def bench_of(engine: str) -> int | None:
    lines = subprocess.run([engine, "bench"], capture_output=True, text=True).stdout.splitlines()
    m = BENCH_RE.match(lines[-1]) if lines else None
    return int(m[1]) if m else None


def load_plan(path: Path) -> tuple[dict, list[dict]]:
    plan = json.loads(path.read_text(encoding="utf-8"))
    cells = []
    for cell in plan["cells"]:
        if "jobs" in cell:
            raise ValueError(f"{path}: a cell cannot set jobs")
        cells.append({**{k: plan[k] for k in ("nodes", "size", "generation") if k in plan},
                      **({"openings": plan["openings"]} if plan.get("openings") else {}), **cell})
    return plan, cells


def main() -> int:
    default_engine = "./yapper.exe" if os.name == "nt" else "./yapper"
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--plan", help="a JSON plan of runs one after another, in place of --out")
    parser.add_argument("--pause", action="store_true",
                        help="stop the run this names, from another terminal; run it again to "
                             "carry on")
    parser.add_argument("--engine", default=default_engine)
    parser.add_argument("--out", help="file names start with this")
    parser.add_argument("--games", type=int, help="in total, across all jobs")
    # 20,000 a move taught a network far more than 5000 in the same time, E38.
    parser.add_argument("--nodes", type=int, default=20000, help="search budget per move")
    parser.add_argument("--size", type=int, default=6)
    parser.add_argument("--generation", type=int, default=0)
    parser.add_argument("--seed", type=int, default=1, help="jobs use this and the next ones")
    # A budget in nodes cannot flag, so unlike a match this can use every logical core.
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    parser.add_argument("--openings", help="a book from tools/mkopenings.py, in place of random plies")
    parser.add_argument("--label", help="what tools/progress.py calls the run")
    parser.add_argument("--status", help="progress as JSON; default external/games/live/datagen-<name>.json")
    args = parser.parse_args()

    if args.plan:
        plan_path = Path(args.plan)
        plan, cells = load_plan(plan_path)
        engine, jobs = plan["engine"], plan["jobs"]
        name, label, out = plan_path.parent.name, plan.get("label") or plan_path.stem, plan_path
    elif args.out and (args.games or args.pause):
        out = Path(args.out)
        engine, jobs, name = args.engine, args.jobs, out.name
        label = args.label or f"datagen {out.name}"
        plan = {}
        cells = [{"out": args.out, "games": args.games or 0, "seed": args.seed,
                  "nodes": args.nodes, "size": args.size, "generation": args.generation,
                  "openings": args.openings}]
    else:
        parser.error("give --plan, or --out with --games")
    status_path = Path(args.status) if args.status else LIVE / f"datagen-{name}.json"
    pause_path = status_path.with_suffix(".pause")
    before = read_status(status_path)
    running = before.get("pid") if not before.get("finished") and \
        progress.alive(before.get("pid")) else None

    if args.pause:
        if not running:
            print(f"{label} is not running")
            return 1
        pause_path.touch()
        print(f"{label} stops within a few seconds; the same command without --pause carries on")
        return 0
    if running:
        print(f"{label} is running already, as process {running}", file=sys.stderr)
        return 1
    # A pause asked for while nothing was playing has already happened.
    pause_path.unlink(missing_ok=True)

    engine = str(Path(engine).resolve())
    if plan.get("bench") and (bench := bench_of(engine)) != plan["bench"]:
        print(f"{engine} benches {bench}, and the plan was written for {plan['bench']}",
              file=sys.stderr)
        return 1

    # Each cell's games and positions so far.
    held = [[sum(n) for n in zip(*(whole_games(path) for path, _ in job_files(cell, jobs)))]
            for cell in cells]
    # Time already spent counts, so the rate tools/progress.py shows is the run's own.
    spent = before["updated"] - before["started"] if not before.get("finished") and \
        before.get("label", "").startswith(label) and "updated" in before else 0.0
    started, held_at_start = time.monotonic(), sum(g for g, _ in held)
    status = {"kind": "datagen", "label": label, "pid": os.getpid(), "out": str(out.resolve()),
              "nodes": cells[0]["nodes"], "size": cells[0]["size"], "jobs": jobs,
              "cap": sum(c["games"] for c in cells), "games": held_at_start,
              "positions": sum(p for _, p in held), "started": time.time() - spent,
              "finished": False, "resumable": True, "cells": len(cells), "cell": 0}
    write_status(status_path, status)

    outcome = "done"
    for i, cell in enumerate(cells):
        if held[i][0] >= cell["games"]:
            continue
        status.update(cell=i + 1, nodes=cell["nodes"], size=cell["size"],
                      label=f"{label}: {cell['label']}" if cell.get("label") else label)
        others = (sum(g for g, _ in held) - held[i][0], sum(p for _, p in held) - held[i][1])
        outcome, *held[i] = play_cell(engine, cell, jobs, others, status, status_path,
                                      pause_path)
        if outcome != "done":
            break

    seconds = time.monotonic() - started
    status.update(label=label, finished=outcome == "done", code=1 if outcome == "failed" else 0)
    write_status(status_path, status)
    pause_path.unlink(missing_ok=True)
    played = status["games"] - held_at_start
    written = sum(path.stat().st_size for cell in cells for path, _ in job_files(cell, jobs)
                  if path.exists())
    if outcome == "paused":
        print(f"paused at {status['games']} of {status['cap']} games; the same command carries on")
        return PAUSED
    print(f"{played} games, {written / 1e6:.1f} MB, {seconds:.1f} seconds, "
          f"{played / seconds:.1f} games/s on {jobs} jobs")
    return 1 if outcome == "failed" else 0


if __name__ == "__main__":
    sys.exit(main())
