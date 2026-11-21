"""
progress.py - a live view of every long run, matches, fits and datagen: where each stands, and
when the queue should end.

    python tools/progress.py                  # redrawn in the terminal every 5 seconds
    python tools/progress.py --serve 8765     # the same as a page, http://localhost:8765
    python tools/progress.py --once           # print it once

What it reads, all under external/games/:

- live/*.json, which tools/match.py writes after every pair, however it was started. Whether its
  process still runs tells a match that is playing from one that was killed. tools/tune.py writes
  one too, with the parameters that have moved furthest; one that stopped short of its end is
  paused, since --resume carries it on, and stays listed however long it waits. `yapper nntrain`
  writes one for every fit run from the repository's root, and tools/datagen.py one a run, which
  is paused in the same way when it stopped short.
- queue.txt, if there is one: the runs planned, in the order they play, so it can say when each
  should start and end. One run a line, then a label:

      # Review of E52, 2026-11-06
      games   600    external/games/tak/e52-k4.log   E52 vs syntaks at 1/4
      sprt    2000   external/games/pol2/r1.log      r1 flat win on the spot, vs E52
      fit     16     external/build/e120/s0          6x6 alone, seed 0
      datagen 27000  external/data/gen13/6s/gen13    gen 13 self-play

  `sprt` may stop before its cap and `games` plays them all. The path names the run: the match.py
  run whose PTN has that directory and name, or else a log, racetrack's or match.py's printout; a
  fit's folder, with the epochs; datagen's --out, with the games. A fit not yet started is
  expected to take as long as the fits before it. The first comment is the title.
- Any other log written in the last ten minutes, such as racetrack's, which keeps no status of
  its own. How long such a run will be is unknown.

A run is stalled when it has been quiet for ten games' time and at least three minutes, as when
racetrack hangs between two idle engines. An SPRT is expected to end where its LLR is heading at
the rate it plays; one not yet started gets the median length of the finished SPRTs beside it, and
the rate of the last run of its kind and length. "At the latest" plays every SPRT to its cap.
Finished runs outside the queue stay listed for a day.

Stdlib only.
"""

from __future__ import annotations

import argparse
import ctypes
import datetime
import http.server
import json
import math
import os
import re
import shutil
import statistics
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAMES = ROOT / "external" / "games"
LIVE = GAMES / "live"
QUEUE = GAMES / "queue.txt"

MATCH_RE = re.compile(
    r"^\s*(\d+) games\s+\+(\d+) -(\d+) =(\d+)\s+Elo\s+([+-]?[\d.]+) \+/-\s+([\d.]+)\s+"
    r"pairs \[([\d, ]+)\](?:\s+LLR ([+-]?[\d.]+) \[([+-]?[\d.]+), ([+-]?[\d.]+)\])?",
    re.M,
)
VERDICT_RE = re.compile(r"^SPRT \[[^\]]*\]: (\w+)", re.M)
FINISHED_RE = re.compile(r"^\d+s, \d+ concurrent", re.M)
RACE_RE = re.compile(r": \+(\d+)-(\d+)=(\d+), ([+-]?\d+) elo \[([+-]?\d+), ([+-]?\d+)\]")
START_RE = re.compile(r"\Astart \S+ (\d\d):(\d\d):(\d\d)")

STALL_GAMES = 10
STALL_FLOOR = 180.0
EARLY_GAMES = 200  # before this, an SPRT's LLR says little about where it is heading
DISCOVER = 600.0
RECENT = 86400.0
SPRT_CAP = 2000  # match.py's default under SPRT, for a printout found without a queue line
FIT_QUIET = 600.0  # a fit scoring millions of held-out positions writes nothing for a while
FIT_TAIL = 30.0  # the least a fit takes after its last step: scoring, writing, checking
ENDED = ("done", "stopped", "paused")
LIVELY = ("running", "stalled")
OWN_STATUS = ("fit", "datagen")  # kinds known only from a status file, never from a log

_logs: dict[Path, tuple[tuple[int, int], dict]] = {}
_statuses: dict[Path, tuple[tuple[int, int], dict]] = {}


def folder(path: Path | str) -> str:
    return os.path.normcase(str(Path(path).resolve()))


def started_at(text: str, born: float, touched: float) -> float:
    """sprts.sh opens each log with the clock time, which outlives a file reused by name."""
    m = START_RE.match(text)
    if not m:
        return born
    t = datetime.datetime.fromtimestamp(touched).replace(
        hour=int(m[1]), minute=int(m[2]), second=int(m[3]), microsecond=0
    )
    if t.timestamp() > touched + 1:
        t -= datetime.timedelta(days=1)
    return t.timestamp()


def read_log(path: Path) -> dict | None:
    try:
        st = path.stat()
    except OSError:
        return None
    key = (st.st_mtime_ns, st.st_size)
    hit = _logs.get(path)
    if hit and hit[0] == key:
        return hit[1]
    text = path.read_text(encoding="utf-8", errors="replace")
    born = getattr(st, "st_birthtime", st.st_ctime)
    info: dict = {"source": "log", "start": started_at(text, born, st.st_mtime),
                  "touched": st.st_mtime, "games": 0}
    for m in MATCH_RE.finditer(text):
        games, w, l, d, elo, err, pairs, llr, lower, upper = m.groups()
        info.update(games=int(games), w=int(w), l=int(l), d=int(d), elo=float(elo),
                    err=float(err), pairs=[int(x) for x in pairs.split(",")])
        if llr:
            info.update(llr=float(llr), lower=float(lower), upper=float(upper))
    races = RACE_RE.findall(text)
    if races:
        w, l, d, elo, lo, hi = (int(x) for x in races[-1])
        info.update(games=w + l + d, w=w, l=l, d=d, elo=float(elo), lo=lo, hi=hi)
    verdict = VERDICT_RE.search(text)
    info["verdict"] = verdict[1] if verdict else None
    info["finished"] = bool(verdict or FINISHED_RE.search(text))
    _logs[path] = (key, info)
    return info


def moved(spsa: list, count: int = 6) -> str:
    """A tools/tune.py run's parameters that have travelled furthest for their range."""
    far = sorted(spsa, key=lambda p: -abs(p[2] - p[1]) / max(1, p[4] - p[3]))[:count]
    return "  ".join(f"{name} {start:g}>{now:.1f}" for name, start, now, _, _ in far)


def short(path: str) -> str:
    """A fit or a datagen run by the last two parts of its path, as a label."""
    parts = Path(path).parts
    return "/".join(parts[-2:])


def read_job(s: dict) -> dict:
    """A fit's status, from `yapper nntrain`, or a datagen run's, from tools/datagen.py. Their
    paths are the fit's output folder and the --out prefix, by which queue.txt names them."""
    out = ROOT / s["out"]
    code = s.get("code")
    run = {"source": "status", "kind": s["kind"], "pid": s.get("pid"), "ptn": str(out),
           "start": s["started"], "touched": s["updated"], "finished": bool(s.get("finished")),
           "code": code, "verdict": "FAILED" if code else None, "w": None}
    if s["kind"] == "fit":
        run.update(label=f"fit {short(s['out'])}", cap=None, games=0, phase=s.get("phase", ""),
                   step=s.get("step", 0), steps=s.get("steps", 0), epoch=s.get("epoch", 0),
                   epochs=s.get("epochs", 0), files=s.get("files", [0, 0]),
                   positions=s.get("positions", 0), losses=s.get("losses", []),
                   averaged=s.get("averaged"), device=s.get("device", ""),
                   read_at=s.get("read_at") or 0.0, train_at=s.get("train_at") or 0.0)
    else:
        run.update(label=s.get("label") or f"datagen {short(s['out'])}", cap=s["cap"],
                   games=s["games"], positions=s.get("positions", 0), nodes=s.get("nodes"),
                   resumable=bool(s.get("resumable")))
    return run


def read_status(path: Path) -> dict | None:
    hit = _statuses.get(path)
    try:
        st = path.stat()
        key = (st.st_mtime_ns, st.st_size)
        if hit and hit[0] == key:
            return hit[1]
        s = json.loads(path.read_text(encoding="utf-8"))
        if s.get("kind") in OWN_STATUS:
            run = read_job(s)
            run["dir"] = folder(Path(run["ptn"]).parent)
            _statuses[path] = (key, run)
            return run
        run = {"source": "status", "label": s.get("label") or path.stem, "pid": s.get("pid"),
               "kind": "sprt" if s.get("sprt") else "games", "cap": s.get("cap"),
               "tc": s.get("tc"), "ptn": s.get("ptn"), "start": s["started"],
               "touched": s["updated"], "games": s["games"], "w": s["w"], "l": s["l"],
               "d": s["d"], "pairs": s["pairs"], "elo": s["elo"], "err": s["err"],
               "verdict": s.get("verdict"), "finished": bool(s.get("finished"))}
    except (OSError, ValueError, KeyError, TypeError):
        return hit[1] if hit else None  # caught mid-write: the last good read stands
    if s.get("llr") is not None:
        run.update(llr=s["llr"], lower=s["lower"], upper=s["upper"])
    if s.get("spsa"):
        run["moved"] = moved(s["spsa"])
        run["spsa"] = s["spsa"]
        run["resumable"] = True
    run["dir"] = folder(Path(run["ptn"]).parent) if run["ptn"] else ""
    _statuses[path] = (key, run)
    return run


def alive(pid: int | None) -> bool:
    if not pid:
        return False
    if os.name != "nt":
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return False
        except PermissionError:
            return True
        return True
    # Never os.kill on Windows: there it terminates the process whatever the signal.
    kernel = ctypes.WinDLL("kernel32")
    kernel.OpenProcess.restype = ctypes.c_void_p
    kernel.GetExitCodeProcess.argtypes = (ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong))
    kernel.CloseHandle.argtypes = (ctypes.c_void_p,)
    handle = kernel.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not handle:
        return False
    code = ctypes.c_ulong()
    ok = kernel.GetExitCodeProcess(handle, ctypes.byref(code))
    kernel.CloseHandle(handle)
    return bool(ok) and code.value == 259  # STILL_ACTIVE


def load_queue(path: Path) -> tuple[str, list[dict]]:
    title, runs = "", []
    if not path.exists():
        return title, runs
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line.startswith("#"):
            title = title or line.lstrip("#").strip()
        elif line:
            kind, cap, log, *label = line.split(maxsplit=3)
            runs.append({"kind": kind, "cap": int(cap), "log": log,
                         "label": label[0] if label else Path(log).stem})
    return title, runs


def own_rate(run: dict) -> float | None:
    span = run["touched"] - run["start"]
    return run["games"] / span if run.get("games") and span > 0 else None


def settle(run: dict, later: bool, now: float) -> str:
    if "source" not in run:
        return "queued"
    if run.get("finished") or (run.get("cap") and run["games"] >= run["cap"]):
        return "done"
    if run["source"] == "status":
        if not alive(run["pid"]):
            return "paused" if run.get("resumable") else "stopped"
    elif later:
        return "stopped"
    rate = own_rate(run)
    limit = FIT_QUIET if run["kind"] == "fit" else \
        max(STALL_GAMES / rate if rate else 600.0, STALL_FLOOR)
    return "stalled" if now - run["touched"] > limit else "running"


def fit_typical(statuses: list[dict], now: float) -> float | None:
    """How long the day's finished fits took, for one not yet started."""
    spans = [s["touched"] - s["start"] for s in statuses
             if s["kind"] == "fit" and s["finished"] and not s.get("code")
             and now - s["touched"] < RECENT]
    return statistics.median(spans) if spans else None


def fit_left(run: dict, statuses: list[dict], now: float) -> float | None:
    """Seconds to a fit's end: its steps at the pace of those done, then scoring the held-out
    games again, which takes about as long as a warm fit's first scoring did."""
    typical = fit_typical(statuses, now)
    if "source" not in run:
        return typical
    tail = max(FIT_TAIL, run["train_at"] - run["read_at"]) if run["read_at"] else FIT_TAIL
    if run["phase"] == "training" and run["step"] and run["train_at"]:
        pace = (run["touched"] - run["train_at"]) / run["step"]
        return max(0.0, (run["steps"] - run["step"]) * pace + tail - (now - run["touched"]))
    if run["steps"] and run["step"] >= run["steps"]:
        return max(0.0, tail - (now - run["touched"]))
    return max(FIT_TAIL, typical - (now - run["start"])) if typical else None


def median_sprt(directory: str, statuses: list[dict]) -> float | None:
    lengths = {p.stem: i["games"] for p in Path(directory).glob("*.log")
               if (i := read_log(p)) and i["verdict"]}
    done = [s for s in statuses if s["kind"] == "sprt" and s["finished"]]
    lengths.update({Path(s["ptn"]).stem: s["games"] for s in done if s["dir"] == directory})
    if not lengths:
        lengths = {str(i): s["games"] for i, s in enumerate(done)}
    return statistics.median(lengths.values()) if lengths else None


def expected_left(run: dict, statuses: list[dict]) -> float:
    games, left = run.get("games", 0), run["cap"] - run.get("games", 0)
    if run["kind"] != "sprt":
        return left
    llr = run.get("llr")
    if llr and games >= EARLY_GAMES:
        drift = llr / games
        need = (run["upper"] - llr) / drift if drift > 0 else (llr - run["lower"]) / -drift
        return min(left, max(0.0, need))
    median = median_sprt(run["dir"], statuses)
    return min(left, max(0.0, median - games)) if median else left


def build(queue: Path) -> dict:
    now = time.time()
    title, entries = load_queue(queue)
    statuses = {p: s for p in LIVE.glob("*.json") if (s := read_status(p))}
    by_ptn = {(s["dir"], Path(s["ptn"]).stem): p for p, s in statuses.items() if s["ptn"]}
    used, listed, plan = set(), set(), []
    for e in entries:
        target = ROOT / e["log"]
        listed.add(folder(target))
        run = {"kind": e["kind"], "cap": e["cap"], "dir": folder(target.parent), "planned": True}
        found = by_ptn.get((run["dir"], target.stem))
        if found:
            used.add(found)
            run.update(statuses[found])
        elif e["kind"] not in OWN_STATUS and (info := read_log(target)):
            run.update(info)
        run["label"] = e["label"]
        plan.append(run)
    for i, run in enumerate(plan):
        run["status"] = settle(run, any("source" in r for r in plan[i + 1:]), now)

    others = []
    for p, s in statuses.items():
        run: dict = dict(s, planned=False)
        run["status"] = settle(run, False, now)
        if p not in used and (run["status"] in (*LIVELY, "paused")
                              or now - run["touched"] < RECENT):
            others.append(run)
    covered = {(s["dir"], Path(s["ptn"]).stem) for s in statuses.values() if s["ptn"]}
    for p in [*GAMES.glob("*.log"), *GAMES.glob("*/*.log")]:
        try:
            fresh = now - p.stat().st_mtime < DISCOVER
        except OSError:
            continue
        if not fresh or folder(p) in listed or (folder(p.parent), p.stem) in covered:
            continue
        info = read_log(p)
        if info and info["games"]:
            run = dict(info, label=p.stem, planned=False, dir=folder(p.parent),
                       kind="sprt" if "llr" in info else "games",
                       cap=SPRT_CAP if "llr" in info else None)
            run["status"] = settle(run, False, now)
            others.append(run)

    all_statuses = list(statuses.values())
    fallback = {(s["kind"], s["cap"]): r for s in all_statuses if (r := own_rate(s))}
    rates: dict[tuple, float] = {}
    cursor = latest = now
    known = True
    for run in plan:
        if run["kind"] == "fit":
            run["rate"] = None
            if run["status"] in ENDED:
                continue
            left = fit_left(run, all_statuses, now) if known else None
            if left is None:
                known = False
                continue
            run["starts"] = cursor
            run["eta"], run["worst"] = cursor + left, latest + left
            cursor, latest = run["eta"], run["worst"]
            continue
        group = (run["kind"], run["cap"], run["dir"])
        if (rate := own_rate(run) if "source" in run else None):
            rates[group] = rate
        run["rate"] = rates.get(group) or fallback.get((run["kind"], run["cap"]))
        if run["status"] in ENDED:
            continue
        if not run["rate"] or not known:
            known = False
            continue
        run["starts"] = cursor
        run["eta"] = cursor + expected_left(run, all_statuses) / run["rate"]
        run["worst"] = latest + (run["cap"] - run.get("games", 0)) / run["rate"]
        cursor, latest = run["eta"], run["worst"]
    for run in others:
        if run["kind"] == "fit":
            run["rate"] = None
            if run["status"] in LIVELY and (left := fit_left(run, all_statuses, now)) is not None:
                run["eta"] = run["worst"] = now + left
            continue
        run["rate"] = own_rate(run)
        if run["status"] in LIVELY and run["rate"] and run["cap"]:
            run["eta"] = now + expected_left(run, all_statuses) / run["rate"]
            run["worst"] = now + (run["cap"] - run["games"]) / run["rate"]
    others.sort(key=lambda r: r["touched"])

    runs = [{k: (None if isinstance(v, float) and not math.isfinite(v) else v)
             for k, v in r.items()} for r in plan + others]
    pending = any(r["status"] not in ENDED for r in plan)
    return {"now": now, "title": title, "runs": runs,
            "end": cursor if known and pending else None,
            "latest": latest if known and pending else None}


def clock(t: float | None) -> str:
    return time.strftime("%H:%M", time.localtime(t)) if t else "?"


def duration(seconds: float) -> str:
    seconds = max(0, int(seconds))
    if seconds < 60:
        return f"{seconds}s"
    m = seconds // 60
    return f"{m // 60}h {m % 60:02d}m" if m >= 60 else f"{m}m"


def score(run: dict, short: bool = False) -> str:
    if run.get("w") is None:
        return ""
    if "lo" in run:
        return f"+{run['w']} -{run['l']} ={run['d']}  {run['elo']:+.0f} [{run['lo']:+d}, {run['hi']:+d}]"
    elo = f"Elo {run['elo'] or 0:+.1f} ±{run['err'] or 0:.1f}"
    return elo if short else f"+{run['w']} -{run['l']} ={run['d']}  {elo}"


def job_score(run: dict) -> str:
    """What a finished fit or datagen run shows in place of a match's score."""
    if run["kind"] == "fit" and run.get("averaged"):
        return f"held out {run['averaged'][0]:.4f}, results {run['averaged'][1]:.4f}"
    if run["kind"] == "datagen":
        return f"{run['positions']:,} positions"
    return ""


def job_progress(run: dict) -> tuple[float, str]:
    """A fit's place: its steps once training, else the files it has read."""
    if run["steps"]:
        return run["step"] / run["steps"], \
            f"step {run['step']}/{run['steps']}, epoch {run['epoch']}/{run['epochs']}"
    read, files = run["files"]
    return (read / files if files else 0.0), f"read {read}/{files} files"


def bar(fraction: float, width: int = 30) -> str:
    filled = round(width * max(0.0, min(1.0, fraction)))
    return "█" * filled + "░" * (width - filled)


def gauge(llr: float, lower: float, upper: float, width: int = 31) -> str:
    cells = ["─"] * width
    cells[round(-lower / (upper - lower) * (width - 1))] = "┼"
    cells[max(0, min(width - 1, round((llr - lower) / (upper - lower) * (width - 1))))] = "●"
    return f"{lower:+.2f} ├{''.join(cells)}┤ {upper:+.2f}"


def summary(model: dict) -> str:
    now, runs = model["now"], model["runs"]
    plan = [r for r in runs if r["planned"]]
    ended = sum(r["status"] in ENDED for r in plan)
    busy = sum(r["status"] in LIVELY for r in runs if not r["planned"])
    parts = []
    if plan:
        parts.append(f"all {len(plan)} queued runs done" if ended == len(plan)
                     else f"{ended} of {len(plan)} queued runs done")
    if model["end"]:
        parts.append(f"the queue ends ~{clock(model['end'])}, {duration(model['end'] - now)} "
                     f"from now, {clock(model['latest'])} at the latest")
    if busy:
        parts.append(f"{busy} other run{'s' if busy > 1 else ''} going")
    return ". ".join(p[:1].upper() + p[1:] for p in parts) if parts else "Nothing running"


def render_text(model: dict, colour: bool) -> str:
    def paint(code: str, s: str) -> str:
        return f"\x1b[{code}m{s}\x1b[0m" if colour else s

    now, runs = model["now"], model["runs"]
    out = [paint("1", model["title"] or "Runs") + "   " + time.strftime("%H:%M:%S"),
           summary(model), ""]
    if not runs:
        return "\n".join(out)
    cols = shutil.get_terminal_size((110, 40)).columns
    width = max(16, min(max(len(r["label"]) for r in runs), cols - 64))
    ended = sorted((r for r in runs if r["status"] in ENDED), key=lambda r: -r["touched"])
    for run in ended:
        label = run["label"][:width].ljust(width)
        if run.get("verdict"):
            tag = paint({"PASSED": "32", "FAILED": "31"}.get(run["verdict"], "33"),
                        f"{run['verdict']:<12}")
        else:
            tag = paint("33", f"{run['status']:<12}") if run["status"] != "done"                 else "done        "
        if run["kind"] == "fit":
            count = f"{run['epochs']} ep"
        else:
            count = str(run["games"]) if run["status"] == "done" or not run["cap"] \
                else f"{run['games']}/{run['cap']}"
        result = job_score(run) if run["kind"] in OWN_STATUS else score(run, True)
        out.append(f"  {label}  {tag} {count:>8}  {result:<30} "
                   f"{duration(run['touched'] - run['start']):>7}")
    for run in (r for r in runs if r["status"] in LIVELY):
        hue = "36" if run["status"] == "running" else "1;33"
        games, cap = run["games"], run["cap"]
        out += ["", f"  {paint('1', run['label'])}  {paint(hue, run['status'].upper())}"
                + ("" if run["planned"] else paint("2", "  (not queued)"))]
        if run["kind"] == "fit":
            fraction, where = job_progress(run)
            out.append(f"    {paint(hue, bar(fraction))} {where}  {run['phase']}  "
                       f"started {clock(run['start'])}")
            if run["positions"]:
                out.append(f"    {run['positions']:,} positions"
                           + (f" on {run['device']}" if run["device"] else ""))
            for i, (_, held, results) in enumerate(run["losses"], 1):
                out.append(f"    epoch {i}: held out {held:.6f}, against results {results:.6f}")
            if run.get("averaged"):
                out.append(f"    averaged: held out {run['averaged'][0]:.6f}, "
                           f"against results {run['averaged'][1]:.6f}")
        else:
            rate = f"  {run['rate'] * 60:.0f}/min" if run["rate"] else ""
            progress = f"{paint(hue, bar(games / cap))} {games}/{cap}" if cap else f"{games}"
            out.append(f"    {progress} games{rate}  started {clock(run['start'])}")
        if run["kind"] == "datagen" and run["positions"]:
            out.append(f"    {run['positions']:,} positions")
        if run.get("w") is not None:
            out.append(f"    {score(run)}" + (f"  pairs {run['pairs']}" if run.get("pairs") else ""))
        if run.get("llr") is not None:
            out.append(f"    LLR {gauge(run['llr'], run['lower'], run['upper'])}  {run['llr']:+.2f}")
        if run.get("moved"):
            out.append(f"    moved {run['moved'][:cols - 12]}")
        if run["status"] == "stalled":
            out.append(paint("1;33", f"    no game for {duration(now - run['touched'])}: hung?"))
        elif run.get("eta"):
            line = f"    ends ~{clock(run['eta'])}, {duration(run['eta'] - now)} left"
            if run["worst"] - run["eta"] > 60:
                line += f", {clock(run['worst'])} at the cap"
            out.append(line)
    queued = [r for r in runs if r["status"] == "queued"]
    if queued:
        out.append("")
    for run in queued:
        label = run["label"][:width].ljust(width)
        when = f"starts ~{clock(run['starts'])}, ends ~{clock(run['eta'])}" if run.get("eta") else ""
        out.append(paint("2", f"  {label}  queued       {when}"))
    return "\n".join(out)


PAGE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Run Queue</title>
<style>
:root { --bg:#f6f6f3; --card:#fff; --ink:#1c1c1a; --mute:#6c6b66; --line:#e2e1dc; --track:#ebeae5;
  --run:#2b6cd4; --ok:#1d8048; --bad:#c0362d; --warn:#a86f12; }
@media (prefers-color-scheme: dark) { :root { --bg:#141413; --card:#1d1d1b; --ink:#ecebe6;
  --mute:#9b9a93; --line:#2d2c29; --track:#2a2926; --run:#6fa2ff; --ok:#4fc27f; --bad:#ff6e64;
  --warn:#e2b04c; } }
* { box-sizing:border-box; }
body { margin:0; background:var(--bg); color:var(--ink);
  font:15px/1.45 system-ui,-apple-system,"Segoe UI",sans-serif; }
main { max-width:880px; margin:0 auto; padding:22px 16px 48px; }
h1 { font-size:20px; margin:0; }
.sub { color:var(--mute); margin:2px 0 18px; }
.num { font-variant-numeric:tabular-nums; }
.card { background:var(--card); border:1px solid var(--line); border-radius:10px;
  padding:14px 16px; margin-bottom:12px; }
.head { display:flex; gap:10px; align-items:baseline; flex-wrap:wrap; }
.head b { flex:1 1 240px; }
.tag { font-size:12px; font-weight:700; letter-spacing:.04em; }
.running { color:var(--run); } .stalled { color:var(--warn); } .PASSED { color:var(--ok); }
.FAILED { color:var(--bad); } .INCONCLUSIVE, .stopped, .paused { color:var(--warn); }
.extra { color:var(--mute); font-size:12px; }
.bar { position:relative; height:8px; background:var(--track); border-radius:99px; margin:10px 0 8px; }
.bar i { position:absolute; left:0; top:0; bottom:0; background:var(--run); border-radius:99px; }
.bar u { position:absolute; top:-3px; bottom:-3px; width:2px; background:var(--mute); }
.stalled .bar i { background:var(--warn); }
.facts { display:flex; gap:6px 18px; flex-wrap:wrap; color:var(--mute); }
.facts span b { color:var(--ink); font-weight:600; }
.llr { position:relative; height:8px; margin:14px 0 4px; border-radius:99px;
  background:linear-gradient(90deg, var(--bad), var(--track) 50%, var(--ok)); opacity:.85; }
.llr i { position:absolute; top:-4px; width:4px; height:16px; margin-left:-2px; border-radius:2px;
  background:var(--ink); }
.llr u { position:absolute; top:0; bottom:0; width:1px; background:var(--ink); opacity:.4; }
.ends { display:flex; justify-content:space-between; color:var(--mute); font-size:12px; }
table { width:100%; border-collapse:collapse; }
td { padding:7px 6px; border-top:1px solid var(--line); vertical-align:baseline; }
tr:first-child td { border-top:0; }
td.r { text-align:right; white-space:nowrap; }
.queued td { color:var(--mute); }
h2 { font-size:13px; text-transform:uppercase; letter-spacing:.06em; color:var(--mute); margin:22px 0 8px; }
.warn { color:var(--warn); font-weight:600; }
#stale { color:var(--warn); }
.params { display:grid; grid-template-columns:repeat(auto-fill, minmax(240px, 1fr)); gap:6px 16px;
  margin-top:10px; font-size:13px; }
/* One line whatever the lengths, so every cell is the same height and the rows stay aligned. */
.p .pt { display:flex; justify-content:space-between; gap:8px; white-space:nowrap; }
.p .pn { color:var(--mute); min-width:0; overflow:hidden; text-overflow:ellipsis; }
.p .pt .num { flex-shrink:0; }
.p.ch .pn { color:var(--ink); font-weight:600; }
.rng { position:relative; height:4px; background:var(--track); border-radius:99px; margin-top:3px; }
.rng u { position:absolute; top:-2px; bottom:-2px; width:1px; background:var(--mute); }
.rng i { position:absolute; top:-2px; width:8px; height:8px; margin-left:-4px; border-radius:99px;
  background:var(--run); }
.p.ch .rng i { background:var(--warn); }
@media (max-width:560px) { .hide { display:none; } }
</style></head>
<body><main>
<h1 id="title">Runs</h1>
<div class="sub num"><span id="summary"></span> <span id="stale" hidden>· the server is not answering</span></div>
<div id="live"></div>
<h2 id="qh">Queued</h2><div class="card"><table id="queued"></table></div>
<h2 id="dh">Done</h2><div class="card"><table id="done"></table></div>
</main>
<script>
const $ = s => document.querySelector(s);
const esc = s => String(s).replace(/[&<>"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const clock = t => t ? new Date(t * 1000).toLocaleTimeString([], {hour: '2-digit', minute: '2-digit'}) : '?';
const dur = s => { s = Math.max(0, Math.round(s)); if (s < 60) return s + 's';
  const m = Math.floor(s / 60); return m >= 60 ? Math.floor(m / 60) + 'h ' + String(m % 60).padStart(2, '0') + 'm' : m + 'm'; };
const sign = x => (x >= 0 ? '+' : '') + x;
const ended = r => ['done', 'stopped', 'paused'].includes(r.status);
function score(r, short) {
  if (r.w === undefined || r.w === null) return '';
  if (r.lo !== undefined) return `+${r.w} −${r.l} =${r.d} &nbsp; <b>${sign(r.elo)}</b> [${sign(r.lo)}, ${sign(r.hi)}]`;
  const elo = `<b>${sign((r.elo || 0).toFixed(1))}</b> ± ${(r.err || 0).toFixed(1)}`;
  return short ? elo : `+${r.w} −${r.l} =${r.d} &nbsp; Elo ${elo}`;
}
const line = html => `<div class="facts num" style="margin-top:4px">${html}</div>`;
const losses = (what, held, results) => line(`${what}: held out <b>${held.toFixed(6)}</b>, against results <b>${results.toFixed(6)}</b>`);
function jobScore(r) {
  if (r.kind === 'fit') return r.averaged ? `held out <b>${r.averaged[0].toFixed(4)}</b>, results <b>${r.averaged[1].toFixed(4)}</b>` : '';
  if (r.kind === 'datagen') return `${r.positions.toLocaleString()} positions`;
  return score(r, true);
}
// Half up, as tools/tune.py rounds what it would ship.
const shipped = x => Math.floor(x + 0.5);
// In src/tune.h's order, which never changes, so a parameter stays where the eye left it.
function params(spsa) {
  const at = (x, p) => 100 * (x - p[3]) / Math.max(1, p[4] - p[3]);
  const changed = spsa.filter(p => shipped(p[2]) !== shipped(p[1])).length;
  const cells = spsa.map(p => {
    const ch = shipped(p[2]) !== shipped(p[1]);
    return `<div class="p${ch ? ' ch' : ''}" title="range ${p[3]} to ${p[4]}; would ship as ${shipped(p[2])}">
      <div class="pt"><span class="pn">${esc(p[0])}</span><span class="num">${p[1]} → <b>${p[2].toFixed(1)}</b></span></div>
      <div class="rng"><u style="left:${at(p[1], p)}%"></u><i style="left:${at(p[2], p)}%"></i></div></div>`;
  }).join('');
  return line(`${spsa.length} parameters, <b>${changed}</b> would ship changed`) + `<div class="params">${cells}</div>`;
}
function live(r, now) {
  let bar = '', facts = [], lines = '';
  if (r.kind === 'fit') {
    const reading = !r.steps;
    const part = reading ? (r.files[1] ? r.files[0] / r.files[1] : 0) : r.step / r.steps;
    bar = `<div class="bar"><i style="width:${100 * part}%"></i></div>`;
    facts.push(reading ? `<span>read <b class="num">${r.files[0]}</b> / ${r.files[1]} files</span>`
      : `<span>step <b class="num">${r.step}</b> / ${r.steps}, epoch <b>${r.epoch}</b> / ${r.epochs}</span>`);
    facts.push(`<span>${esc(r.phase)}</span>`);
    if (r.positions) facts.push(`<span><b class="num">${r.positions.toLocaleString()}</b> positions${r.device ? ' on ' + esc(r.device) : ''}</span>`);
    lines = r.losses.map((l, i) => losses('epoch ' + (i + 1), l[1], l[2])).join('');
    if (r.averaged) lines += losses('averaged', r.averaged[0], r.averaged[1]);
  } else if (r.cap) {
    const mark = r.eta && r.rate ? `<u style="left:${Math.min(100, 100 * (r.games + (r.eta - now) * r.rate) / r.cap)}%" title="expected end"></u>` : '';
    bar = `<div class="bar"><i style="width:${100 * r.games / r.cap}%"></i>${mark}</div>`;
    facts.push(`<span><b class="num">${r.games}</b> / ${r.cap} games</span>`);
  } else facts.push(`<span><b class="num">${r.games}</b> games</span>`);
  if (r.kind === 'datagen' && r.positions) lines = line(`<b>${r.positions.toLocaleString()}</b> positions`);
  if (r.rate) facts.push(`<span><b class="num">${Math.round(r.rate * 60)}</b>/min</span>`);
  facts.push(`<span>started <b>${clock(r.start)}</b></span>`);
  if (r.status === 'stalled') facts.push(`<span class="warn">no game for ${dur(now - r.touched)}: hung?</span>`);
  else if (r.eta) facts.push(`<span>ends ~<b>${clock(r.eta)}</b>, ${dur(r.eta - now)} left` +
    (r.worst - r.eta > 60 ? `, ${clock(r.worst)} at the cap` : '') + '</span>');
  let llr = '';
  if (r.llr !== undefined && r.llr !== null) {
    const at = x => 100 * (x - r.lower) / (r.upper - r.lower);
    llr = `<div class="llr"><u style="left:${at(0)}%"></u><i style="left:${Math.max(0, Math.min(100, at(r.llr)))}%"></i></div>
      <div class="ends num"><span>${r.lower.toFixed(2)} fail</span><span>LLR <b>${sign(r.llr.toFixed(2))}</b></span><span>pass ${sign(r.upper.toFixed(2))}</span></div>`;
  }
  return `<div class="card ${r.status}"><div class="head"><b>${esc(r.label)}</b>
    ${r.planned ? '' : '<span class="extra">not queued</span>'}
    <span class="tag ${r.status}">${r.status.toUpperCase()}</span></div>${bar}
    <div class="facts" style="margin-top:${bar ? 0 : 8}px">${facts.join('')}</div>
    <div class="facts num" style="margin-top:4px">${score(r)}${r.pairs ? ' &nbsp; pairs [' + r.pairs.join(', ') + ']' : ''}</div>${lines}${llr}
    ${r.spsa ? params(r.spsa) : r.moved ? line('moved ' + esc(r.moved)) : ''}</div>`;
}
function render(m) {
  const now = m.now, runs = m.runs;
  $('#title').textContent = m.title || 'Runs';
  document.title = m.title ? 'Queue: ' + m.title : 'Run Queue';
  $('#summary').textContent = m.summary + ` · updated ${new Date(now * 1000).toLocaleTimeString()}`;
  $('#live').innerHTML = runs.filter(r => r.status === 'running' || r.status === 'stalled').map(r => live(r, now)).join('');
  const queued = runs.filter(r => r.status === 'queued');
  $('#qh').hidden = $('#queued').parentNode.hidden = !queued.length;
  $('#queued').innerHTML = queued.map(r => `<tr class="queued"><td>${esc(r.label)}</td>
    <td class="r num">${r.eta ? '~' + clock(r.starts) + ' – ' + clock(r.eta) : ''}</td></tr>`).join('');
  const done = runs.filter(ended);
  $('#dh').hidden = $('#done').parentNode.hidden = !done.length;
  $('#done').innerHTML = done.sort((a, b) => b.touched - a.touched).map(r => {
    const tag = r.verdict || r.status;
    return `<tr><td>${esc(r.label)}</td><td class="tag ${tag}">${tag.toUpperCase()}</td>
      <td class="r num hide">${r.kind === 'fit' ? r.epochs + ' ep' : (r.status !== 'done' && r.cap ? r.games + '/' + r.cap : r.games) + ' g'}</td>
      <td class="r num">${jobScore(r)}</td><td class="r num hide">${dur(r.touched - r.start)}</td></tr>`;
  }).join('');
}
async function tick() {
  try { render(await (await fetch('data', {cache: 'no-store'})).json()); $('#stale').hidden = true; }
  catch (e) { $('#stale').hidden = false; }
}
tick(); setInterval(tick, 3000);
</script></body></html>
"""


def serve(queue: Path, host: str, port: int) -> None:
    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self) -> None:
            if self.path.split("?")[0] == "/data":
                model = build(queue)
                model["summary"] = summary(model)
                body, kind = json.dumps(model).encode(), "application/json"
            else:
                body, kind = PAGE.encode(), "text/html; charset=utf-8"
            self.send_response(200)
            self.send_header("Content-Type", kind)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, format: str, *args) -> None:
            pass

    server = http.server.ThreadingHTTPServer((host, port), Handler)
    print(f"serving at http://{'localhost' if host == '127.0.0.1' else host}:{port}", flush=True)
    server.serve_forever()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--queue", type=Path, default=QUEUE)
    parser.add_argument("--interval", type=float, default=5.0, help="seconds between redraws")
    parser.add_argument("--once", action="store_true", help="print once and exit")
    parser.add_argument("--serve", type=int, metavar="PORT", help="serve a page instead")
    parser.add_argument("--host", default="127.0.0.1",
                        help="0.0.0.0 lets other machines on the network see the page")
    args = parser.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    try:
        if args.serve:
            serve(args.queue, args.host, args.serve)
        elif args.once:
            print(render_text(build(args.queue), sys.stdout.isatty()))
        else:
            os.system("")  # lets a Windows console take ANSI escapes
            while True:
                text = render_text(build(args.queue), True).replace("\n", "\x1b[K\n")
                sys.stdout.write("\x1b[H" + text + "\x1b[K\n\x1b[J")
                sys.stdout.flush()
                time.sleep(args.interval)
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
