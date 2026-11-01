"""
match.py - play two TEI engines against each other, and decide when to stop.

    python tools/match.py --dev ./yapper.exe --base external/baselines/old.exe --sprt
    python tools/match.py --dev ./yapper.exe --base external/engines/opponent.exe \
        --tc 60+0.6 --games 200 --ptn external/games/gauntlet.ptn

Each opening is played as a pair with colours reversed, so the book's own bias cancels and
the pair is the unit of evidence. A pentanomial SPRT reads pairs, which is why it needs far
fewer games than counting wins would. Every move is refereed by tools/takref.py rather than
trusted, and an illegal move or a crash loses the game on the spot.

Games are written as PTN: open the file in https://ptn.ninja, or use tools/ptnlink.py.

Read docs/TESTING.md before trusting a result. One change per test, and do not read the
number early: stopping when it looks good is what the SPRT exists to replace.

Stdlib only.
"""

from __future__ import annotations

import argparse
import datetime
import math
import multiprocessing
import os
import queue
import random
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import takref

# racetrack's limit, and well past anything a sane game reaches.
MAX_PLIES = 1000

# How long past its clock an engine is given before it is presumed hung.
HANG_SECONDS = 5.0


def score_of_elo(elo: float) -> float:
    return 1.0 / (1.0 + 10.0 ** (-elo / 400.0))


def elo_of_score(score: float) -> float:
    score = min(max(score, 1e-6), 1.0 - 1e-6)
    return -400.0 * math.log10(1.0 / score - 1.0)


def constrained_pdf(pdf: list[tuple[float, float]], mean: float) -> list[float]:
    """The maximum-likelihood distribution with this expected score.

    pdf is (score, probability). The solution is p / (1 + t * (score - mean)) for the one t
    that gives the required mean, and the mean is monotonic in t, so bisection finds it.
    """
    def excess(t: float) -> float:
        return sum(p * (a - mean) / (1.0 + t * (a - mean)) for a, p in pdf)

    low = -1.0 / (max(a for a, _ in pdf) - mean) + 1e-12
    high = 1.0 / (mean - min(a for a, _ in pdf)) - 1e-12
    for _ in range(200):
        t = (low + high) / 2.0
        if excess(t) > 0.0:
            low = t
        else:
            high = t
    return [p / (1.0 + t * (a - mean)) for a, p in pdf]


def pentanomial_llr(pairs: list[int], elo0: float, elo1: float) -> float:
    """Log-likelihood ratio of a GSPRT on [LL, LD, LW+DD, WD, WW] pair counts."""
    total = sum(pairs)
    if total == 0:
        return 0.0
    # A category never seen must not read as impossible.
    counts = [max(c, 1e-3) for c in pairs]
    n = sum(counts)
    scores = (0.0, 0.25, 0.5, 0.75, 1.0)
    pdf = [(a, c / n) for a, c in zip(scores, counts)]
    h0 = constrained_pdf(pdf, score_of_elo(elo0))
    h1 = constrained_pdf(pdf, score_of_elo(elo1))
    return sum(c * math.log(p1 / p0) for c, p0, p1 in zip(pairs, h0, h1))


def pair_elo(pairs: list[int]) -> tuple[float, float]:
    """Elo and its 95% half-width, from the variance of the pairs themselves."""
    n = sum(pairs)
    if n == 0:
        return 0.0, 0.0
    scores = (0.0, 0.25, 0.5, 0.75, 1.0)
    mean = sum(a * c for a, c in zip(scores, pairs)) / n
    variance = sum(c * (a - mean) ** 2 for a, c in zip(scores, pairs)) / n
    error = 1.96 * math.sqrt(variance / n)
    return (elo_of_score(mean),
            (elo_of_score(mean + error) - elo_of_score(mean - error)) / 2.0)


class EngineError(Exception):
    pass


class Engine:
    """One TEI process. Reads on its own thread so a hung engine can be timed out."""

    def __init__(self, command: list[str], options: dict[str, str]):
        self.command = command
        self.options = options
        self.name = Path(command[0]).stem
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.DEVNULL, text=True, bufsize=1)
        self.lines: queue.Queue[str | None] = queue.Queue()
        threading.Thread(target=self._reader, daemon=True).start()
        self.send("tei")
        declared = set()
        while True:
            line = self.read(10.0)
            if line.startswith("id name "):
                self.name = line[8:].strip()
            elif line.startswith("option name "):
                declared.add(line.split()[2])
            elif line.strip() == "teiok":
                break
        for name, value in options.items():
            if name == "HalfKomi" and "HalfKomi" not in declared:
                if value != "0":
                    raise EngineError(f"{self.name} has no HalfKomi option")
                continue
            self.send(f"setoption name {name} value {value}")

    def _reader(self) -> None:
        for line in self.process.stdout:
            self.lines.put(line)
        self.lines.put(None)

    def send(self, line: str) -> None:
        try:
            self.process.stdin.write(line + "\n")
            self.process.stdin.flush()
        except OSError as error:
            raise EngineError(f"{self.name} stopped listening") from error

    def read(self, timeout: float) -> str:
        try:
            line = self.lines.get(timeout=max(timeout, 0.001))
        except queue.Empty:
            raise EngineError(f"{self.name} hung") from None
        if line is None:
            raise EngineError(f"{self.name} exited")
        return line

    def wait_ready(self) -> None:
        self.send("isready")
        deadline = time.monotonic() + 30.0
        while self.read(deadline - time.monotonic()).strip() != "readyok":
            pass

    def close(self) -> None:
        try:
            self.send("quit")
            self.process.wait(timeout=2)
        except (EngineError, subprocess.TimeoutExpired):
            self.process.kill()


@dataclass
class Limits:
    base: float = 0.0
    inc: float = 0.0
    nodes: int = 0
    depth: int = 0

    def go(self, clocks: list[float]) -> str:
        if self.nodes:
            return f"go nodes {self.nodes}"
        if self.depth:
            return f"go depth {self.depth}"
        return (f"go wtime {int(clocks[0] * 1000)} btime {int(clocks[1] * 1000)} "
                f"winc {int(self.inc * 1000)} binc {int(self.inc * 1000)}")


@dataclass
class GameResult:
    ptn_result: str          # R-0, 0-R, F-0, 0-F, 1/2-1/2, 1-0, 0-1
    termination: str
    moves: list[str] = field(default_factory=list)
    comments: list[str] = field(default_factory=list)
    opening_length: int = 0

    def score_for_white(self) -> float:
        if self.ptn_result in ("R-0", "F-0", "1-0"):
            return 1.0
        if self.ptn_result in ("0-R", "0-F", "0-1"):
            return 0.0
        return 0.5


def play_game(white: Engine, black: Engine, opening: list[str], size: int, halfkomi: int,
              limits: tuple[Limits, Limits]) -> GameResult:
    """`limits` is by colour: an unequal match is how the harness itself gets tested."""
    game = takref.Game(size, halfkomi)
    played: list[str] = []
    comments: list[str] = []
    seen: dict[str, int] = {game.key(): 1}

    for engine in (white, black) if white is not black else (white,):
        engine.send(f"teinewgame {size}")
        engine.wait_ready()

    for move in opening:
        game.play(move)
        played.append(takref.canonical(move))
        comments.append("book")
        seen[game.key()] = seen.get(game.key(), 0) + 1

    clocks = [limits[takref.WHITE].base, limits[takref.BLACK].base]

    def finish(result: str, reason: str = "") -> GameResult:
        return GameResult(result, reason, played, comments, len(opening))

    while True:
        result = game.result()
        if result is not None:
            return finish(result)
        if seen[game.key()] >= 3:
            return finish("1/2-1/2", "threefold repetition")
        if len(played) >= MAX_PLIES:
            return finish("1/2-1/2", f"reached {MAX_PLIES} plies")

        color = game.to_move
        engine = white if color == takref.WHITE else black
        forfeit = "0-1" if color == takref.WHITE else "1-0"
        side = "white" if color == takref.WHITE else "black"
        mine = limits[color]
        timed = not (mine.nodes or mine.depth)

        position = "position startpos" + (" moves " + " ".join(played) if played else "")
        started = time.monotonic()
        try:
            engine.send(position)
            engine.send(mine.go(clocks))
            deadline = started + (clocks[color] if timed else 3600.0) + HANG_SECONDS
            move, comment = None, ""
            while move is None:
                line = engine.read(deadline - time.monotonic())
                if line.startswith("bestmove"):
                    parts = line.split()
                    move = parts[1] if len(parts) > 1 else ""
                elif line.startswith("info") and " score " in line:
                    words = line.split()
                    if "cp" in words:
                        cp = int(words[words.index("cp") + 1])
                        depth = words[words.index("depth") + 1] if "depth" in words else "?"
                        # PTN comments read from white's side, as racetrack writes them.
                        cp = cp if color == takref.WHITE else -cp
                        comment = f"{cp / 100:+.2f}/{depth}"
        except EngineError as error:
            engine.process.kill()
            return finish(forfeit, f"{side}: {error}")

        elapsed = time.monotonic() - started
        try:
            move = takref.canonical(move)
        except ValueError:
            return finish(forfeit, f"{side} sent a malformed move: {move!r}")
        if move not in game.legal_moves():
            return finish(forfeit, f"{side} made an illegal move: {move}")

        if timed:
            if elapsed > clocks[color]:
                return finish(forfeit, f"{side} lost on time")
            clocks[color] += mine.inc - elapsed

        game.play(move)
        played.append(move)
        comments.append(f"{comment} {elapsed:.2f}s".strip())
        seen[game.key()] = seen.get(game.key(), 0) + 1


def write_ptn(path: Path, result: GameResult, white: str, black: str, size: int, halfkomi: int,
              clock: str, round_number: int) -> None:
    today = datetime.date.today()
    tags = [("Site", "Yapper match"), ("Date", f"{today.year}.{today.month:02}.{today.day:02}"),
            ("Round", str(round_number)), ("Player1", white), ("Player2", black),
            ("Size", str(size)), ("Clock", clock), ("Result", result.ptn_result)]
    if halfkomi:
        tags.append(("Komi", f"{halfkomi / 2:g}"))

    # PTN Ninja refuses a file with a tag it does not know, so a game's ending goes in a
    # comment rather than racetrack's Termination tag.
    comments = list(result.comments)
    if result.termination and comments:
        comments[-1] = f"{comments[-1]} {result.termination}".strip()

    lines = [f'[{name} "{value}"]' for name, value in tags] + [""]
    plies = list(zip(result.moves, comments))
    for number, start in enumerate(range(0, len(plies), 2), 1):
        chunk = plies[start:start + 2]
        text = " ".join(f"{move} {{{comment}}}" if comment else move for move, comment in chunk)
        lines.append(f"{number}. {text}")
    lines[-1] += f" {result.ptn_result}"
    with path.open("a", encoding="utf-8", newline="\n") as out:
        out.write("\n".join(lines) + "\n\n")


class Tally:
    def __init__(self):
        self.wins = self.losses = self.draws = 0
        self.pairs = [0, 0, 0, 0, 0]  # dev's score in the pair: 0, .5, 1, 1.5, 2
        self.games = 0
        self.endings: dict[str, int] = {}

    def add_pair(self, first: float, second: float) -> None:
        for score in (first, second):
            self.wins += score == 1.0
            self.losses += score == 0.0
            self.draws += score == 0.5
        self.games += 2
        self.pairs[int((first + second) * 2)] += 1

    def line(self) -> str:
        elo, error = pair_elo(self.pairs)
        return (f"{self.games:6} games  +{self.wins} -{self.losses} ={self.draws}  "
                f"Elo {elo:+7.1f} +/- {error:5.1f}  pairs {self.pairs}")


def read_book(path: str | None, size: int) -> list[list[str]]:
    if path:
        book = []
        for line in Path(path).read_text().splitlines():
            line = line.split("#")[0].strip()
            if line:
                book.append(line.split())
        return book
    # Without a book, the first two stones are all that varies.
    squares = [takref.square_name(r, f) for r in range(size) for f in range(size)]
    return [[a, b] for a in squares for b in squares if a != b]


def parse_tc(text: str) -> tuple[float, float]:
    base, _, inc = text.partition("+")
    return float(base), float(inc or 0.0)


def split_options(text: str | None) -> dict[str, str]:
    options = {}
    for item in (text or "").split(","):
        if item:
            name, _, value = item.partition("=")
            options[name] = value
    return options


@dataclass
class Config:
    dev_command: list[str]
    base_command: list[str]
    dev_options: dict[str, str]
    base_options: dict[str, str]
    dev_limits: Limits
    base_limits: Limits
    book: list[list[str]]
    size: int
    halfkomi: int


# Each worker is a process, not a thread. The referee is slow Python, and on a thread it
# would hold the GIL while another game's clock ran, flagging engines for the runner's own
# delay.
WORKER: dict = {}


def worker_start(config: Config) -> None:
    WORKER["config"] = config
    WORKER["dev"] = Engine(config.dev_command, config.dev_options)
    WORKER["base"] = Engine(config.base_command, config.base_options)


def revived(engine: Engine) -> Engine:
    """A crash costs one game, not every game after it."""
    if engine.process.poll() is None:
        return engine
    return Engine(engine.command, engine.options)


def run_pair(index: int) -> tuple[int, list[tuple[GameResult, str, str, bool]]]:
    config: Config = WORKER["config"]
    opening = config.book[index % len(config.book)]
    games = []
    for dev_is_white in (True, False):
        dev = WORKER["dev"] = revived(WORKER["dev"])
        base = WORKER["base"] = revived(WORKER["base"])
        white, black = (dev, base) if dev_is_white else (base, dev)
        limits = ((config.dev_limits, config.base_limits) if dev_is_white
                  else (config.base_limits, config.dev_limits))
        result = play_game(white, black, opening, config.size, config.halfkomi, limits)
        games.append((result, white.name, black.name, dev_is_white))
    return index, games


def main() -> int:
    default_engine = "./yapper.exe" if os.name == "nt" else "./yapper"
    parser = argparse.ArgumentParser(description="Play two TEI engines against each other.")
    parser.add_argument("--dev", default=default_engine)
    parser.add_argument("--base", required=True)
    parser.add_argument("--dev-arg", help="an argument for the dev binary, such as tei")
    parser.add_argument("--base-arg")
    parser.add_argument("--dev-options", help="NAME=VALUE,NAME=VALUE")
    parser.add_argument("--base-options")
    parser.add_argument("--size", type=int, default=6)
    parser.add_argument("--halfkomi", type=int, default=4,
                        help="default 4: two komi on 6x6 is where top engines are compared")
    parser.add_argument("--tc", default="10+0.1", help="base+increment, seconds")
    parser.add_argument("--dev-nodes", type=int, default=0, help="a fixed nodes limit, per move")
    parser.add_argument("--base-nodes", type=int, default=0)
    parser.add_argument("--dev-depth", type=int, default=0, help="a fixed depth limit, per move")
    parser.add_argument("--base-depth", type=int, default=0)
    parser.add_argument("--book", help="one opening per line, as PTN moves")
    parser.add_argument("--games", type=int, default=0, help="stop here; 0 means 2000 under SPRT")
    parser.add_argument("--concurrency", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    parser.add_argument("--sprt", action="store_true")
    parser.add_argument("--elo0", type=float, default=0.0)
    parser.add_argument("--elo1", type=float, default=5.0)
    parser.add_argument("--alpha", type=float, default=0.05)
    parser.add_argument("--beta", type=float, default=0.05)
    parser.add_argument("--ptn", help="append every game here, in PTN")
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()

    book = read_book(args.book, args.size)
    random.Random(args.seed).shuffle(book)
    games = args.games or (2000 if args.sprt else 100)
    pair_count = (games + 1) // 2

    base_time, inc = parse_tc(args.tc)
    config = Config(
        dev_command=[args.dev] + ([args.dev_arg] if args.dev_arg else []),
        base_command=[args.base] + ([args.base_arg] if args.base_arg else []),
        dev_options={**split_options(args.dev_options), "HalfKomi": str(args.halfkomi)},
        base_options={**split_options(args.base_options), "HalfKomi": str(args.halfkomi)},
        dev_limits=Limits(base_time, inc, args.dev_nodes, args.dev_depth),
        base_limits=Limits(base_time, inc, args.base_nodes, args.base_depth),
        book=book, size=args.size, halfkomi=args.halfkomi)

    # A bad path or option must fail here: an initializer that raises makes a pool respawn
    # its workers forever.
    for command, options in ((config.dev_command, config.dev_options),
                             (config.base_command, config.base_options)):
        try:
            Engine(command, options).close()
        except (OSError, EngineError) as error:
            print(f"cannot start {command[0]}: {error}", file=sys.stderr)
            return 2

    ptn_path = Path(args.ptn) if args.ptn else None
    if ptn_path:
        ptn_path.parent.mkdir(parents=True, exist_ok=True)

    tally = Tally()
    upper = math.log((1.0 - args.beta) / args.alpha)
    lower = math.log(args.beta / (1.0 - args.alpha))
    verdict = None
    started = time.monotonic()

    with multiprocessing.Pool(args.concurrency, worker_start, (config,)) as pool:
        # Leaving this block kills the workers, and an engine exits when its stdin closes.
        for index, played in pool.imap_unordered(run_pair, range(pair_count)):
            scores = []
            for number, (result, white, black, dev_is_white) in enumerate(played):
                score = result.score_for_white()
                scores.append(score if dev_is_white else 1.0 - score)
                if ptn_path:
                    write_ptn(ptn_path, result, white, black, args.size, args.halfkomi, args.tc,
                              index * 2 + number + 1)
                if result.termination:
                    key = result.termination.split(":")[0]
                    tally.endings[key] = tally.endings.get(key, 0) + 1
            tally.add_pair(scores[0], scores[1])

            line = tally.line()
            if args.sprt:
                llr = pentanomial_llr(tally.pairs, args.elo0, args.elo1)
                line += f"  LLR {llr:+.2f} [{lower:+.2f}, {upper:+.2f}]"
                if llr >= upper or llr <= lower:
                    verdict = "PASSED" if llr >= upper else "FAILED"
            print(line, flush=True)
            if verdict:
                break

    print(f"\n{tally.line()}")
    print(f"{time.monotonic() - started:.0f}s, {args.concurrency} concurrent")
    if tally.endings:
        print(f"endings: {tally.endings}")
    if args.sprt:
        print(f"SPRT [{args.elo0}, {args.elo1}]: {verdict or 'INCONCLUSIVE'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
