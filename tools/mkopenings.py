"""
mkopenings.py - openings for datagen from various games.

    python tools/mkopenings.py --them syntaks --them topaz --out external/data/openings/vs6.txt \
        external/games/tak/*.ptn external/games/e49/*.ptn external/games/hc/*.ptn
"""

from __future__ import annotations

import argparse
import random
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import gamestats

MARKS = re.compile(r"[*'!?]+$")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("ptn", nargs="+")
    parser.add_argument("--them", action="append", required=True,
                        help="part of a player name; a game counts when either side matches")
    parser.add_argument("--size", type=int, default=6)
    parser.add_argument("--first", type=int, default=10, help="shortest opening, in plies")
    parser.add_argument("--last", type=int, default=30, help="longest opening, in plies")
    parser.add_argument("--step", type=int, default=4, help="plies between openings of a game")
    parser.add_argument("--out", required=True)
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()

    them = [t.lower() for t in args.them]
    openings: dict[str, None] = {}
    games = 0
    for path in args.ptn:
        for game in gamestats.split_games(Path(path).read_text(errors="replace")):
            tags = dict(re.findall(r'\[(\w+) "([^"]*)"\]', game))
            if int(tags.get("Size", "0")) != args.size:
                continue
            players = (tags.get("Player1", "") + " " + tags.get("Player2", "")).lower()
            if not any(t in players for t in them):
                continue
            body = game[game.rindex("]") + 1:]
            moves = [MARKS.sub("", m) for m, _ in gamestats.parse_moves(body)]
            games += 1
            for n in range(args.first, min(args.last, len(moves) - 1) + 1, args.step):
                openings[" ".join(moves[:n])] = None
    lines = list(openings)
    random.Random(args.seed).shuffle(lines)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines) + "\n")
    print(f"{games} games, {len(lines)} openings, {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
