"""
datacheck.py - read training games without the engine, and check them against the rules.

    python tools/datacheck.py external/data/gen1-*.ydg

Every game gets its header summed up. A sample is then replayed through tools/takref.py, which
shares nothing with src/: each move must be legal, and the game must end the way its header says.
This is also the reference reader for docs/DATA.md, for anything that trains from these files.

Stdlib only.
"""

from __future__ import annotations

import argparse
import statistics
import struct
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator

sys.path.insert(0, str(Path(__file__).parent))
import takref

MAGIC = 0x31474459
NO_SCORE = 0x7FFF
HEADER = struct.Struct("<IBbBBHBBI")

ONGOING, WHITE_ROAD, BLACK_ROAD, WHITE_FLATS, BLACK_FLATS, DRAW = range(6)
RESULTS = {"R-0": WHITE_ROAD, "0-R": BLACK_ROAD, "F-0": WHITE_FLATS, "0-F": BLACK_FLATS,
           "1/2-1/2": DRAW}
ENDINGS = ("rules", "repetition", "ply limit")
DIRECTIONS = "+>-<"


@dataclass
class Game:
    size: int
    halfkomi: int
    outcome: int
    ending: int
    random_plies: int
    generation: int
    nodes: int
    moves: list[int]
    scores: list[int]


def read_games(path: Path) -> Iterator[Game]:
    data = path.read_bytes()
    offset = 0
    while offset + HEADER.size <= len(data):
        magic, size, halfkomi, outcome, ending, plies, random_plies, generation, nodes = \
            HEADER.unpack_from(data, offset)
        if magic != MAGIC:
            raise ValueError(f"{path}: bad magic at byte {offset}")
        offset += HEADER.size
        if offset + plies * 4 > len(data):
            return  # a run killed part way through a game
        words = struct.unpack_from(f"<{plies * 2}H", data, offset)
        offset += plies * 4
        moves = list(words[0::2])
        scores = [s - 65536 if s >= 32768 else s for s in words[1::2]]
        yield Game(size, halfkomi, outcome, ending, random_plies, generation, nodes, moves, scores)


def move_ptn(move: int) -> str:
    """The encoding in src/types.h, written out independently of src/notation.c."""
    square = move & 63
    kind = (move >> 6) & 3
    pattern = move >> 8
    name = "abcdefgh"[square & 7] + str((square >> 3) + 1)
    if pattern == 0:
        return {1: "", 2: "S", 3: "C"}[kind] + name
    drops = []
    run = 0
    for bit in range(8):
        run += 1
        if (pattern >> bit) & 1:
            drops.append(run)
            run = 0
    carry = sum(drops)
    return (str(carry) if carry > 1 else "") + name + DIRECTIONS[kind] + \
        ("".join(map(str, drops)) if len(drops) > 1 else "")


def check_game(game: Game) -> str | None:
    board = takref.Game(game.size, game.halfkomi)
    for ply, move in enumerate(game.moves):
        ptn = move_ptn(move)
        if ptn not in board.legal_moves():
            return f"ply {ply}: {ptn} is illegal"
        if board.result() is not None:
            return f"ply {ply}: the game was already over"
        board.play(ptn)
    result = board.result()
    if game.ending == 0:
        if result is None or RESULTS[result] != game.outcome:
            return f"ended {result}, header says {game.outcome}"
    elif game.outcome != DRAW or result is not None:
        return f"cut off with result {result}, header says {game.outcome}"
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("files", nargs="+", type=Path)
    parser.add_argument("--check-every", type=int, default=200,
                        help="replay one game in this many through takref")
    parser.add_argument("--same-as", type=Path, help="fail unless the one file matches this one")
    args = parser.parse_args()

    if args.same_as and args.files[0].read_bytes() != args.same_as.read_bytes():
        print(f"{args.files[0]} and {args.same_as} differ: datagen is not deterministic")
        return 1

    outcomes: Counter[int] = Counter()
    endings: Counter[int] = Counter()
    komi: Counter[int] = Counter()
    plies: list[int] = []
    random_plies: list[int] = []
    deviations = 0
    white_score = 0.0
    bytes_total = 0
    games = checked = 0
    failures = 0

    for path in args.files:
        bytes_total += path.stat().st_size
        for game in read_games(path):
            outcomes[game.outcome] += 1
            endings[game.ending] += 1
            komi[game.halfkomi] += 1
            plies.append(len(game.moves))
            random_plies.append(game.random_plies)
            deviations += sum(s == NO_SCORE for s in game.scores[game.random_plies:])
            white_score += {WHITE_ROAD: 1.0, WHITE_FLATS: 1.0, DRAW: 0.5}.get(game.outcome, 0.0)
            if games % args.check_every == 0:
                checked += 1
                problem = check_game(game)
                if problem:
                    failures += 1
                    print(f"{path.name} game {games}: {problem}")
            games += 1

    if not games:
        print("no games")
        return 1
    names = {WHITE_ROAD: "white road", BLACK_ROAD: "black road", WHITE_FLATS: "white flats",
             BLACK_FLATS: "black flats", DRAW: "draw"}
    print(f"{games} games, {sum(plies)} positions, {bytes_total / sum(plies):.2f} bytes a position")
    print(f"plies: median {statistics.median(plies)}, longest {max(plies)}, "
          f"random {min(random_plies)}-{max(random_plies)}")
    searched = sum(plies) - sum(random_plies)
    print(f"deviations: {deviations}, one in {searched / deviations:.0f} searched plies"
          if deviations else "deviations: none")
    print("outcomes: " + ", ".join(f"{names[k]} {100 * v / games:.1f}%"
                                   for k, v in sorted(outcomes.items())))
    print("endings: " + ", ".join(f"{ENDINGS[k]} {100 * v / games:.1f}%"
                                  for k, v in sorted(endings.items())))
    print("half komi: " + ", ".join(f"{k}: {100 * v / games:.1f}%" for k, v in sorted(komi.items())))
    print(f"white scored {100 * white_score / games:.1f}%")
    print(f"takref replayed {checked} games: {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
