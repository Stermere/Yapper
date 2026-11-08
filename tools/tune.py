"""
tune.py - SPSA over the tunables in src/tune.h, by self-play of a `make TUNE=on` build.

    make TUNE=on EXE=yapper-tune
    python tools/tune.py --list
    python tools/tune.py --name lmr --params LmrBase,LmrDivisor,LmrMoves \
        --pairs 20000 --tc 2+0.02 --book tests/books/6s_4ply.txt
    python tools/tune.py --name lmr --pause     # from another terminal
    python tools/tune.py --name lmr --resume
    python tools/tune.py --name lmr --report

An iteration draws a direction for every parameter at once, and plays one opening as a pair with
colours reversed between the two engines either side of the values, `plus` and `minus`. Every
parameter then moves along plus's wins less its losses. One pair is one iteration, as on
fishtest, so r_end means what it means there. Workers do not wait for each other: each takes the
values as they stand when it starts a pair, so every core stays busy.

The parameters come from the engine's `tei` handshake, with the defaults and ranges written in
src/tune.h, so a tunable added there is picked up here with nothing else to change. Each is
perturbed by a twentieth of its range at the end of the run, at least 1, and by more before.

How far a parameter travels goes as r_end times the number of pairs. A short run at fishtest's
0.002 measures a gradient and then barely acts on it, and reads as "the defaults are right" when
they are not: scale r_end up as the run gets shorter.

**What comes out is a candidate, not a result.** Nothing in SPSA tests a null hypothesis: it
walks wherever its noise leads and reports the end with a straight face. Before any default in
src/tune.h changes, the candidate must pass tools/match.py's SPRT against the values it started
from, and should be confirmed at a time control the run did not tune at. --report prints that
command. Record both in docs/EXPERIMENTS.md.

State goes to external/tune/<name>.json after every pair, so a run survives being stopped, and
every pair's values to <name>.csv beside it. Progress goes to external/games/live, which
tools/progress.py shows like any match, and as paused once it stops short of the end.

A run plays a copy of its engine, external/tune/<name>.exe, so a rebuild in the days a long run
may take cannot change the binary between one session and the next. --pause lets the pairs being
played finish and then exits; Ctrl+C, a closed window or a reboot lose only those pairs, which
--resume draws again.

Stdlib only.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import csv
import json
import math
import multiprocessing
import os
import random
import shutil
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import match

ROOT = Path(__file__).resolve().parent.parent
RUNS = ROOT / "external" / "tune"

# Spall's exponents, which fishtest uses. Not worth tuning: c_end and r_end are.
ALPHA = 0.602
GAMMA = 0.101

# Engine options that configure rather than play.
NOT_TUNABLE = {"Hash", "HalfKomi"}


def default_engine() -> str:
    return "./yapper-tune.exe" if os.name == "nt" else "./yapper-tune"


def query_tunables(engine: str) -> dict[str, tuple[int, int, int]]:
    """{name: (default, min, max)} for every spin option the engine declares that plays."""
    try:
        out = subprocess.run([engine], input="tei\nquit\n", capture_output=True, text=True,
                             timeout=30).stdout
    except (OSError, subprocess.TimeoutExpired) as error:
        raise SystemExit(f"cannot start {engine}: {error}")
    found = {}
    for line in out.splitlines():
        words = line.split()
        # option name RfpMargin type spin default 90 min 20 max 300
        if len(words) == 11 and words[:2] == ["option", "name"] and words[3:5] == ["type", "spin"]:
            if words[2] not in NOT_TUNABLE:
                found[words[2]] = (int(words[6]), int(words[8]), int(words[10]))
    return found


def rounded(x: float) -> int:
    """Half up, not to even, so a value on .5 goes the same way whichever side it came from."""
    return int(math.floor(x + 0.5))


class Param:
    """One tunable. `c_end` is how far the perturbation reaches on the last pair: too small and
    the result is noise, too large and the two engines are not the one being tuned. `r_end` is
    the last step as a fraction of it, per unit of result."""

    def __init__(self, name: str, value: float, lo: int, hi: int, c_end: float, r_end: float,
                 start: float | None = None):
        self.name, self.value, self.lo, self.hi = name, float(value), lo, hi
        self.c_end, self.r_end = c_end, r_end
        self.start = float(value if start is None else start)

    def clamp(self, x: float) -> float:
        return min(float(self.hi), max(float(self.lo), x))

    def gains(self, k: int, n: int) -> tuple[float, float]:
        """(a_k, c_k) for the pair numbered k of n, in fishtest's schedule."""
        big_a = 0.1 * n
        a = self.r_end * self.c_end ** 2 * (big_a + n) ** ALPHA
        c = self.c_end * n ** GAMMA
        return a / (big_a + k + 1) ** ALPHA, c / (k + 1) ** GAMMA

    def to_dict(self) -> dict:
        return {"name": self.name, "value": self.value, "lo": self.lo, "hi": self.hi,
                "c_end": self.c_end, "r_end": self.r_end, "start": self.start}

    @staticmethod
    def from_dict(d: dict) -> Param:
        return Param(d["name"], d["value"], d["lo"], d["hi"], d["c_end"], d["r_end"], d["start"])


@dataclass
class Task:
    k: int
    deltas: list[int]
    plus: dict[str, int]
    minus: dict[str, int]
    openings: list[list[str]]


@dataclass
class Config:
    command: list[str]
    options: dict[str, str]
    limits: match.Limits
    size: int
    halfkomi: int


# Each worker is a process for the reason match.py gives: the referee would hold the GIL while
# another game's clock ran.
WORKER: dict = {}


def worker_start(config: Config) -> None:
    WORKER["config"] = config
    # A dict each: Engine keeps the one it is given, and set_values() writes to it.
    WORKER["plus"] = match.Engine(config.command, dict(config.options))
    WORKER["minus"] = match.Engine(config.command, dict(config.options))
    threading.Thread(target=die_with_parent, daemon=True).start()


def die_with_parent() -> None:
    """Windows leaves a pool's workers running when the tuner is killed, each with two engines
    still playing, and they would steal the clock from the run that resumes."""
    multiprocessing.parent_process().join()
    for side in ("plus", "minus"):
        WORKER[side].process.kill()
    os._exit(1)


def set_values(engine: match.Engine, values: dict[str, int]) -> match.Engine:
    """Only what changed is sent, and a revived engine is started with what it last had."""
    engine = match.revived(engine)
    for name, value in values.items():
        if engine.options.get(name) != str(value):
            engine.send(f"setoption name {name} value {value}")
            engine.options[name] = str(value)
    return engine


def run_task(task: Task) -> tuple[list[float], list[str]]:
    """Plus's score in every game, in pair order, and how the games that did not end on the
    board ended."""
    config: Config = WORKER["config"]
    scores, endings = [], []
    for opening in task.openings:
        for plus_is_white in (True, False):
            plus = WORKER["plus"] = set_values(WORKER["plus"], task.plus)
            minus = WORKER["minus"] = set_values(WORKER["minus"], task.minus)
            white, black = (plus, minus) if plus_is_white else (minus, plus)
            result = match.play_game(white, black, opening, config.size, config.halfkomi,
                                     (config.limits, config.limits))
            score = result.score_for_white()
            scores.append(score if plus_is_white else 1.0 - score)
            if result.termination:
                endings.append(result.termination.split(":")[0])
    return scores, endings


class Run:
    def __init__(self, name: str):
        self.name = name
        self.state_path = RUNS / f"{name}.json"
        self.csv_path = RUNS / f"{name}.csv"
        self.pause_path = RUNS / f"{name}.pause"
        self.status_path = match.LIVE / f"spsa-{name}.json"
        self.engine = ""
        self.params: list[Param] = []
        self.total = 0  # pairs
        self.done = 0
        self.issued = 0
        self.batch = 1
        self.tc = "2+0.02"
        self.nodes = 0
        self.size = 6
        self.halfkomi = 4
        self.book: str | None = None
        self.hash_mb = 0
        self.concurrency = 1
        self.seed = 0
        self.cursor = 0
        self.elapsed = 0.0  # seconds spent playing, over every session
        self.rng = random.Random()
        self.tally = match.Tally()
        self.openings: list[list[str]] = []

    # -- persistence

    def save(self) -> None:
        RUNS.mkdir(parents=True, exist_ok=True)
        state = {
            "engine": self.engine, "params": [p.to_dict() for p in self.params],
            "total": self.total, "done": self.done, "batch": self.batch, "tc": self.tc,
            "nodes": self.nodes, "size": self.size, "halfkomi": self.halfkomi, "book": self.book,
            "hash": self.hash_mb, "concurrency": self.concurrency, "seed": self.seed,
            "cursor": self.cursor, "elapsed": self.elapsed, "rng": self.rng.getstate(),
            "tally": {"w": self.tally.wins, "l": self.tally.losses, "d": self.tally.draws,
                      "pairs": self.tally.pairs, "games": self.tally.games,
                      "endings": self.tally.endings},
        }
        temp = self.state_path.with_suffix(".tmp")
        temp.write_text(json.dumps(state, indent=1), encoding="utf-8")
        os.replace(temp, self.state_path)

    def load(self) -> None:
        d = json.loads(self.state_path.read_text(encoding="utf-8"))
        self.engine = d["engine"]
        self.params = [Param.from_dict(p) for p in d["params"]]
        self.total, self.done, self.batch = d["total"], d["done"], d["batch"]
        self.tc, self.nodes = d["tc"], d["nodes"]
        self.size, self.halfkomi = d["size"], d["halfkomi"]
        self.book, self.hash_mb, self.concurrency = d["book"], d["hash"], d["concurrency"]
        self.seed, self.cursor, self.elapsed = d["seed"], d["cursor"], d["elapsed"]
        # JSON hands the state back as lists, and setstate wants its tuple.
        version, internal, gauss = d["rng"]
        self.rng.setstate((version, tuple(internal), gauss))
        t = d["tally"]
        self.tally.wins, self.tally.losses, self.tally.draws = t["w"], t["l"], t["d"]
        self.tally.pairs, self.tally.games = t["pairs"], t["games"]
        self.tally.endings = t["endings"]
        # Pairs that were being played when it stopped are drawn again, and so is one logged
        # after the last save, so its row goes.
        self.issued = self.done
        if self.csv_path.exists():
            with self.csv_path.open(newline="", encoding="utf-8") as source:
                rows = list(csv.reader(source))
            kept = rows[:1] + [row for row in rows[1:] if int(row[0]) <= self.done]
            if len(kept) != len(rows):
                with self.csv_path.open("w", newline="", encoding="utf-8") as out:
                    csv.writer(out).writerows(kept)

    def log_row(self, result: float) -> None:
        new = not self.csv_path.exists()
        with self.csv_path.open("a", newline="", encoding="utf-8") as out:
            writer = csv.writer(out)
            if new:
                writer.writerow(["pairs", "result"] + [p.name for p in self.params])
            writer.writerow([self.done, f"{result:+g}"] + [f"{p.value:.3f}" for p in self.params])

    # -- the algorithm

    def prepare(self) -> None:
        self.openings = match.read_book(self.book, self.size)
        random.Random(self.seed).shuffle(self.openings)

    def draw(self) -> Task:
        """The next iteration's two engines, from the values as they stand."""
        k = self.issued
        deltas = [1 if self.rng.random() < 0.5 else -1 for _ in self.params]
        plus, minus = {}, {}
        for p, d in zip(self.params, deltas):
            _, ck = p.gains(k, self.total)
            plus[p.name] = rounded(p.clamp(p.value + ck * d))
            minus[p.name] = rounded(p.clamp(p.value - ck * d))
        openings = []
        for _ in range(self.batch):
            openings.append(self.openings[self.cursor % len(self.openings)])
            self.cursor += 1
        self.issued += self.batch
        return Task(k, deltas, plus, minus, openings)

    def apply(self, task: Task, scores: list[float], endings: list[str]) -> float:
        # Plus's wins less its losses, as fishtest counts a pair.
        result = sum(2.0 * s - 1.0 for s in scores)
        for p, d in zip(self.params, task.deltas):
            ak, _ = p.gains(task.k, self.total)
            # The separation the engines got, not the one asked for: both went through rounding,
            # and near a bound through the clamp, which can halve it or take it away. Dividing by
            # c_k there would understate the gradient exactly where it is already weakest. None
            # at all says nothing about this parameter, whatever it says about the rest.
            eff = (task.plus[p.name] - task.minus[p.name]) / 2.0 * d
            if eff > 0.0:
                p.value = p.clamp(p.value + ak * result / eff * d)
        for i in range(0, len(scores) - 1, 2):
            self.tally.add_pair(scores[i], scores[i + 1])
        for key in endings:
            self.tally.endings[key] = self.tally.endings.get(key, 0) + 1
        self.done += len(scores) // 2
        self.log_row(result)
        return result

    # -- display

    def condition(self) -> str:
        return f"{self.nodes} nodes" if self.nodes else self.tc

    def movers(self, count: int) -> list[Param]:
        """The parameters that have travelled furthest for their range."""
        def travel(p: Param) -> float:
            return abs(p.value - p.start) / max(1, p.hi - p.lo)
        return sorted(self.params, key=travel, reverse=True)[:count]

    def moved_text(self, count: int) -> str:
        return "  ".join(f"{p.name} {p.start:g}>{p.value:.1f}" for p in self.movers(count))

    def write_status(self, started: float) -> None:
        # Finished only at the end: a run that stopped short is paused, whatever stopped it.
        status = {"label": f"SPSA {self.name}", "pid": os.getpid(), "dev": self.engine,
                  "base": self.engine, "tc": self.condition(), "ptn": None,
                  "cap": 2 * self.total, "sprt": None, "lower": None, "upper": None,
                  "started": started, "finished": self.done >= self.total, "verdict": None,
                  "spsa": [[p.name, p.start, p.value, p.lo, p.hi] for p in self.params]}
        match.write_status(self.status_path, status, self.tally, None)

    def report(self) -> None:
        width = max(len(p.name) for p in self.params)
        print(f"\n{self.name}: {self.done}/{self.total} pairs at {self.condition()}, "
              f"{self.size}x{self.size}, plus vs minus {self.tally.line().strip()}")
        print(f"  {'name':<{width}} {'start':>7} {'now':>9} {'shipped':>8}   range    c_end")
        for p in self.params:
            print(f"  {p.name:<{width}} {p.start:>7g} {p.value:>9.2f} {rounded(p.value):>8}   "
                  f"[{p.lo}, {p.hi}]  {p.c_end:g}")
        changed = [p for p in self.params if rounded(p.value) != rounded(p.start)]
        if not changed:
            print("\nNo parameter has moved far enough to round to a new value yet.")
            return
        options = ",".join(f"{p.name}={rounded(p.value)}" for p in changed)
        clock = f"--dev-nodes {self.nodes} --base-nodes {self.nodes}" if self.nodes \
            else f"--tc {self.tc}"
        book = f" --book {self.book}" if self.book else ""
        print("\nThe candidate, against the values it started from, by the same binary:")
        print(f"  python tools/match.py --dev {self.engine} --base {self.engine} "
              f"--dev-options {options} --sprt --size {self.size} --halfkomi {self.halfkomi} "
              f"{clock}{book} --ptn external/games/tune/{self.name}.ptn")
        print("\nIf it passes, these are its lines in src/tune.h:")
        for p in changed:
            print(f"  X({p.name}, {rounded(p.value)}, {p.lo}, {p.hi})")
        print("\nThis is a candidate, not a result. SPRT it, confirm it at another time control, "
              "and record both in docs/EXPERIMENTS.md before any default changes.")


def parse_c_end(text: str | None) -> tuple[float | None, dict[str, float]]:
    """`4` for every parameter, or `Name=4,Other=10` for some."""
    if not text:
        return None, {}
    if "=" not in text:
        return float(text), {}
    pairs = (item.split("=", 1) for item in text.split(",") if item)
    return None, {name: float(value) for name, value in pairs}


def play(run: Run, started_at: float) -> str:
    """Plays until the run is done, paused or stopped, and says which."""
    command = [run.engine]
    options = {"HalfKomi": str(run.halfkomi)}
    if run.hash_mb:
        options["Hash"] = str(run.hash_mb)
    base, inc = match.parse_tc(run.tc)
    config = Config(command, options, match.Limits(base, inc, run.nodes, 0), run.size, run.halfkomi)

    # A bad path or option must fail here, before a pool of workers each fails on it.
    try:
        match.Engine(command, dict(options)).close()
    except (OSError, match.EngineError) as error:
        print(f"cannot start {run.engine}: {error}", file=sys.stderr)
        return "stopped"

    session_start = time.monotonic()
    elapsed_before = run.elapsed
    last_print = 0.0
    pausing = False
    pending: dict[concurrent.futures.Future, Task] = {}
    pool = concurrent.futures.ProcessPoolExecutor(run.concurrency, initializer=worker_start,
                                                  initargs=(config,))
    try:
        while True:
            if not pausing and run.pause_path.exists():
                pausing = True
                print(f"\npausing once the {len(pending)} pairs being played end", flush=True)
            while not pausing and len(pending) < run.concurrency and run.issued < run.total:
                task = run.draw()
                pending[pool.submit(run_task, task)] = task
            if not pending:
                break
            # A timeout, so a pause is seen while every worker is mid-game.
            finished, _ = concurrent.futures.wait(
                pending, timeout=1.0, return_when=concurrent.futures.FIRST_COMPLETED)
            if not finished:
                continue
            for future in finished:
                task = pending.pop(future)
                scores, endings = future.result()
                run.apply(task, scores, endings)
            run.elapsed = elapsed_before + time.monotonic() - session_start
            run.save()
            run.write_status(started_at)
            if time.monotonic() - last_print >= 15.0 or run.done >= run.total:
                last_print = time.monotonic()
                left = (run.total - run.done) * run.elapsed / max(1, run.done)
                print(f"{run.done:7}/{run.total} pairs  +{run.tally.wins} -{run.tally.losses} "
                      f"={run.tally.draws}  ~{left / 3600:.1f}h left  {run.moved_text(5)}",
                      flush=True)
    except (KeyboardInterrupt, Exception) as error:
        # A pair in flight is lost, and drawn again on --resume.
        reason = "stopped" if isinstance(error, KeyboardInterrupt) else f"failed: {error!r}"
        print(f"\n{reason}. The state is saved, and --resume carries on from pair {run.done}.",
              flush=True)
        pool.shutdown(wait=False, cancel_futures=True)
        run.save()
        run.write_status(started_at)
        return "stopped"
    pool.shutdown()
    run.save()
    run.write_status(started_at)
    run.pause_path.unlink(missing_ok=True)
    if run.done < run.total:
        print(f"\npaused at pair {run.done}; --resume carries on.", flush=True)
        return "paused"
    return "finished"


def main() -> int:
    parser = argparse.ArgumentParser(description="SPSA over the tunables of a TUNE=on build.")
    parser.add_argument("--engine", default=default_engine(), help="a `make TUNE=on` build")
    parser.add_argument("--name", default="spsa", help="names the run's files")
    parser.add_argument("--list", action="store_true", help="what the engine exposes, then exit")
    parser.add_argument("--params", help="comma-separated; default every tunable")
    parser.add_argument("--exclude", default="", help="comma-separated names to leave alone")
    parser.add_argument("--pairs", type=int, default=20000, help="iterations, one pair each")
    parser.add_argument("--batch", type=int, default=1, help="pairs a worker plays per draw")
    parser.add_argument("--tc", default="2+0.02", help="base+increment, seconds")
    parser.add_argument("--nodes", type=int, default=0,
                        help="a fixed nodes limit a move instead of the clock, which the Time "
                             "tunables never see")
    parser.add_argument("--size", type=int, default=6)
    parser.add_argument("--halfkomi", type=int, default=4)
    parser.add_argument("--book", help="one opening per line, as PTN moves")
    parser.add_argument("--hash", type=int, default=0, help="megabytes; default the engine's")
    parser.add_argument("--concurrency", type=int,
                        help="default half the cores; given with --resume, it replaces the run's")
    parser.add_argument("--seed", type=int, default=0, help="0 picks one and records it")
    parser.add_argument("--c-end", help="the last perturbation: N for all, or Name=N,...; "
                                        "default a twentieth of the range, at least 1")
    parser.add_argument("--r-end", type=float, default=0.002,
                        help="the last step for each unit of result, as a fraction of c_end")
    parser.add_argument("--resume", action="store_true", help="carry on with run --name")
    parser.add_argument("--pause", action="store_true",
                        help="ask run --name to stop once its pairs in flight end, then exit")
    parser.add_argument("--report", action="store_true", help="where run --name stands, then exit")
    parser.add_argument("--dry-run", action="store_true",
                        help="show the run and its first two engines, then exit")
    args = parser.parse_args()

    run = Run(args.name)

    if args.report or args.resume or args.pause:
        if not run.state_path.exists():
            print(f"no run named {args.name}: {run.state_path} does not exist", file=sys.stderr)
            return 1
        if args.pause:
            run.pause_path.touch()
            print(f"{args.name} pauses once the pairs it is playing end; --resume carries on")
            return 0
        run.load()
        if args.report:
            run.report()
            return 0
        if run.done >= run.total:
            print(f"{args.name} has played all {run.total} pairs")
            run.report()
            return 0
        if args.concurrency:
            run.concurrency = max(1, args.concurrency)
        # A pause asked for while nothing was playing has already happened.
        run.pause_path.unlink(missing_ok=True)
    else:
        engine = str(Path(args.engine).resolve())
        tunables = query_tunables(engine)
        if not tunables:
            print(f"{args.engine} declares no tunables. Build one with: make TUNE=on "
                  f"EXE=yapper-tune", file=sys.stderr)
            return 1
        if args.list:
            width = max(len(name) for name in tunables)
            for name, (value, lo, hi) in tunables.items():
                print(f"  {name:<{width}} {value:>7}   {f'[{lo}, {hi}]':<14} c_end "
                      f"{max(1.0, (hi - lo) / 20.0):g}")
            return 0
        if run.state_path.exists() and not args.dry_run:
            print(f"a run named {args.name} exists: --resume it, --report it, or pick another "
                  f"--name", file=sys.stderr)
            return 1

        wanted = [s.strip() for s in args.params.split(",") if s.strip()] if args.params \
            else list(tunables)
        excluded = {s.strip() for s in args.exclude.split(",") if s.strip()}
        everyone, each = parse_c_end(args.c_end)
        for name in wanted + list(excluded) + list(each):
            if name not in tunables:
                print(f"the engine has no tunable {name}; --list shows them", file=sys.stderr)
                return 1
        for name in wanted:
            if name in excluded:
                continue
            value, lo, hi = tunables[name]
            c_end = each.get(name, everyone or max(1.0, (hi - lo) / 20.0))
            run.params.append(Param(name, value, lo, hi, c_end, args.r_end))
        if not run.params:
            print("nothing left to tune", file=sys.stderr)
            return 1

        run.engine = engine
        run.total = args.pairs
        run.batch = max(1, args.batch)
        run.tc, run.nodes = args.tc, args.nodes
        run.size, run.halfkomi = args.size, args.halfkomi
        run.book = args.book
        run.hash_mb = args.hash
        run.concurrency = max(1, args.concurrency or (os.cpu_count() or 2) // 2)
        run.seed = args.seed or random.randrange(1, 2 ** 31)
        run.rng = random.Random(run.seed)

    if not args.resume and not args.dry_run:
        RUNS.mkdir(parents=True, exist_ok=True)
        own = RUNS / f"{run.name}{Path(run.engine).suffix}"
        shutil.copy2(run.engine, own)
        run.engine = str(own)
        run.pause_path.unlink(missing_ok=True)

    run.prepare()
    print(f"SPSA {run.name}: {len(run.params)} parameters, {run.done}/{run.total} pairs at "
          f"{run.condition()} on {run.size}x{run.size}, {run.concurrency} at once, seed {run.seed}")
    print(f"  {run.engine}")
    print(f"  {', '.join(p.name for p in run.params)}")

    if args.dry_run:
        task = run.draw()
        width = max(len(p.name) for p in run.params)
        print(f"\n  the first pair, opening {' '.join(task.openings[0])}:")
        print(f"  {'name':<{width}} {'now':>7} {'plus':>7} {'minus':>7}   c_0")
        for p in run.params:
            print(f"  {p.name:<{width}} {p.value:>7g} {task.plus[p.name]:>7} "
                  f"{task.minus[p.name]:>7}   {p.gains(0, run.total)[1]:.2f}")
        return 0

    if not args.resume:
        run.save()
    outcome = play(run, time.time() - run.elapsed)
    run.report()
    return 1 if outcome == "stopped" else 0


if __name__ == "__main__":
    sys.exit(main())
