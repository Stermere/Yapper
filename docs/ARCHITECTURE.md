# Architecture

## The board

**One layout for every size.** A square is `rank * 8 + file` on all boards, so shifts are
constants, and a size is only a mask and a carry limit. One binary plays all sizes through one
code path with no per-size specialisation.

**A stack is a shift register with its height built in.** `stones[sq]` holds one bit per stone
with bit 0 on top, and a 1 above the bottom stone, so an empty square holds 1. Taking `k` stones
is a mask and a shift, landing `d` is a shift and an OR, and there is no height to keep in step
with either.

**Towers cost nothing until one exists.** A word can hold 63 stones and `deep[sq]` carries the
rest, for the contrived stacks only 7x7 and 8x8 can reach. With no stack of 47 stones on the
board, none can overflow in one spread, so a spread does not have to ask. `towers` in the state
says when it does, and `spread()` is built twice from one body: the build for towers makes calls
in the middle of its loop, and the common one makes none and has no question to ask at any hop.

**Only the top stone has a type.** Nothing lands on a wall or capstone without flattening it,
so everything below the top is a flat. `walls` and `caps` are bitboards and need no per-stack
storage.

**Moves are 16 bits**: square, direction, and an 8-bit pattern where bit `i` ends a hop after
stone `i`. Carry count, hop count and drop sizes are all read off with bit operations, and the
255 patterns are exactly the possible spreads under a carry limit of 8.

## Why it is fast

Which spreads a stack has depends on just two numbers: how much it may carry and how far is
open. So `movegen_init()` builds one list of pre-shifted patterns per pair, and generating a
direction is a table lookup plus an OR over a run of u16s. The vectors are GCC's own, so they
lower to SSE, AVX or NEON with no intrinsics in the source. A lane always copies eight moves and
the list then advances by what was meant, so a short run has no loop to mispredict.

Placements work the same way, a rank at a time: the byte of empty squares picks a ready-made list
of moves, and adding the rank's first square makes them real. A rank costs the same with one
empty square as with eight. Lone stones go the same way, a direction and then a rank at a time,
so the per-stack path only ever sees real stacks and capstones. That is not square order, and
search sees the lone stones' moves first.

Reach comes from a table of every line of eight squares, indexed by the byte of blockers on the
line, so a stack costs two lookups rather than four bit scans. A file becomes a byte with one
multiply.

Counting needs no list. Most stones on a board are alone, and a lone stone has one move per open
neighbour, so all of them are counted at once with four shifts and popcounts; `tall` says which
stacks are left to walk, and those cost a table read per line. That is why bulk perft runs at
several billion leaves a second.

The road check is what usually dominates a Tak node, and it barely runs here. A road must cross
from every rank to the next, or from every file to the next, along a pair of adjacent stones, and
`road_conceivable()` tests that in a dozen operations and no branches. A flood only follows if it
passes, and then only from squares the move changed, inside the player's own stones.

Make and unmake split the board into two parts. The 64 bytes of `PositionState` are saved once
for a node, and restored by one copy after each of its moves, which is also what recovers a
flattened wall with no special case. Only the stacks are walked back, and that is a single shift
per hop. Saving per move would be slower than it looks, because each copy would read what the one
before it wrote.

A lone flat or wall stepping one square has a make and unmake of its own. It has no hops to walk
and no pattern to read, and its new owner is simply the side to move. The generator lists these
together, so the branch that picks the path predicts well, and `board_play()` takes it too, so
perft and search time the same code.

The hash is a sum: each stack's whole word times a key for its square, plus a key for each wall
and capstone. A spread therefore costs one multiply for each square it touches, however many
stones move, and needs no table spanning every height. A sum is weak in its low bits, so the
transposition table mixes it before reading them; everything else compares all 64 bits. The side
to move is not in it, or anywhere: white moves on even plies, so `board_stm()` reads the ply, and
`board_key()` adds the side only for the one reader that needs it. Make ends with `ply++`.

**Register pressure is the real enemy.** A spread has around fifteen values live, and what a
compiler does when it runs out shows up directly: an early version spent 290 instructions on a
1-hop spread, most of them spills and reloads. Hence no calls inside the fast spread, owners
worked out in registers and written once, and the rare paths out of line. `perft_node` is
flattened so a node saves registers once rather than at every call into the board, and has a loop
for each kind of move. An `Undo` that never escapes can stay in registers across all of a node's
moves, and clang does exactly that, which is why it is used when it is there and why the sources
are built as one translation unit.

## Tactics

`board_road_placements()` answers "where can this side finish a road by placing a stone?" in
four floods with no move generation: flood from each edge, then a square that touches both ends
of a crossing wins. The search uses it as a win-in-one test at every node, and as a threat
detector the leaf search follows: a leaf with a road hanging is not quiet, so it plays out the
replies that stop the road instead of standing pat.

## The search

Iterative deepening PVS with aspiration windows, a bucketed transposition table, null move,
reverse futility, late move reductions and pruning, killers and history. All of it is the
textbook skeleton and none of its margins have been measured. It runs on one worker thread, and
every piece of per-search state lives in `Worker`, so Lazy SMP is a matter of having more of
them.

## The evaluation

`evaluate()` is linear: how much of each term a position has, times what that term is worth. The
terms are in `eval.h` and the weights in `weights.h`, which a fit writes. Each weight has a value
for the opening and for the end of the game, packed into one word so a term costs one add, and
the two are blended once at the end by how many stones the game has left. That is the smaller of
what either side has left and the empty squares, since a game ends on whichever comes first.

There is one body, built twice like `spread()`. Play adds up the score, and a trace also records
each term's count, so a fit and the engine cannot read a position differently. Selftest holds the
two equal.

Scores stay in hundredths of a flat, because the search's margins are written in them. It is a
stepping stone: networks will replace it, and nothing outside `eval.c` knows what kind of
evaluation is behind `evaluate()`.

## Training data

`docs/DATA.md` has the format and the tools. A game on disk is its moves and the search's scores,
at four bytes a ply, and replay gives every position back, so data never depends on the evaluation
that is being fitted to it.
