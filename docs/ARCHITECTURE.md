# Architecture

## The board

**One layout for every size.** A square is `rank * 8 + file` on all boards, so shifts are
constants, and a size is only a mask and a carry limit. One binary plays all sizes through one
code path with no per-size specialisation.

**A stack is a shift register.** `stones[sq]` holds one bit per stone with bit 0 on top, so
taking `k` stones is a mask and a shift, and landing `d` is a shift and an OR. Height lives
apart in `height[sq]`, which fits in one cache line for the whole board. A stack passes 64
stones only on 7x7 and 8x8, and only contrived ones; `deep[sq]` carries the overflow behind an
unlikely branch at the far end of the struct, so it costs nothing in play and the board is
still correct for the tallest legal stack.

**Only the top stone has a type.** Nothing lands on a wall or capstone without flattening it,
so everything below the top is a flat. `walls` and `caps` are bitboards and need no per-stack
storage.

**Moves are 16 bits**: square, direction, and an 8-bit pattern where bit `i` ends a hop after
stone `i`. Carry count, hop count and drop sizes are all read off with bit operations, and the
255 patterns are exactly the possible spreads under a carry limit of 8.

## Why it is fast

Which spreads a stack has depends on just two numbers: how much it may carry and how far is
open. So `movegen_init()` builds one list of pre-shifted patterns per pair, and generating a
direction is a table lookup plus an OR over a run of u16s, which compilers vectorise for AVX2
and NEON alike. Counting is the same lookup with no list at all, which is why bulk perft runs
above a billion leaves a second.

The road check is what usually dominates a Tak node, and it is local here. A road can only
appear through a square the move changed, so `board_outcome_after()` floods from those squares
alone, inside only the player's own stones, and a small group stops after a round or two. A
position where the move made no new road stone does no flood at all.

Make saves four bitboards and the hash, and unmake restores them rather than undoing them. That
also recovers a flattened wall with no special case. Only the stacks are walked back.

The hash treats a square's whole stack as one value, so a spread costs per square touched
rather than per stone moved, and no table has to span every height a stack can reach.

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

`evaluate()` is a handful of hand-set terms: flats, walls, capstones, stack contents, group
spans, road threats. It exists so games can be played and is expected to be replaced.
