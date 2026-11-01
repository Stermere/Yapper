"""
takref.py - the rules of Tak, written to be obviously correct rather than fast.

This is the second opinion on src/. It shares nothing with the engine on purpose: no
bitboards, no patterns, no tables. A stack is a list of stones, a spread is a recursive
walk along a ray, a road is a breadth-first search. The rules are transcribed from
https://ustak.org/play-beautiful-game-tak/ and each one sits in the function that applies
it, so it can be read against that page.

It is the oracle for tools/perftdiff.py and the referee in tools/match.py. Do not optimise
it: every clever thing here is one more thing the two implementations could get wrong in
the same way.

Stdlib only.
"""

from __future__ import annotations

import re
from dataclasses import dataclass

WHITE, BLACK = 0, 1
FLAT, WALL, CAP = "F", "S", "C"

# Board size -> (stones, capstones) per player. 3x3 is not in the USTAK table but is
# standard PTN, and small boards reach the end-of-game rules quickly.
PIECES = {3: (10, 0), 4: (15, 0), 5: (21, 1), 6: (30, 1), 7: (40, 2), 8: (50, 2)}

STEPS = {"+": (1, 0), ">": (0, 1), "-": (-1, 0), "<": (0, -1)}

MOVE_RE = re.compile(r"^([1-8]?)([FSC]?)([a-h])([1-8])([<>+-]?)([1-8]*)\*?['\"!?]*$")


@dataclass(frozen=True)
class Stone:
    color: int
    kind: str


def square_name(rank: int, file: int) -> str:
    return "abcdefgh"[file] + str(rank + 1)


class Game:
    def __init__(self, size: int = 6, halfkomi: int = 0):
        self.size = size
        self.halfkomi = halfkomi
        self.ply = 0
        # board[rank][file] is a stack, bottom stone first.
        self.board: list[list[list[Stone]]] = [[[] for _ in range(size)] for _ in range(size)]
        stones, capstones = PIECES[size]
        self.stones = [stones, stones]
        self.capstones = [capstones, capstones]

    @property
    def to_move(self) -> int:
        return self.ply % 2

    def copy(self) -> "Game":
        other = Game.__new__(Game)
        other.size = self.size
        other.halfkomi = self.halfkomi
        other.ply = self.ply
        other.board = [[list(stack) for stack in rank] for rank in self.board]
        other.stones = list(self.stones)
        other.capstones = list(self.capstones)
        return other

    # ------------------------------------------------------------------ moves --

    def legal_moves(self) -> list[str]:
        moves = []
        me = self.to_move
        for rank in range(self.size):
            for file in range(self.size):
                name = square_name(rank, file)
                stack = self.board[rank][file]
                if not stack:
                    # Each player's first turn places one opposing flat stone.
                    if self.ply < 2:
                        moves.append(name)
                        continue
                    if self.stones[me] > 0:
                        moves.append(name)
                        moves.append("S" + name)
                    if self.capstones[me] > 0:
                        moves.append("C" + name)
                elif self.ply >= 2 and stack[-1].color == me:
                    moves.extend(self._spreads(rank, file, stack))
        return moves

    def _spreads(self, rank: int, file: int, stack: list[Stone]) -> list[str]:
        moves = []
        name = square_name(rank, file)
        # You may pick up at most as many stones as the board is wide.
        for carry in range(1, min(len(stack), self.size) + 1):
            top = stack[-1].kind
            for arrow, step in STEPS.items():
                for drops in self._drop_sequences(rank, file, step, carry, top):
                    prefix = str(carry) if carry > 1 else ""
                    suffix = "".join(map(str, drops)) if len(drops) > 1 else ""
                    moves.append(prefix + name + arrow + suffix)
        return moves

    def _drop_sequences(self, rank, file, step, carried, top) -> list[list[int]]:
        """Every way to drop `carried` stones along a ray, at least one on each square."""
        rank += step[0]
        file += step[1]
        if not (0 <= rank < self.size and 0 <= file < self.size):
            return []
        target = self.board[rank][file]
        target_kind = target[-1].kind if target else None
        if target_kind == CAP:
            return []
        if target_kind == WALL:
            # The capstone, by itself, may drop onto a standing stone at the end of a move.
            return [[1]] if top == CAP and carried == 1 else []
        sequences = [[carried]]
        for drop in range(1, carried):
            for rest in self._drop_sequences(rank, file, step, carried - drop, top):
                sequences.append([drop] + rest)
        return sequences

    def play(self, move: str) -> None:
        """Applies a move, which must be legal. Accepts any spelling of it."""
        move = canonical(move)
        me = self.to_move
        match = MOVE_RE.match(move)
        count, kind, file_char, rank_char, arrow, drops = match.groups()
        file = "abcdefgh".index(file_char)
        rank = int(rank_char) - 1

        if not arrow:
            kind = kind or FLAT
            # On the first turn the stone belongs to the opponent.
            owner = 1 - me if self.ply < 2 else me
            if kind == CAP:
                self.capstones[owner] -= 1
            else:
                self.stones[owner] -= 1
            self.board[rank][file].append(Stone(owner, kind))
        else:
            carry = int(count or 1)
            drops = [int(d) for d in drops] or [carry]
            stack = self.board[rank][file]
            carried = stack[-carry:]
            del stack[-carry:]
            step = STEPS[arrow]
            for drop in drops:
                rank += step[0]
                file += step[1]
                target = self.board[rank][file]
                if target and target[-1].kind == WALL:
                    # Flattened: it stays where it is, as a flat of the same colour.
                    target[-1] = Stone(target[-1].color, FLAT)
                target.extend(carried[:drop])
                carried = carried[drop:]
        self.ply += 1

    # ------------------------------------------------------------------- ends --

    def _has_road(self, color: int) -> bool:
        def counts(rank: int, file: int) -> bool:
            stack = self.board[rank][file]
            return bool(stack) and stack[-1].color == color and stack[-1].kind != WALL

        n = self.size
        crossings = [
            ([(r, 0) for r in range(n)], lambda r, f: f == n - 1),
            ([(0, f) for f in range(n)], lambda r, f: r == n - 1),
        ]
        for starts, reached in crossings:
            seen = set()
            frontier = [s for s in starts if counts(*s)]
            while frontier:
                rank, file = frontier.pop()
                if (rank, file) in seen:
                    continue
                seen.add((rank, file))
                if reached(rank, file):
                    return True
                for dr, df in STEPS.values():
                    r, f = rank + dr, file + df
                    if 0 <= r < n and 0 <= f < n and counts(r, f):
                        frontier.append((r, f))
        return False

    def flat_counts(self) -> tuple[int, int]:
        counts = [0, 0]
        for rank in self.board:
            for stack in rank:
                if stack and stack[-1].kind == FLAT:
                    counts[stack[-1].color] += 1
        return counts[WHITE], counts[BLACK]

    def result(self) -> str | None:
        """R-0, 0-R, F-0, 0-F, 1/2-1/2, or None while the game goes on."""
        mover = 1 - self.to_move
        # If a move makes a road for both players, the player who made the move wins.
        for color in (mover, 1 - mover):
            if self._has_road(color):
                return "R-0" if color == WHITE else "0-R"

        full = all(stack for rank in self.board for stack in rank)
        out = any(self.stones[c] + self.capstones[c] == 0 for c in (WHITE, BLACK))
        if not (full or out):
            return None
        white, black = self.flat_counts()
        lead = 2 * (white - black) - self.halfkomi
        return "F-0" if lead > 0 else "0-F" if lead < 0 else "1/2-1/2"

    # -------------------------------------------------------------------- tps --

    def tps(self) -> str:
        ranks = []
        for rank in range(self.size - 1, -1, -1):
            cells: list[str] = []
            run = 0
            for stack in self.board[rank]:
                if not stack:
                    run += 1
                    continue
                if run:
                    cells.append("x" + (str(run) if run > 1 else ""))
                    run = 0
                text = "".join(str(s.color + 1) for s in stack)
                cells.append(text + (stack[-1].kind if stack[-1].kind != FLAT else ""))
            if run:
                cells.append("x" + (str(run) if run > 1 else ""))
            ranks.append(",".join(cells))
        return "/".join(ranks) + f" {self.to_move + 1} {self.ply // 2 + 1}"

    def key(self) -> str:
        """Identifies the position for repetition: the board and the side to move."""
        return self.tps().rsplit(" ", 1)[0]

    @classmethod
    def from_tps(cls, tps: str, halfkomi: int = 0) -> "Game":
        board_text, player, move = tps.split()
        rows = board_text.split("/")
        game = cls(len(rows), halfkomi)
        game.ply = (int(move) - 1) * 2 + int(player) - 1
        for row_index, row in enumerate(rows):
            rank = game.size - 1 - row_index
            file = 0
            for cell in row.split(","):
                if cell.startswith("x"):
                    file += int(cell[1:] or 1)
                    continue
                kind = cell[-1] if cell[-1] in "SC" else FLAT
                colors = cell.rstrip("SC")
                stack = [Stone(int(c) - 1, FLAT) for c in colors]
                stack[-1] = Stone(stack[-1].color, kind)
                game.board[rank][file] = stack
                file += 1
                for stone in stack:
                    if stone.kind == CAP:
                        game.capstones[stone.color] -= 1
                    else:
                        game.stones[stone.color] -= 1
        return game


def canonical(move: str) -> str:
    """The one spelling of a move that legal_moves() uses."""
    match = MOVE_RE.match(move.strip())
    if not match:
        raise ValueError(f"not a PTN move: {move!r}")
    count, kind, file, rank, arrow, drops = match.groups()
    if not arrow:
        if count or drops:
            raise ValueError(f"not a PTN move: {move!r}")
        return ("" if kind in ("", FLAT) else kind) + file + rank
    if kind:
        raise ValueError(f"not a PTN move: {move!r}")
    carry = int(count or 1)
    hops = [int(d) for d in drops] or [carry]
    if sum(hops) != carry:
        raise ValueError(f"drops do not add up: {move!r}")
    prefix = str(carry) if carry > 1 else ""
    return prefix + file + rank + arrow + ("".join(map(str, hops)) if len(hops) > 1 else "")


def perft(game: Game, depth: int) -> int:
    """Leaves at exactly `depth`: a game that ended early contributes none."""
    if depth == 0:
        return 1
    if game.result() is not None:
        return 0
    moves = game.legal_moves()
    if depth == 1:
        return len(moves)
    total = 0
    for move in moves:
        child = game.copy()
        child.play(move)
        total += perft(child, depth - 1)
    return total
