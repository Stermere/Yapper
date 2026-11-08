"""
mkopenings.py - starting positions for datagen from Yapper's own games or people's.

    python tools/mkopenings.py --out external/data/seeds/variants-6s.txt --first 8 --last 250 \
        --margin 6 --max 200000 external/games/*.ptn external/games/*/*.ptn

Each game gives the moves up to every `step`-th ply from `first` to `last`, and no nearer its end
than `margin`, one line a position. A game with another engine on either side is always left out:
no other engine's play goes into Yapper's data, as games, openings or seeds (docs/DATA.md).
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

# A rival is any binary under external/engines, or one named like the engines Yapper meets.
RIVALS = re.compile(r"external[/\\]engines|syntaks|topaz|tiltak|taktician|pentakle|bot",
                    re.IGNORECASE)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("ptn", nargs="+")
    parser.add_argument("--them", action="append", default=[],
                        help="part of a player name; if given, a game counts when a side matches")
    parser.add_argument("--size", type=int, default=6)
    parser.add_argument("--first", type=int, default=10, help="shortest line, in plies")
    parser.add_argument("--last", type=int, default=30, help="longest line, in plies")
    parser.add_argument("--step", type=int, default=4, help="plies between lines of a game")
    parser.add_argument("--margin", type=int, default=1, help="plies a line stops short of the end")
    parser.add_argument("--max", type=int, default=0, help="keep this many lines at random")
    parser.add_argument("--out", required=True)
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()

    them = [t.lower() for t in args.them]
    openings: dict[str, None] = {}
    games = rivals = 0
    for path in args.ptn:
        for game in gamestats.split_games(Path(path).read_text(errors="replace")):
            tags = dict(re.findall(r'\[(\w+) "([^"]*)"\]', game))
            if int(tags.get("Size", "0")) != args.size:
                continue
            players = tags.get("Player1", "") + " " + tags.get("Player2", "")
            if RIVALS.search(players):
                rivals += 1
                continue
            if them and not any(t in players.lower() for t in them):
                continue
            body = game[game.rindex("]") + 1:]
            moves = [MARKS.sub("", m) for m, _ in gamestats.parse_moves(body)]
            games += 1
            for n in range(args.first, min(args.last, len(moves) - args.margin) + 1, args.step):
                openings[" ".join(moves[:n])] = None
    lines = list(openings)
    random.Random(args.seed).shuffle(lines)
    if args.max:
        lines = lines[:args.max]
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines) + "\n", newline="\n")
    print(f"{games} games, {rivals} with another engine left out, {len(openings)} lines, "
          f"{len(lines)} kept, {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
