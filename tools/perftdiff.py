"""
perftdiff.py - fuzz the engine against tools/takref.py, and seal what they agree on.

    python tools/perftdiff.py                        # 48 games a size, most of an hour
    python tools/perftdiff.py --games 400 --depth 3  # eight times that, for a board rewrite
    python tools/perftdiff.py --seal tests/perft/sealed.perft

Random games on every size are played out by takref alone, and every position they pass
through is put to both implementations: the exact move list, the game result under
several komi values, and perft. Sealing writes positions and counts taken from takref, so
`make perft` can check them in seconds without Python. Re-seal only after a deliberate
rule change, and only after this passes: a suite re-sealed to make it pass stops meaning
anything.
"""

from __future__ import annotations

import argparse
import multiprocessing
import os
import random
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import takref

OUTCOMES = {"": 0, "R-0": 1, "0-R": 2, "F-0": 3, "0-F": 4, "1/2-1/2": 5}


class Engine:
    def __init__(self, path: str):
        self.process = subprocess.Popen([path, "tei"], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, text=True, bufsize=1)

    def send(self, line: str) -> None:
        self.process.stdin.write(line + "\n")
        self.process.stdin.flush()

    def read_until(self, prefix: str) -> str:
        while True:
            line = self.process.stdout.readline()
            if not line:
                raise RuntimeError("engine exited")
            if line.startswith(prefix):
                return line.strip()

    def close(self) -> None:
        self.send("quit")
        self.process.wait(timeout=5)


def random_positions(rng: random.Random, size: int) -> list[str]:
    """TPS of every position of one random game, leaning on spreads to grow stacks."""
    game = takref.Game(size)
    positions = []
    for _ in range(600):
        positions.append(game.tps())
        if game.result() is not None:
            break
        moves = game.legal_moves()
        spreads = [m for m in moves if any(arrow in m for arrow in "<>+-")]
        game.play(rng.choice(spreads if spreads and rng.random() < 0.7 else moves))
    return positions


def compare_position(engine: Engine, tps: str, depth: int) -> list[str]:
    problems = []
    game = takref.Game.from_tps(tps)
    size = game.size
    engine.send(f"teinewgame {size}")
    engine.send(f"position tps {tps}")

    engine.send("moves")
    mine = set(engine.read_until("moves").split()[1:])
    theirs = set(game.legal_moves()) if game.result() is None else set()
    if mine != theirs:
        problems.append(f"moves differ: engine only {sorted(mine - theirs)[:6]}, "
                        f"takref only {sorted(theirs - mine)[:6]}")

    for halfkomi in (0, 3, 4, -3):
        engine.send(f"setoption name HalfKomi value {halfkomi}")
        engine.send("print")
        reported = int(engine.read_until("outcome").split()[1])
        game.halfkomi = halfkomi
        expected = OUTCOMES[game.result() or ""]
        if reported != expected:
            problems.append(f"outcome differs at halfkomi {halfkomi}: "
                            f"engine {reported}, takref {expected}")
    engine.send("setoption name HalfKomi value 0")
    game.halfkomi = 0

    for d in range(1, depth + 1):
        engine.send(f"perft {d}")
        got = int(engine.read_until("perft").split()[1])
        want = takref.perft(game, d)
        if got != want:
            problems.append(f"perft {d}: engine {got}, takref {want}")
    return problems


def fuzz_worker(args) -> tuple[int, list[str]]:
    engine_path, size, seed, depth, deep_every = args
    rng = random.Random(seed)
    engine = Engine(engine_path)
    checked = 0
    failures = []
    for index, tps in enumerate(random_positions(rng, size)):
        d = depth if index % deep_every == 0 else min(depth, 2)
        for problem in compare_position(engine, tps, d):
            failures.append(f"{tps}: {problem}")
        checked += 1
    engine.close()
    return checked, failures


def seal_worker(args) -> str:
    size, tps, depth = args
    game = takref.Game.from_tps(tps)
    counts = [takref.perft(game, d) for d in range(depth + 1)]
    return f"{size} | none | tps {tps} | {' '.join(map(str, counts))}"


def select_for_sealing(seed: int) -> list[tuple[int, str, int]]:
    """A spread over every size and phase, at depths takref can finish."""
    rng = random.Random(seed)
    chosen = []
    for size in range(3, 9):
        for _ in range(8):
            positions = random_positions(rng, size)
            for fraction in (0.3, 0.6, 0.95):
                tps = positions[int((len(positions) - 1) * fraction)]
                chosen.append((size, tps, 3 if size <= 6 else 2))
    return chosen


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--engine", default="./yapper.exe" if os.name == "nt" else "./yapper")
    parser.add_argument("--games", type=int, default=48, help="random games per size")
    parser.add_argument("--depth", type=int, default=3)
    parser.add_argument("--deep-every", type=int, default=9,
                        help="only every Nth position goes to full depth")
    parser.add_argument("--seed", type=int, default=20261031)
    parser.add_argument("--seal", metavar="FILE", help="write a sealed suite instead of fuzzing")
    args = parser.parse_args()

    start = time.time()
    with multiprocessing.Pool() as pool:
        if args.seal:
            lines = pool.map(seal_worker, select_for_sealing(args.seed), chunksize=1)
            header = ("# Sealed from tools/takref.py, not from the engine. Reseal only after a\n"
                      "# deliberate rule change: tools/perftdiff.py says how.\n"
                      "# <size> | <convention> | <position> | <perft 0> <perft 1> ...\n\n")
            Path(args.seal).write_text(header + "\n".join(lines) + "\n", newline="\n")
            print(f"sealed {len(lines)} positions in {time.time() - start:.0f}s")
            return 0

        jobs = [(args.engine, size, args.seed + size * 1000 + game, args.depth, args.deep_every)
                for size in range(3, 9) for game in range(args.games)]
        checked = 0
        failures = []
        for count, found in pool.imap_unordered(fuzz_worker, jobs):
            checked += count
            failures.extend(found)
            if len(failures) > 200:
                break

    for line in failures[:20]:
        print("FAIL", line)
    print(f"{checked} positions, {len(failures)} differences, {time.time() - start:.0f}s")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
