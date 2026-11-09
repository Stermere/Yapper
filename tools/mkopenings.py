"""
mkopenings.py - starting positions for datagen from Yapper's own games or people's.

    python tools/mkopenings.py --out external/data/seeds/variants-6s.txt --first 8 --last 250 \
        --margin 6 --max 200000 external/games/*.ptn external/games/*/*.ptn

Each game gives the moves up to every `step`-th ply from `first` to `last`, and no nearer its end
than `margin`, one line a position. A game with another engine on either side is always left out:
no other engine's play goes into Yapper's data, as games, openings or seeds (docs/DATA.md).

--exclude leaves out the lines of an earlier book, so a new generation starts from positions the
last one did not. --swing keeps only positions that Yapper's own search misjudged: where the score
it gave a position, not yet decided, is that far from a score it gave within the next `within`
plies of the same game. It reads the scores tools/match.py and racetrack keep in the PTN, so it
only ever reads Yapper's own judgement, the other engines' games being left out above.
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


def swung(scores: list[float | None], n: int, args: argparse.Namespace) -> bool:
    """Whether the search after the first n plies misjudged that position."""
    if scores[n] is None or abs(scores[n]) > args.calm:
        return False
    later = (s for s in scores[n + 1:n + 1 + args.within] if s is not None)
    return any(abs(s - scores[n]) >= args.swing for s in later)


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
    parser.add_argument("--exclude", action="append", default=[], help="a book whose lines to skip")
    parser.add_argument("--swing", type=float, default=0.0,
                        help="in flats; keep only positions misjudged by this much")
    parser.add_argument("--within", type=int, default=8, help="plies a swing is looked for in")
    parser.add_argument("--calm", type=float, default=3.0,
                        help="in flats; a position scored past this is decided, not misjudged")
    args = parser.parse_args()

    them = [t.lower() for t in args.them]
    excluded = {line.strip() for book in args.exclude
                for line in Path(book).read_text().splitlines()}
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
            parsed = gamestats.parse_moves(body)
            moves = [MARKS.sub("", m) for m, _ in parsed]
            # Each the score of the search that chose the move, from white's side.
            scores = [float(m[1]) if c and (m := gamestats.SCORE_RE.match(c)) else None
                      for _, c in parsed]
            games += 1
            for n in range(args.first, min(args.last, len(moves) - args.margin) + 1, args.step):
                if args.swing and not swung(scores, n, args):
                    continue
                line = " ".join(moves[:n])
                if line not in excluded:
                    openings[line] = None
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
