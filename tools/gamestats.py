"""
gamestats.py - how a match went, side by side, from the PTN alone.

    python tools/gamestats.py external/games/vs-tiltak.ptn --them tiltak

Replays each game through tools/takref.py and compares the two sides by how they lost or won,
what they do with their stones, and what stands on the board at fixed plies. These are symptoms
to read, not targets: only an SPRT says a change is good.

Scores in PTN comments read from white's side, as racetrack writes them. racetrack drops mate
scores, so a search that proves the result leaves the score of an iteration before the proof,
shallow and in no measurable time. Those are skipped.

Stdlib only.
"""

from __future__ import annotations

import argparse
import re
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import takref

RESULTS = ("R-0", "0-R", "F-0", "0-F", "1/2-1/2", "1-0", "0-1")
SCORE_RE = re.compile(r"\{([+-]?\d+\.\d+)/(\d+)(?:\s+(\d+\.\d+)s)?")
SNAPSHOTS = (16, 30, 44, 60, 80)
COLUMNS = ("flats", "walls", "caps", "tall", "friends", "enemies", "biggest", "reserve")


def split_games(text: str) -> list[str]:
    """Each game starts at the first tag after the moves of the one before."""
    return [g for g in re.split(r"\n\s*\n(?=\[)", text) if "[" in g and "]" in g]


def parse_moves(body: str) -> list[tuple[str, str | None]]:
    moves: list[tuple[str, str | None]] = []
    for token in re.findall(r"\{[^}]*\}|[^\s{}]+", body):
        if token.startswith("{"):
            if moves:
                moves[-1] = (moves[-1][0], token)
        elif not re.fullmatch(r"\d+\.", token) and token not in RESULTS:
            moves.append((token, None))
    return moves


def snapshot(game: takref.Game, color: int) -> dict[str, float]:
    row = dict.fromkeys(COLUMNS, 0.0)
    for rank in game.board:
        for stack in rank:
            if not stack or stack[-1].color != color:
                continue
            row[{"F": "flats", "S": "walls", "C": "caps"}[stack[-1].kind]] += 1
            row["biggest"] = max(row["biggest"], len(stack))
            if len(stack) > 1:
                row["tall"] += 1
                row["friends"] += sum(s.color == color for s in stack[:-1])
                row["enemies"] += sum(s.color != color for s in stack[:-1])
    row["reserve"] = game.stones[color]
    return row


def median(values: list[float]) -> str:
    return f"{statistics.median(values):.1f}" if values else "-"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("ptn")
    parser.add_argument("--them", required=True, help="part of the opponent's player name")
    args = parser.parse_args()

    tally = {"won": 0, "lost": 0, "drawn": 0}
    endings: dict[str, int] = {}
    road_finish = {"place": 0, "spread": 0}
    margins: list[int] = []
    plies: dict[str, list[int]] = {"won": [], "lost": [], "drawn": []}
    first_cap: dict[str, list[int]] = {"us": [], "them": []}
    walls = {"us": 0, "them": 0}
    spreads = {"us": 0, "them": 0}
    tables: dict[int, dict[str, list[dict[str, float]]]] = {
        p: {"us": [], "them": []} for p in SNAPSHOTS}
    ahead_in_loss = 0

    games = split_games(Path(args.ptn).read_text(encoding="utf-8"))
    for text in games:
        tags = dict(re.findall(r'\[(\w+) "([^"]*)"\]', text))
        theirs = [args.them.lower() in tags[tag].lower() for tag in ("Player1", "Player2")]
        if sum(theirs) != 1:
            # tools/match.py names both sides of a self-play match Yapper.
            print(f"cannot tell the sides apart: {tags['Player1']} against {tags['Player2']}",
                  file=sys.stderr)
            return 2
        our_color = takref.BLACK if theirs[0] else takref.WHITE
        result = tags["Result"]
        halfkomi = round(float(tags.get("Komi", "0")) * 2)
        size = int(tags["Size"])
        winner = {"R-0": 0, "F-0": 0, "1-0": 0, "0-R": 1, "0-F": 1, "0-1": 1}.get(result)
        verdict = "drawn" if winner is None else "won" if winner == our_color else "lost"
        tally[verdict] += 1
        endings[result] = endings.get(result, 0) + 1

        game = takref.Game(size, halfkomi)
        our_scores: list[float] = []
        caps_seen: set[str] = set()
        moves = parse_moves(text[text.rindex("]") + 1:])
        for move, comment in moves:
            who = "us" if game.ply % 2 == our_color else "them"
            if re.search(r"[+\-<>]", move):
                spreads[who] += 1
            elif move[0] == "S":
                walls[who] += 1
            elif move[0] == "C" and who not in caps_seen:
                caps_seen.add(who)
                first_cap[who].append(game.ply)
            match = SCORE_RE.match(comment or "")
            proven = match and (int(match.group(2)) < 3 or match.group(3) == "0.00")
            if match and who == "us" and not proven:
                value = float(match.group(1))
                our_scores.append(value if our_color == takref.WHITE else -value)
            game.play(move)
            if game.ply in tables:
                tables[game.ply]["us"].append(snapshot(game, our_color))
                tables[game.ply]["them"].append(snapshot(game, 1 - our_color))

        plies[verdict].append(game.ply)
        last = moves[-1][0] if moves else ""
        if verdict == "lost" and result in ("R-0", "0-R"):
            road_finish["spread" if re.search(r"[+\-<>]", last) else "place"] += 1
        if verdict != "drawn" and result in ("F-0", "0-F"):
            white, black = game.flat_counts()
            lead = 2 * (white - black) - halfkomi
            ours = lead if our_color == takref.WHITE else -lead
            margins.append(ours)
        late = our_scores[-max(1, len(our_scores) // 3):]
        if verdict == "lost" and late and max(late) > 1.0:
            ahead_in_loss += 1

    print(f"{len(games)} games: +{tally['won']} -{tally['lost']} ={tally['drawn']}, {endings}")
    print(f"median plies: won {median(plies['won'])}, lost {median(plies['lost'])}")
    print(f"road losses finished by: {road_finish}")
    lost = [-m for m in margins if m < 0]
    won = [m for m in margins if m > 0]
    print(f"flat games, margin in half flats with komi: lost {len(lost)}, median {median(lost)}; "
          f"won {len(won)}, median {median(won)}")
    print(f"above +1 in the last third of a lost game: {ahead_in_loss}/{tally['lost']}")
    for who in ("us", "them"):
        print(f"{who:>4}: first capstone at median ply {median(first_cap[who])} "
              f"in {len(first_cap[who])} games, {walls[who] / len(games):.1f} walls and "
              f"{spreads[who] / len(games):.1f} spreads a game")
    print()
    print("ply  who     n  " + " ".join(f"{c:>7}" for c in COLUMNS))
    for ply in SNAPSHOTS:
        for who in ("us", "them"):
            rows = tables[ply][who]
            if rows:
                cells = " ".join(f"{statistics.mean(r[c] for r in rows):7.1f}" for c in COLUMNS)
                print(f"{ply:3d}  {who:<4} {len(rows):4d}  {cells}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
