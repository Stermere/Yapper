"""
ptnlink.py - a https://ptn.ninja link for a game out of a PTN file.

    python tools/ptnlink.py external/games/run.ptn          # the last game
    python tools/ptnlink.py external/games/run.ptn 17       # the 17th

PTN Ninja reads anything that does not look lz-compressed as plain PTN, so percent-encoding
is all a link needs. To browse a whole match instead, open the file in PTN Ninja directly.

Stdlib only.
"""

from __future__ import annotations

import argparse
import re
import sys
import urllib.parse
from pathlib import Path


def split_games(text: str) -> list[str]:
    """Each game starts at the first tag after a blank line."""
    starts = [m.start() for m in re.finditer(r"(?:^|\n\n)\[", text)]
    return [text[a:b].strip() for a, b in zip(starts, starts[1:] + [len(text)])]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("file")
    parser.add_argument("game", type=int, nargs="?", default=0, help="1-based; default the last")
    args = parser.parse_args()

    games = split_games(Path(args.file).read_text(encoding="utf-8"))
    if not games:
        print(f"no games in {args.file}", file=sys.stderr)
        return 1
    if not 0 <= args.game <= len(games):
        print(f"{args.file} has {len(games)} games", file=sys.stderr)
        return 1
    print("https://ptn.ninja/" + urllib.parse.quote(games[args.game - 1], safe=""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
