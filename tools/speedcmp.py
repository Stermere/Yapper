"""
speedcmp.py - race move generation against the baseline binaries in external/engines.

    make speed
    python tools/speedcmp.py --runs 1

Same machine, same positions, one program at a time, best of --runs. Run nothing else
alongside it. A baseline that is not there is skipped, so this works on a fresh clone,
though it then has nothing to race.

The baselines are black boxes on purpose: see docs/STATUS.md. Node counts are printed because
they are a check of their own, and a baseline that keeps going past a finished game will
report more of them.

syntaks and pentakle have one perft each, and neither makes the last ply: proved for syntaks and
inferred for pentakle, both from outside. They only race on bulk.

Stdlib only.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

POSITIONS = [
    ("6x6 start", None, 5),
    ("6x6 midgame",
     "x4,2,1/2,x2,21S,12,1/2,1,1,2,1,2/1221C,1,2,112C,1,1/2,1,2,1,2,1/2,x,2,2,1,x 1 22", 4),
    ("6x6 late",
     "x,1,x4/2S,1,x,2,x2/2,121C,212S,x,2C,x/1,211121S,1S,112,11112,x/2221,x,2,1,2,1S/"
     "2222221,x,2,2,2,2 2 43", 4),
    ("6x6 endgame",
     "2,2,21S,2,2,2/2,x,222221,2,2,x/1,1,2221C,x,111112C,2S/x,1,2S,x2,121211212/"
     "1,1,1212S,1S,2,1S/x2,2,1,21,1 1 42", 4),
]

LINE = re.compile(r"(full|bulk) depth (\d+): (\d+)\s+([\d.]+)s\s+([\d.]+) Mnps")
TOTAL = re.compile(r"total: (\d+)")
RATE = re.compile(r"([\d.]+) (M?)nps")


def timed(command: list[str], depth: int, runs: int) -> dict[str, tuple[int, float]]:
    """The best result of each kind, as {kind: (nodes, Mnps)}."""
    best: dict[str, tuple[int, float]] = {}
    for _ in range(runs):
        out = subprocess.run(command, capture_output=True, text=True).stdout
        for kind, d, nodes, _seconds, mnps in LINE.findall(out):
            if int(d) == depth and (kind not in best or float(mnps) > best[kind][1]):
                best[kind] = (int(nodes), float(mnps))
    return best


def piped(binary: Path, commands: str, runs: int) -> dict[str, tuple[int, float]]:
    """The same for a binary driven over stdin, which prints a total and then a rate."""
    best: dict[str, tuple[int, float]] = {}
    for _ in range(runs):
        out = subprocess.run([str(binary)], input=commands, capture_output=True, text=True).stdout
        total, rate = TOTAL.search(out), RATE.search(out)
        if total and rate:
            mnps = float(rate.group(1)) / (1 if rate.group(2) else 1e6)
            if "bulk" not in best or mnps > best["bulk"][1]:
                best["bulk"] = (int(total.group(1)), mnps)
    return best


def main() -> int:
    default_engine = "./yapper.exe" if os.name == "nt" else "./yapper"
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--engine", default=default_engine)
    parser.add_argument("--baselines", default="external/engines")
    parser.add_argument("--runs", type=int, default=3)
    args = parser.parse_args()

    base = Path(args.baselines)
    suffix = ".exe" if os.name == "nt" else ""
    tiltak = base / f"tiltak-perft{suffix}"
    topaz = base / f"topaz-perft{suffix}"
    syntaks = base / f"syntaks{suffix}"
    pentakle = base / f"pentakle{suffix}"

    for name, tps, depth in POSITIONS:
        print(f"== {name}, depth {depth}", flush=True)
        extra = [tps] if tps else []
        rows = {"yapper": timed([args.engine, "perft", "6", str(depth)] +
                                (["tps", tps] if tps else []), depth, args.runs)}
        if tiltak.exists():
            rows["tiltak"] = timed([str(tiltak), "6", str(depth)] + extra, depth, args.runs)
        if topaz.exists():
            rows["topaz"] = timed([str(topaz), str(depth)] + extra, depth, args.runs)
        position = f"position tps {tps}\n" if tps else "position startpos\n"
        if syntaks.exists():
            rows["syntaks"] = piped(syntaks, f"tei\nisready\nteinewgame 6\n{position}"
                                    f"splitperft {depth}\nisready\nquit\n", args.runs)
        if pentakle.exists():
            rows["pentakle"] = piped(pentakle, f"{position}perft {depth}\n", args.runs)

        for kind in ("full", "bulk"):
            rivals = [row[kind][1] for engine, row in rows.items()
                      if engine != "yapper" and kind in row]
            for engine, row in rows.items():
                if kind in row:
                    nodes, mnps = row[kind]
                    lead = ""
                    if engine == "yapper" and rivals:
                        lead = f"  {mnps / max(rivals):.1f}x the best baseline"
                    print(f"  {engine:8} {kind} {nodes:>14,} nodes {mnps:9.1f} Mnps{lead}",
                          flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
