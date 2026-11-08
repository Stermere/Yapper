"""
playtak.py - people's games from PlayTak's history, as PTN, for datagen's seeds.

    python tools/playtak.py fetch external/data/human/playtak
    python tools/playtak.py ptn external/data/human/playtak --size 6 \
        --out external/data/human/playtak-6s.ptn

`fetch` saves the server's whole history a page at a time and picks up where it stopped. `ptn`
keeps finished games between two people at the size's usual reserves, turns the server's notation
into PTN, and replays a sample through tools/takref.py. A game with an engine on either side is
dropped: another engine's play never goes into Yapper's data, docs/DATA.md.

Stdlib only.
"""

from __future__ import annotations

import argparse
import json
import random
import re
import sys
import time
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import takref

API = "https://api.playtak.com/v1/games-history"
PER_PAGE = 5000

# Accounts that are programs. PlayTak's bots carry "bot" in the name; the rest are engines that
# have played there under their own names.
ENGINES = re.compile(r"bot|tiltak|topaz|taktician|syntaks|pentakle|alphatak|takkerus|cutak|"
                     r"yapper|tak_?ai|\bai\b", re.IGNORECASE)

PLACE = re.compile(r"P ([A-H][1-8])(?: ([WC]))?$")
SPREAD = re.compile(r"M ([A-H][1-8]) ([A-H][1-8])((?: \d)+)$")


def fetch(folder: Path) -> int:
    folder.mkdir(parents=True, exist_ok=True)
    page, pages = 1, None
    while pages is None or page <= pages:
        path = folder / f"page-{page:04d}.json"
        if path.exists() and pages is not None:
            page += 1
            continue
        url = f"{API}?limit={PER_PAGE}&page={page}"
        request = urllib.request.Request(url, headers={"User-Agent": "yapper-datagen"})
        with urllib.request.urlopen(request, timeout=120) as response:
            raw = response.read()
        listing = json.loads(raw)
        pages = listing["totalPages"]
        if not path.exists():
            path.write_bytes(raw)
            print(f"page {page} of {pages}: {len(listing['items'])} games", flush=True)
            time.sleep(1.0)
        page += 1
    return 0


def square(name: str) -> str:
    return name.lower()


def move_ptn(token: str) -> str | None:
    """The server's `P A1 W` or `M A1 A3 1 2` as PTN, or None if it is neither."""
    token = token.strip()
    place = PLACE.match(token)
    if place:
        return {"W": "S", "C": "C", None: ""}[place.group(2)] + square(place.group(1))
    spread = SPREAD.match(token)
    if not spread:
        return None
    start, end = spread.group(1), spread.group(2)
    drops = [int(d) for d in spread.group(3).split()]
    files, ranks = ord(end[0]) - ord(start[0]), int(end[1]) - int(start[1])
    if (files == 0) == (ranks == 0) or max(abs(files), abs(ranks)) != len(drops):
        return None
    arrow = ">" if files > 0 else "<" if files < 0 else "+" if ranks > 0 else "-"
    carry = sum(drops)
    return (str(carry) if carry > 1 else "") + square(start) + arrow + \
        ("".join(map(str, drops)) if len(drops) > 1 else "")


def replays(size: int, moves: list[str]) -> bool:
    game = takref.Game(size)
    for move in moves:
        if game.result() is not None or takref.canonical(move) not in game.legal_moves():
            return False
        game.play(move)
    return True


def to_ptn(folder: Path, size: int, out: Path, min_plies: int, min_rating: int,
           check: int, seed: int) -> int:
    seen: set[int] = set()
    kept: list[tuple[dict, list[str]]] = []
    dropped = {"size or reserves": 0, "engine": 0, "unfinished": 0, "short": 0, "rating": 0,
               "notation": 0}
    stones, capstones = takref.PIECES[size]
    for path in sorted(folder.glob("page-*.json")):
        for item in json.loads(path.read_bytes())["items"]:
            if item["id"] in seen:
                continue
            seen.add(item["id"])
            # -1 is the server's default, from before reserves could be chosen.
            if item["size"] != size or item["pieces"] not in (-1, stones) or \
                    item["capstones"] not in (-1, capstones) or item.get("opening") != "swap":
                dropped["size or reserves"] += item["size"] == size
                continue
            if ENGINES.search(item["player_white"]) or ENGINES.search(item["player_black"]):
                dropped["engine"] += 1
                continue
            if item["result"] in ("0-0", "") or not item["notation"]:
                dropped["unfinished"] += 1
                continue
            if min(item["rating_white"], item["rating_black"]) < min_rating:
                dropped["rating"] += 1
                continue
            moves = [move_ptn(t) for t in item["notation"].split(",")]
            if any(m is None for m in moves):
                dropped["notation"] += 1
                continue
            if len(moves) < min_plies:
                dropped["short"] += 1
                continue
            kept.append((item, moves))

    sample = random.Random(seed).sample(kept, min(check, len(kept)))
    failures = sum(not replays(size, moves) for _, moves in sample)

    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", newline="\n") as f:
        for item, moves in kept:
            date = datetime.fromtimestamp(item["date"] / 1000, timezone.utc).strftime("%Y.%m.%d")
            # Only tags PTN Ninja knows: CLAUDE.md invariant 10.
            f.write(f'[Size "{size}"]\n[Komi "{item["komi"] / 2:g}"]\n'
                    f'[Player1 "{item["player_white"]}"]\n[Player2 "{item["player_black"]}"]\n'
                    f'[Date "{date}"]\n[Result "{item["result"]}"]\n\n')
            pairs = [" ".join(moves[i:i + 2]) for i in range(0, len(moves), 2)]
            f.write("\n".join(f"{n + 1}. {p}" for n, p in enumerate(pairs)))
            f.write(f" {item['result']}\n\n")
    plies = sum(len(m) for _, m in kept)
    print(f"{len(seen)} games read, {len(kept)} kept, {plies} plies, {out}")
    print("dropped: " + ", ".join(f"{k} {v}" for k, v in dropped.items()))
    print(f"takref replayed {len(sample)} games: {failures} failures")
    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    get = sub.add_parser("fetch", help="save the history's pages into a folder")
    get.add_argument("folder")
    conv = sub.add_parser("ptn", help="write one size's games between people as PTN")
    conv.add_argument("folder")
    conv.add_argument("--size", type=int, default=6)
    conv.add_argument("--out", required=True)
    conv.add_argument("--min-plies", type=int, default=12)
    conv.add_argument("--min-rating", type=int, default=0, help="for both players")
    conv.add_argument("--check", type=int, default=1000, help="games replayed through takref")
    conv.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()
    if args.command == "fetch":
        return fetch(Path(args.folder))
    return to_ptn(Path(args.folder), args.size, Path(args.out), args.min_plies, args.min_rating,
                  args.check, args.seed)


if __name__ == "__main__":
    sys.exit(main())
