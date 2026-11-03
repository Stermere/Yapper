"""
mkbook.py - build an opening book of four-ply positions the engine sees as typical.

    python tools/mkbook.py --size 6 --halfkomi 4 --out tests/books/6s_4ply.txt
    python tools/mkbook.py --size 3 --plies 2 --out tests/books/3s_2ply.txt

An opening is only fair if it does not hand one side the game, and match.py plays each one
with both colours, so what is left of any bias cancels. The filter is the engine's own
shallow search, so rebuild a book only with a reason: changing it changes every later match.

Stdlib only.
"""

from __future__ import annotations

import argparse
import math
import os
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import takref
from perftdiff import Engine


def search_score(engine: Engine, opening: str, depth: int) -> int:
    engine.send(f"position startpos moves {opening}")
    engine.send(f"go depth {depth}")
    score = 0
    while True:
        line = engine.process.stdout.readline()
        if not line:
            raise RuntimeError("engine exited")
        if line.startswith("bestmove"):
            return score
        words = line.split()
        if "cp" in words:
            score = int(words[words.index("cp") + 1])


def main() -> int:
    default_engine = "./yapper.exe" if os.name == "nt" else "./yapper"
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--engine", default=default_engine)
    parser.add_argument("--size", type=int, default=6)
    parser.add_argument("--plies", type=int, default=4)
    parser.add_argument("--halfkomi", type=int, default=4)
    parser.add_argument("--count", type=int, default=1000)
    parser.add_argument("--depth", type=int, default=6)
    parser.add_argument("--sample", type=int, default=3, help="openings tried per one kept")
    parser.add_argument("--seed", type=int, default=20261031)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    rng = random.Random(args.seed)
    engine = Engine(args.engine)
    engine.send(f"setoption name HalfKomi value {args.halfkomi}")
    engine.send(f"teinewgame {args.size}")

    squares = [takref.square_name(r, f) for r in range(args.size) for f in range(args.size)]
    # A small board has fewer openings than were asked for; then every one of them is kept.
    wanted = min(args.count * args.sample, math.perm(len(squares), args.plies))
    count = min(args.count, wanted)
    scored: dict[str, int] = {}
    while len(scored) < wanted:
        opening = " ".join(rng.sample(squares, args.plies))
        if opening not in scored:
            scored[opening] = search_score(engine, opening, args.depth)
    engine.close()

    # The engine's opinion of an even position drifts with komi and with the evaluation, so
    # a fixed window around zero can reject nearly everything. The median follows it.
    median = sorted(scored.values())[len(scored) // 2]
    even = sorted(scored, key=lambda opening: abs(scored[opening] - median))[:count]
    rng.shuffle(even)
    book = {opening: None for opening in even}
    spread = [scored[opening] for opening in even]
    print(f"median {median} cp, kept {min(spread)} to {max(spread)} cp")

    Path(args.out).write_text("\n".join(book) + "\n", newline="\n")
    print(f"{args.out}: kept {len(book)} of {len(scored)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
