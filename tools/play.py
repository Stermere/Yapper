"""
play.py - play against the engine in a terminal.

    python tools/play.py                        # 5x5, you are White, one second a move
    python tools/play.py --size 6 --color black
    python tools/play.py --depth 2              # an easier opponent

The referee is tools/takref.py, so an illegal move is refused rather than played. A finished
game is saved to external/games/played.ptn with a link to replay it on ptn.ninja.

Stdlib only.
"""

from __future__ import annotations

import argparse
import os
import random
import sys
import urllib.parse
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import match
import ptnlink
import takref

HELP = """
  Placing, from your reserve:
    a1          a flat stone on a1. Flats count for roads and for the final score
    Sa1         a wall on a1. Walls block, but are not part of a road
    Ca1         your capstone on a1. It counts for roads and can flatten a wall

  Moving a stack you control (your stone on top):
    a1>         one stone east. The arrows are  + north  > east  - south  < west
    3a1>        three stones east, all dropped on the next square
    3a1>12      three stones east: drop 1 on b1, then 2 on c1
    You can carry up to the board's width, and must drop at least one on every square.

  Your first move places a flat of your OPPONENT's colour, and so does theirs.

  Win by connecting two opposite edges with a road of flats and capstones. If the board
  fills or a player runs out of stones, whoever has the most flats on top wins.

  Commands: moves (list legal moves), hint, undo, resign, quit, help
"""

RESULTS = {
    "R-0": "White wins by a road.",
    "0-R": "Black wins by a road.",
    "F-0": "White wins on flats.",
    "0-F": "Black wins on flats.",
    "1/2-1/2": "A draw.",
    "1-0": "White wins by resignation.",
    "0-1": "Black wins by resignation.",
}


def cell_text(stack: list[takref.Stone], width: int) -> str:
    """Bottom to top: w and b for each stone, then S or C for a wall or capstone."""
    text = "".join("wb"[stone.color] for stone in stack)
    if stack and stack[-1].kind != takref.FLAT:
        text += stack[-1].kind
    return text if len(text) <= width else ".." + text[2 - width:]


def show(game: takref.Game) -> None:
    width = 7
    files = "abcdefgh"[: game.size]
    edge = "   +" + (("-" * width + "+") * game.size)
    print()
    print("    " + "".join(f"{f:^{width + 1}}" for f in files))
    print(edge)
    for rank in range(game.size - 1, -1, -1):
        row = "|".join(f"{cell_text(s, width):^{width}}" for s in game.board[rank])
        print(f" {rank + 1} |{row}| {rank + 1}")
        print(edge)
    white, black = game.flat_counts()
    print(f"    Flats on top  White {white}  Black {black}", end="")
    print(f"  (Black gets {game.halfkomi / 2:g} komi)" if game.halfkomi else "")
    print(f"    Reserves      White {game.stones[0]}+{game.capstones[0]}C"
          f"  Black {game.stones[1]}+{game.capstones[1]}C")


def engine_move(engine: match.Engine, played: list[str], limit: str, seconds: float) -> str:
    position = "position startpos" + (" moves " + " ".join(played) if played else "")
    engine.send(position)
    engine.send(limit)
    while True:
        line = engine.read(seconds)
        if line.startswith("bestmove"):
            return line.split()[1]


def main() -> int:
    default_engine = "./yapper.exe" if os.name == "nt" else "./yapper"
    parser = argparse.ArgumentParser(description="Play against the engine in a terminal.")
    parser.add_argument("--engine", default=default_engine)
    parser.add_argument("--size", type=int, default=5, choices=range(3, 9))
    parser.add_argument("--halfkomi", type=int, default=0, help="komi in half points, for Black")
    parser.add_argument("--color", choices=["white", "black", "random"], default="white")
    parser.add_argument("--movetime", type=int, default=1000, help="engine's milliseconds a move")
    parser.add_argument("--depth", type=int, default=0, help="a fixed depth, overriding movetime")
    args = parser.parse_args()

    engine = match.Engine([args.engine], {"HalfKomi": str(args.halfkomi)})
    engine.send(f"teinewgame {args.size}")
    engine.wait_ready()

    limit = f"go depth {args.depth}" if args.depth else f"go movetime {args.movetime}"
    wait = 600.0 if args.depth else args.movetime / 1000 + 10.0
    color = args.color if args.color != "random" else random.choice(["white", "black"])
    human = takref.WHITE if color == "white" else takref.BLACK

    game = takref.Game(args.size, args.halfkomi)
    played: list[str] = []
    result = None
    print(f"You are {color.capitalize()}. Type help for how to move.")

    while result is None:
        result = game.result()
        if result is not None:
            break
        show(game)

        if game.to_move != human:
            move = engine_move(engine, played, limit, wait)
            print(f"\n{engine.name} plays {move}")
            game.play(move)
            played.append(takref.canonical(move))
            continue

        turn = f"{'White' if human == takref.WHITE else 'Black'}, move {game.ply // 2 + 1}"
        if game.ply < 2:
            turn += " (place your opponent's stone)"
        try:
            text = input(f"\n{turn}> ").strip()
        except EOFError:
            text = "quit"

        if text == "quit":
            engine.close()
            return 0
        if text in ("help", "?"):
            print(HELP)
        elif text == "moves":
            print(" ".join(game.legal_moves()))
        elif text == "hint":
            print(f"The engine would play {engine_move(engine, played, limit, wait)}")
        elif text == "undo":
            # Back to your own turn: your last move and the reply to it.
            for _ in range(min(2, len(played))):
                played.pop()
            game = takref.Game(args.size, args.halfkomi)
            for move in played:
                game.play(move)
        elif text == "resign":
            result = "0-1" if human == takref.WHITE else "1-0"
        elif text:
            try:
                move = takref.canonical(text)
            except ValueError:
                print("Not a move. Type help for how to write one.")
                continue
            if move not in game.legal_moves():
                print(f"{move} is not legal here. Type moves to see what is.")
                continue
            game.play(move)
            played.append(move)

    show(game)
    print(f"\n{RESULTS[result]}")
    engine.close()

    if played:
        path = Path("external/games/played.ptn")
        path.parent.mkdir(parents=True, exist_ok=True)
        white, black = ("You", engine.name) if human == takref.WHITE else (engine.name, "You")
        finished = match.GameResult(result, "", played, [""] * len(played))
        match.write_ptn(path, finished, white, black, args.size, args.halfkomi,
                        limit.removeprefix("go "), 1)
        last = ptnlink.split_games(path.read_text(encoding="utf-8"))[-1]
        print(f"Saved to {path}. Replay it:")
        print("https://ptn.ninja/" + urllib.parse.quote(last, safe=""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
