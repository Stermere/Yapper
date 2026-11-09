"""
playtakbot.py - the engine plays on PlayTak, https://playtak.com.

    python tools/playtakbot.py --user YapperBot                # 6x6, 600+20, komi 2, unrated
    python tools/playtakbot.py --user YapperBot --size 5 --tc 300+5 --halfkomi 0 --games 10
    python tools/playtakbot.py --user YapperBot --accept BeginnerBot6 --rated
    python tools/playtakbot.py --guest --opponent Guest123     # no account: a test game

It posts a seek, plays whoever takes it, and seeks again. With --accept it takes that player's
seeks instead of posting its own. The password comes from PLAYTAK_PASSWORD or is asked for, never
from the command line, where the shell's history and the process list would keep it.

PlayTak's admins mark the accounts programs play from as bots. Until this one is, it plays
unrated games only: --rated is for after.

Ctrl+C once stops seeking and lets a game in progress finish; twice quits at once, which loses
that game when the server's reconnection window runs out. A dropped connection is retried, and
the server hands back the game in progress. Every finished game is appended to
external/games/playtak.ptn, with a link to replay it.

The protocol is the server's own (USTakAssociation/tak-server), read from what it sends: `Seek`
needs its trailing field even when empty, or the server reads the fields differently.

Stdlib only.
"""

from __future__ import annotations

import argparse
import getpass
import os
import queue
import re
import signal
import socket
import sys
import threading
import time
import urllib.parse
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import match
import playtak
import ptnlink
import takref

HOST, PORT = "playtak.com", 10000

# The server drops a client it has not heard from; its documentation asks for a PING this often.
PING_SECONDS = 30.0
RETRY_SECONDS = 30.0

PTN = Path(__file__).resolve().parent.parent / "external" / "games" / "playtak.ptn"

WELCOME = re.compile(r"Welcome (\S+)!$")
# Game Start <no> <size> <white> vs <black> <your colour> <seconds> <halfkomi> <stones> <caps>
# <trigger move> <extra seconds>. The increment is not in it, so it comes from the seek.
GAME_START = re.compile(r"Game Start (\d+) (\d+) (\S+) vs (\S+) (white|black) (\d+) (\d+) (\d+) "
                        r"(\d+)")
# Seek new <no> <name> <size> <seconds> <increment> <W|B|A> <halfkomi> <stones> <caps>
# <unrated> <tournament> <trigger move> <extra seconds> [opponent]
SEEK_NEW = re.compile(r"Seek new (\d+) (\S+) (\d+) (\d+) (\d+) ([WBA]) (\d+) (\d+) (\d+) ([01]) "
                      r"([01]) (\d+) (\d+) ?(\S*)")


def say(text: str) -> None:
    print(f"{time.strftime('%H:%M:%S')} {text}", flush=True)


def server_move(move: str) -> str:
    """A canonical PTN move in the server's notation: `P A1 W`, or `M A1 C1 1 2`."""
    count, kind, file, rank, arrow, drops = takref.MOVE_RE.match(move).groups()
    start = file.upper() + rank
    if not arrow:
        return f"P {start}" + {"": "", "S": " W", "C": " C"}[kind]
    hops = [int(d) for d in drops] or [int(count or 1)]
    rank_step, file_step = takref.STEPS[arrow]
    end = "ABCDEFGH"[ord(file) - ord("a") + file_step * len(hops)] + \
        str(int(rank) + rank_step * len(hops))
    return f"M {start} {end} " + " ".join(map(str, hops))


def score_comment(line: str, color: int) -> str:
    words = line.split()
    if "cp" not in words:
        return ""
    cp = int(words[words.index("cp") + 1])
    depth = words[words.index("depth") + 1] if "depth" in words else "?"
    # PTN comments read from white's side, as racetrack writes them.
    return f"{(cp if color == takref.WHITE else -cp) / 100:+.2f}/{depth}"


class Server:
    """The server's line protocol over TCP. Lines are read on their own thread and queued as
    ("server", self, line), then ("closed", self) once the connection ends, so a stale
    connection's lines are told apart from the live one's."""

    def __init__(self, events: queue.Queue, verbose: bool):
        self.verbose = verbose
        self.sock = socket.create_connection((HOST, PORT), timeout=30)
        self.sock.settimeout(None)
        self.sent = time.monotonic()
        threading.Thread(target=self._reader, args=(events,), daemon=True).start()

    def _reader(self, events: queue.Queue) -> None:
        pending = b""
        try:
            while data := self.sock.recv(65536):
                *lines, pending = (pending + data).split(b"\n")
                for line in lines:
                    events.put(("server", self, line.decode("utf-8", "replace").rstrip("\r")))
        except OSError:
            pass
        events.put(("closed", self))

    def send(self, line: str) -> None:
        if self.verbose:
            say("> " + ("Login ****" if line.startswith("Login ") else line))
        try:
            self.sock.sendall((line + "\n").encode())
        except OSError:
            pass  # the reader sees the connection end and says so
        self.sent = time.monotonic()

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


@dataclass
class Live:
    """The game being played, as the server has told it."""
    number: str
    size: int
    halfkomi: int
    color: int
    white: str
    black: str
    base: int
    increment: int
    game: takref.Game
    clocks: list[float]  # milliseconds, white's then black's
    moves: list[str] = field(default_factory=list)
    comments: list[str] = field(default_factory=list)
    # From before a dropped connection, for the moves the server replays.
    earlier: list[str] = field(default_factory=list)
    over: bool = False


class Bot:
    def __init__(self, args: argparse.Namespace, password: str):
        self.args = args
        self.password = password
        self.events: queue.Queue = queue.Queue()
        self.server: Server | None = None
        self.engine = self.new_engine()
        self.name = ""
        self.live: Live | None = None
        self.resume: Live | None = None
        self.increment = args.inc
        self.increments: dict[str, int] = {}
        self.seeks: dict[str, re.Match] = {}
        self.thinker: threading.Thread | None = None
        self.searches = 0
        self.thinking: int | None = None
        self.failures = 0
        self.played = 0
        self.interrupts = 0
        self.noticed = 0

    # The engine.

    def new_engine(self) -> match.Engine:
        return match.Engine([self.args.engine], {"Hash": str(self.args.hash)})

    def restart_engine(self) -> None:
        self.engine.process.kill()
        self.engine = self.new_engine()
        if self.live:
            self.engine_game(self.live)

    def engine_game(self, live: Live) -> None:
        self.engine.send(f"setoption name HalfKomi value {live.halfkomi}")
        self.engine.send(f"teinewgame {live.size}")
        self.engine.wait_ready()

    def think(self, live: Live) -> None:
        self.searches += 1
        search, moves, color = self.searches, list(live.moves), live.color
        clocks = list(live.clocks)
        clocks[color] = max(clocks[color] - self.args.margin, 50.0)
        inc = live.increment * 1000
        go = f"go wtime {int(clocks[0])} btime {int(clocks[1])} winc {inc} binc {inc}"
        budget = clocks[color] / 1000

        def work() -> None:
            started = time.monotonic()
            comment = ""
            try:
                self.engine.send("position startpos" + (" moves " + " ".join(moves) if moves
                                                        else ""))
                self.engine.send(go)
                deadline = started + budget + match.HANG_SECONDS
                while True:
                    line = self.engine.read(deadline - time.monotonic())
                    if line.startswith("bestmove"):
                        words = line.split()
                        self.events.put(("bestmove", search, words[1] if len(words) > 1 else "",
                                         comment, time.monotonic() - started))
                        return
                    if line.startswith("info") and " score " in line:
                        comment = score_comment(line, color) or comment
            except match.EngineError as error:
                self.events.put(("engine", search, str(error)))

        self.thinking = search
        self.thinker = threading.Thread(target=work, daemon=True)
        self.thinker.start()

    def cancel(self) -> None:
        """Stops a search and waits for it, so that nothing else is reading the engine."""
        self.thinking = None
        if self.thinker and self.thinker.is_alive():
            try:
                self.engine.send("stop")
            except match.EngineError:
                pass
            self.thinker.join(5.0)
            if self.thinker.is_alive():
                say("the engine did not stop; restarting it")
                self.restart_engine()
        self.thinker = None

    def maybe_think(self) -> None:
        live = self.live
        # A burst of moves is the server replaying a game after a reconnection: let it finish.
        if live is None or live.over or self.thinking is not None or not self.events.empty():
            return
        if live.game.to_move == live.color:
            self.think(live)

    def on_bestmove(self, search: int, move: str, comment: str, elapsed: float) -> None:
        if search != self.thinking or self.live is None:
            return
        live = self.live
        self.cancel()
        self.failures = 0
        try:
            move = takref.canonical(move)
        except ValueError:
            pass
        if move not in live.game.legal_moves():
            say(f"the engine sent an illegal move, {move!r}; resigning")
            self.resign()
            return
        self.server.send(f"Game#{live.number} {server_move(move)}")
        self.play(move, f"{comment} {elapsed:.1f}s".strip())

    def on_engine_error(self, search: int, error: str) -> None:
        if search != self.thinking or self.live is None:
            return
        self.thinking = None
        self.failures += 1
        say(f"engine: {error}")
        if self.failures >= 3:
            say("the engine keeps failing; resigning and stopping")
            self.interrupts = max(self.interrupts, 1)
            self.resign()
            return
        self.restart_engine()

    # The game.

    def play(self, move: str, comment: str) -> None:
        live = self.live
        number = live.game.ply // 2 + 1
        mover = live.white if live.game.to_move == takref.WHITE else live.black
        live.game.play(move)
        live.moves.append(move)
        live.comments.append(comment)
        live.over = live.game.result() is not None
        dots = "." if live.game.to_move == takref.BLACK else "..."
        say(f"{number}{dots} {move:<8} {mover} {comment}")

    def start(self, found: re.Match) -> None:
        number, size, white, black, color, base, komi, stones, caps = found.groups()
        size, base, komi = int(size), int(base), int(komi)
        self.cancel()
        resume, self.resume = self.resume, None
        if resume and resume.number != number:
            resume = None
        increment = resume.increment if resume else self.increments.pop(number, self.increment)
        live = Live(number, size, komi, takref.WHITE if color == "white" else takref.BLACK,
                    white, black, base, increment, takref.Game(size, komi), [base * 1000.0] * 2,
                    earlier=resume.comments if resume else [])
        self.live = live
        say(f"game {number}: {white} vs {black}, {size}x{size}, {base}+{increment}, "
            f"komi {komi / 2:g}" + (", resumed" if resume else ""))
        # PlayTak has no link to one game: its client reads nothing from the URL.
        say(f"watch at https://playtak.com, game {number} in the games in progress")
        if size not in takref.PIECES or (int(stones), int(caps)) != takref.PIECES[size]:
            say(f"{stones} stones and {caps} capstones are not the usual reserve; resigning")
            self.resign()
            return
        try:
            self.engine_game(live)
        except match.EngineError as error:
            say(f"engine: {error}; restarting it")
            self.restart_engine()

    def on_game(self, rest: str) -> None:
        live = self.live
        word = rest.split(" ", 1)[0]
        if word in ("P", "M"):
            self.cancel()
            move = playtak.move_ptn(rest)
            if move is None or takref.canonical(move) not in live.game.legal_moves():
                say(f"takref refuses the server's move {rest!r}; resigning")
                self.resign()
                return
            index = len(live.moves)
            self.play(takref.canonical(move),
                      live.earlier[index] if index < len(live.earlier) else "")
        elif word == "Time":
            live.clocks = [float(t) * 1000 for t in rest.split()[1:3]]
        elif word == "Timems":
            live.clocks = [float(t) for t in rest.split()[1:3]]
        elif word == "Over":
            self.finish(rest.split()[1])
        elif word.startswith("Abandoned"):
            self.finish("1-0" if live.color == takref.WHITE else "0-1", "the other side quit")
        elif word == "OfferDraw":
            say("a draw is offered, and declined by ignoring it")
        elif word == "RequestUndo":
            say("an undo is asked for, and declined by ignoring it")

    def resign(self) -> None:
        live = self.live
        self.server.send(f"Game#{live.number} Resign")
        self.finish("0-1" if live.color == takref.WHITE else "1-0", "resigned")

    def finish(self, result: str, reason: str = "") -> None:
        live = self.live
        self.cancel()
        self.live = None
        self.played += 1
        if result == "0-0":
            say(f"game {live.number} aborted")
        else:
            white_score = match.GameResult(result, "").score_for_white()
            mine = white_score if live.color == takref.WHITE else 1.0 - white_score
            verdict = {1.0: "won", 0.5: "drawn", 0.0: "lost"}[mine]
            say(f"game {live.number} {verdict}: {result}" + (f", {reason}" if reason else ""))
            if live.moves:
                self.save(live, result, reason)
        if not self.stopping():
            self.offer()

    def save(self, live: Live, result: str, reason: str) -> None:
        PTN.parent.mkdir(parents=True, exist_ok=True)
        finished = match.GameResult(result, reason, live.moves, live.comments)
        # The round is the server's game number, which finds the game in its history.
        match.write_ptn(PTN, finished, live.white, live.black, live.size, live.halfkomi,
                        f"{live.base}+{live.increment}", int(live.number), site="PlayTak")
        last = ptnlink.split_games(PTN.read_text(encoding="utf-8"))[-1]
        say("https://ptn.ninja/" + urllib.parse.quote(last, safe=""))

    # The lobby.

    def standard(self, found: re.Match) -> bool:
        size, stones, caps = int(found.group(3)), int(found.group(8)), int(found.group(9))
        return size in takref.PIECES and (stones, caps) == takref.PIECES[size]

    def acceptable(self, found: re.Match) -> bool:
        a = self.args
        return (found.group(2).lower() == a.accept.lower() and int(found.group(3)) == a.size
                and self.standard(found) and (a.rated or found.group(10) == "1")
                and found.group(11) == "0" and found.group(14) in ("", self.name))

    def offer(self) -> None:
        a = self.args
        if a.accept:
            for number, found in list(self.seeks.items()):
                if self.acceptable(found):
                    self.take(number, found)
                    return
            say(f"waiting for a seek from {a.accept}")
            return
        stones, caps = takref.PIECES[a.size]
        self.increment = a.inc
        # The trailing field is the opponent, and the space before it must be sent even when it
        # is empty: without it the server reads the extra-time fields as an older form's.
        self.server.send(f"Seek {a.size} {a.base} {a.inc} {a.color} {a.halfkomi} {stones} {caps} "
                         f"{0 if a.rated else 1} 0 0 0 {a.opponent}")
        say(f"seeking {a.size}x{a.size}, {a.base}+{a.inc}, komi {a.halfkomi / 2:g}, "
            f"{'rated' if a.rated else 'unrated'}" + (f", for {a.opponent}" if a.opponent else ""))

    def take(self, number: str, found: re.Match) -> None:
        del self.seeks[number]
        self.increment = int(found.group(5))
        self.server.send(f"Accept {number}")
        say(f"accepting {found.group(2)}'s seek {number}")

    def on_line(self, line: str) -> None:
        if self.args.verbose and line != "OK":
            say("< " + line)
        if line.startswith("Game#"):
            number, _, rest = line[5:].partition(" ")
            if self.live and number == self.live.number:
                self.on_game(rest)
        elif line.startswith("Game Start"):
            found = GAME_START.match(line)
            if found:
                self.start(found)
        elif line.startswith("Seek new"):
            found = SEEK_NEW.match(line)
            if found:
                self.seeks[found.group(1)] = found
                if self.args.accept and self.live is None and not self.stopping() and \
                        self.acceptable(found):
                    self.take(found.group(1), found)
        elif line.startswith("Seek remove"):
            self.seeks.pop(line.split()[2], None)
        elif line.startswith("GameList Add"):
            words = line.split()
            if self.name in (words[3], words[4]):
                self.increments[words[2]] = int(words[7])
                if self.live and self.live.number == words[2]:
                    self.live.increment = int(words[7])
        elif line.startswith(("Tell ", "Message ", "Error", "NOK")) and not self.args.verbose:
            say(line)
        elif WELCOME.match(line) and not self.name:
            self.name = WELCOME.match(line).group(1)
            say(f"logged in as {self.name}")
            self.offer()

    # The connection.

    def stopping(self) -> bool:
        return self.interrupts > 0 or 0 < self.args.games <= self.played

    def session(self) -> str:
        """Plays until done, the connection drops, or the login is refused."""
        server = self.server
        self.name = ""
        server.send("Protocol 1")  # clocks in milliseconds
        server.send("Client Yapper")
        server.send("Login Guest" if self.args.guest else
                    f"Login {self.args.user} {self.password}")
        heard = time.monotonic()
        while True:
            if self.interrupts != self.noticed:
                self.noticed = self.interrupts
                if self.interrupts == 1 and self.live:
                    say("finishing this game; Ctrl+C again quits now and abandons it")
            if self.interrupts > 1 or (self.stopping() and self.live is None):
                return "done"
            now = time.monotonic()
            if now - server.sent > PING_SECONDS:
                server.send("PING")
            if now - heard > 3 * PING_SECONDS:
                return "dropped"
            try:
                event = self.events.get(timeout=0.5)
            except queue.Empty:
                self.maybe_think()
                continue
            if event[0] == "server" and event[1] is server:
                heard = now
                if event[2] == "Authentication failure":
                    say("PlayTak refused the login")
                    return "refused"
                self.on_line(event[2])
            elif event[0] == "closed" and event[1] is server:
                return "dropped"
            elif event[0] == "bestmove":
                self.on_bestmove(*event[1:])
            elif event[0] == "engine":
                self.on_engine_error(*event[1:])
            self.maybe_think()

    def interrupt(self, signum, frame) -> None:
        # Only counted here: the loop acts on it, between events rather than inside one.
        self.interrupts += 1

    def run(self) -> int:
        signal.signal(signal.SIGINT, self.interrupt)
        outcome = "dropped"
        while True:
            try:
                self.server = Server(self.events, self.args.verbose)
            except OSError as error:
                say(f"cannot reach {HOST}:{PORT}: {error}")
            else:
                outcome = self.session()
                if outcome != "dropped":
                    break
                self.server.close()
                say("the connection dropped")
            self.cancel()
            self.resume, self.live = self.live or self.resume, None
            if self.interrupts:
                break
            say(f"trying again in {RETRY_SECONDS:g} seconds")
            deadline = time.monotonic() + RETRY_SECONDS
            while time.monotonic() < deadline and not self.interrupts:
                time.sleep(0.2)
        if self.server:
            self.server.send("quit")
            self.server.close()
        self.cancel()
        self.engine.close()
        say(f"{self.played} games played")
        return 1 if outcome == "refused" else 0


def main() -> int:
    default_engine = "./yapper.exe" if os.name == "nt" else "./yapper"
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    who = parser.add_mutually_exclusive_group(required=True)
    who.add_argument("--user", help="the account; its password is PLAYTAK_PASSWORD, or asked for")
    who.add_argument("--guest", action="store_true", help="play as a guest, for testing")
    parser.add_argument("--engine", default=default_engine)
    parser.add_argument("--hash", type=int, default=256, help="megabytes")
    parser.add_argument("--size", type=int, default=6, choices=range(3, 9))
    parser.add_argument("--tc", default="600+20", help="seconds+increment, whole seconds")
    parser.add_argument("--halfkomi", type=int, default=4, choices=range(0, 9),
                        help="komi in half flats, for black; PlayTak's usual 6x6 game has 4")
    parser.add_argument("--color", choices=["A", "W", "B"], default="A", help="A is either")
    parser.add_argument("--rated", action="store_true",
                        help="rated games, once PlayTak has marked the account as a bot")
    parser.add_argument("--opponent", default="", help="only this player may take the seek")
    parser.add_argument("--accept", default="", help="take this player's seeks instead")
    parser.add_argument("--games", type=int, default=0, help="stop after this many; 0 never")
    parser.add_argument("--margin", type=int, default=1000,
                        help="milliseconds kept off the engine's own clock, for the network")
    parser.add_argument("--verbose", action="store_true", help="print everything sent and read")
    args = parser.parse_args()

    base, inc = match.parse_tc(args.tc)
    if base != int(base) or inc != int(inc):
        parser.error("the server takes whole seconds")
    args.base, args.inc = int(base), int(inc)
    if args.opponent and args.accept:
        parser.error("--opponent is for a seek of our own, --accept for someone else's")
    if not Path(args.engine).exists() and not Path(args.engine + ".exe").exists():
        parser.error(f"no engine at {args.engine}: run make first")

    password = ""
    if args.user:
        password = os.environ.get("PLAYTAK_PASSWORD") or getpass.getpass(
            f"PlayTak password for {args.user}: ")
    return Bot(args, password).run()


if __name__ == "__main__":
    sys.exit(main())
