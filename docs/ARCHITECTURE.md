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

`movegen_road_spread()` answers the same question for a spread, which is how most roads are
finished. A road's first new square has to touch the group at one edge and its last the group at
the other, and a spread's squares are one line, so the floods above rule out every line that does
not touch both. For what survives, the stones each pattern leaves on top are read off the stack's
bits, with nothing played. It is still the dearest test a node has, so it comes after the prunes:
a position already failing high can only be confirmed by finding a win.

## The search

Iterative deepening PVS with aspiration windows, a bucketed transposition table, null move,
ProbCut, reverse futility, late move reductions and pruning, and singular extensions. Reductions
are a ply stronger at a node expected to cut, and move-count pruning comes sooner when the side to
move's evaluation has fallen since its last move. The null move does little here: a pass gives up
a placement, 450 on the evaluation's scale, so its search holds at a tenth of the nodes it is tried
at, E74. So a node the
evaluation puts above beta is first searched four plies shallower against a bound past beta, and
cut if even that holds. It is right about 99% of the time, saves more the deeper the search, and
from depth 7 passed only on a clock long enough to reach that depth, E112. The table move and the
killers are tried
before a move list exists, because most nodes cut on one of them. The rest go by history of the
whole move, so each spread a stack can make keeps its own; a second history, of what answered the
move before, measured neutral beside the prior and went, E65. On top of that goes a prior. From
depth 4, `PriorDepth`, it is `policy.c`'s, a small network fitted by `yapper policyfit` to the moves the search
chose in self-play. A move's features, all read the same under the board's symmetries and from
either side, say what a placement touches and leaves for the roads, how many placements each
side's best road through its square still needs, what a spread takes, covers and gives up, and how
either answers the last two moves. Each indexes a row of 16 lanes, summed onto the units' biases:
15 are clipped units weighed into the prior and the last is linear, E52. Features of the position
alone, square classes and lines of road stones fitted a little better and played the same, and
went in E65. A node scores its moves at once, so what they share, a
placement's square, a spread's source and carry and the squares it lands on, is summed once.
History is cut back at the start of every search, to three quarters by `HistoryKeep`, so the prior
leads until the search has evidence of its own, which pays only with the network prior, E52. A node
nearer the leaves cannot pay for scoring every move that way, E100, and there the prior is set by
hand: a flat or capstone that leaves
a road to finish next move, one on a square where the other side would leave one, and a spread by
the tops it takes less those it gives up, read off the stack's bits, E50.

The evaluation the pruning reads is corrected by what the search has learned of its error, the
correction history of chess engines with keys taken from a measurement. A move leaves the same
thing on the board wherever in the tree it is played, and the evaluation misses it each time:
within one search the errors of two nodes reached by the same move correlate at 0.34. So for each
side to move the search keeps what its finished nodes found beyond the evaluation, in a slot for
the move that led to the node and one for the mover's own move before it, fitted on top of one slot
for the side, and reverse futility, the tests of ProbCut and the null move, and the improving flag
read the evaluation with the two move slots added. The side's own slot is fitted and never read: read at every node it
feeds on itself, one side's up and the other's down, and lost 190. The table and the evaluation
cache keep the network's own word, and the slots carry from move to move of a game: E82.

A node answering a road plays only what can stop it, which `movegen_road_answer()` works out from
the squares the road would be placed on: a placement on the one such square, a spread that lands on
each of them or on a stone of a group each would join, and no placement at all when there are two.
Anything else leaves a road to be placed, which selftest holds for every move it turns away. The
third such reply and those after it are searched a ply shallower: never reduced before, their
subtrees grew to nearly half of a deep search, E108, and the cut passed at 30+0.3, E109.

The clock is shared out over the moves the game has left, worked out from the shorter reserve, so
it runs down towards the end of the game rather than being spent early and left over. Moving time
towards the end passed where moving it towards the opening did not: EXPERIMENTS.md E22 and E25.
Within a move it goes by what the search has found. No iteration starts past a threshold, and the
threshold is latest when the best move has just changed and comes down a step for each iteration
it holds, with a little more after a fall in the score: another iteration names another move a
third of the time in the first case and one time in seventeen once a move has held for seven, E76.
The steps are tuned with the rest, E102.

The tree's shape is still the textbook one. Treating spreads as threats, sparing moves that
threaten a road from reductions, and the improving flag in reverse futility and reductions were
tested and dropped, E23 and E36, and reverse futility's margin was measured and left alone, E19.
A prover for forced roads, pruning spreads that leave the same tops as one that failed, and a leaf
that answers a road by spread with only the moves that could stop it all lost, E50.
The aspiration window has not been measured. Every number the search plays by, its margins,
depths, reductions, history, prior weight, hand-set order, the evaluation's price for a road
threat and the clock, is a line in `src/tune.h`, which `tools/tune.py` can reach in a
`make TUNE=on` build. The search runs on one worker thread, and every piece of
per-search state lives in `Worker`, so Lazy SMP is a matter of having more of them.

## The evaluation

Every size is played by a network of its own, below, and `evaluate()` adds the one thing a fit
cannot read: what a road threat is worth, set by hand, since no position a fit reads has a road
hanging.
Scores are in hundredths of a flat, because the search's margins are written in them.

A size with no network counts flats. Nothing ships like that. It is there so that a
build whose network has changed shape, and so has no weights that fit it, can still build the
trainer that fits new ones from the games on disk.

A linear evaluation of 40 hand-picked terms came first, fitted to its own games, and it taught
the first networks: EXPERIMENTS.md E15 to E32. Every size's network beat it, and it was deleted,
E38. Those were a network per size, and one network shared by every size then beat them on every
size it could be measured on, E39. Each size has its own weights again since E119, in the shared
network's shape, so that a size can be fitted for itself without moving the others.

## The network

**A stack is one input.** An occupied square is a token: its top stone's type and the colours
of its top six stones, which are the low bits of the stack's word. Six is the carry limit of
6x6, and 98.6% of stacks are no taller. Encoding each stone by its depth instead would renumber
every stone under a stack whenever one lands on it, so a move would touch more rows for less.
A square's token changes only when a move touches the square, so a move costs at most two rows
per square, and a placement one.

**Each side has a view of its own.** Colours are folded so the top stone is its owner's, and each
side's view reads a stack as its own or as the other side's, with weights for each. The
accumulator holds white's view and then black's, and the output weighs the side to move's view
apart from the other. Both views are the same weights, so swapping the colours and the side to
move only swaps the views. A stack's row holds both halves, its owner's view first, and a black
top sends them to the other halves of the accumulator, so the table is the size one view of the
same width would need. One view, which a black top subtracted from, could only weigh the
difference between the sides: never a total, such as how full a line is, and a wall of either
side's always counted as minus the other's, though only one of them blocks. Two views of 64
filters beat one of 128 at the same cost by +88 in self-play, and by 57 and 103 against two other
engines: EXPERIMENTS.md E44.

**The board's symmetries are in the weights.** The 1024 hidden units are 64 filters in each view,
each seen through all eight rotations and reflections of the board, and the output weighs the
eight alike.
Turning a position only permutes each filter's units, so the score cannot change, and selftest
holds that exactly, for any weights. Training stores each filter once. That is an eighth of the
weights to fit, with no augmentation, and the engine spreads them over the symmetries as it
loads them.

**Nothing in make or unmake knows about it.** The search keeps a frame per ply, filled only when
that node is evaluated. A table hit skips the evaluation, so a frame comes from the nearest
ancestor that has one, through the squares that the moves on the path since then have changed,
and the search already records those moves. Selftest checks a frame carried this way against a
refresh, bit for bit.

**What is not stacks goes around it.** Output weights come in eight buckets by stones left,
finer near the end, where what a flat is worth climbs steeply. That is the smaller of what
either side has left and the empty squares, since a game ends on whichever comes first. A linear
skip adds the flat lead with komi, the reserves and komi itself, which are what a game is finally
counted on.

**A head reads the pools.** Each filter's eight units, squared, are pooled into one in each view,
so nothing past the pooling can tell how the board is turned. Two small layers read the 128 pools,
16 clipped units and then 32, the first with the lead, reserves and komi too, and the output adds
the 32 to the straight sum of the pools. Each size and bucket has a head of its own. It costs a
fifth of the search's speed and played +54 in self-play, +86 at equal nodes: EXPERIMENTS.md E48.

**It is integer.** Activations are clipped to [0, 1], where 1 is 255, and squared; each square is
cut to 16 bits before it is pooled, so the pools are summed in 16 bits. Every layer of the head
is 16-bit inputs times 16-bit weights summed in 32 bits, and each layer's units are cut back to 16
bits before the next reads them. A size's header gives its arrays no length, and the engine checks
the lengths their values give against its own shape, so a build of another shape will not compile
with it: given a length, C would fill the missing weights with zeros and the engine would play on
them. Every build scores
alike, so bench is the same from clang, gcc and a build with no CPU assumptions. `nntrain` fits
on the same rounded weights the engine runs, so the fit is what plays.

**What a node's evaluation costs.** Bringing the accumulator up to date cost three times what
scoring it did, a third of the search's time, E43; with the head, scoring costs about as much
again. All of a carry's rows are applied in one pass, a slice of the accumulator at a time held in
vector registers, from the ancestor's accumulator straight into the node's. The head's loops are
written for what the compiler makes of them: each part was timed alone, and the obvious forms ran
two to four times slower, E48.
Its two layers are kept by pairs of inputs and summed with the multiply-add of adjacent lanes, a
register of units at a time, with no sum across a register. No compiler found that from portable
code, so x86 is asked for it by its builtin and everything else takes the same products written
out: E77.

**A score is kept once it is paid for.** Four nodes in five are evaluated and return at once, by
reverse futility or as leaves, and neither writes to the transposition table. So the search keeps
the last million evaluations in a cache of its own, 8 MB, a position's key with its score in the low
bits. A quarter of all evaluations are found there, from other orders of the same moves and from
the iteration before. Every bit of the key is compared, so the tree is the same node for node, and
`search_clear()` empties it with the table: E74.

**Every size has its own network, of one shape.** The weights of size n are `src/net<n>.h`: its
filters with a cell for each of its squares, and its own head. A size without a header counts
flats. Only the size in play is spread into the table a search reads, tens of megabytes, built
when a game of that size starts and freed when another size does: `nn_select()`. The weights of
the other sizes stay in the binary, never read, so the engine holds one table at a time, 55 MB in
all on 6x6 where the six tables at once came to 150, E119.

The trainer lays the filters on an 8x8 grid, each board with its edges on the grid's edges, the
near half of each line counted from one edge and the rest from the other, so sizes fitted together
share a corner, an edge and the lines beside it, and only the middle differs. Sharing let the sizes
with few games learn from 6x6's what stacks and captives are worth: 7x7 and 8x8 gained most, E39.
Fitted from one list of games, sizes share in that way; fitted from a list of one size, that size
is fitted alone, and its header is the only one written.

## Training data

`docs/DATA.md` has the format and the tools. A game on disk is its moves and the search's scores,
at four bytes a ply, and replay gives every position back, so data never depends on the evaluation
that is being fitted to it.

A network is fitted on an NVIDIA GPU, and only there: a CPU is a hundred times slower, which
hundreds of millions of positions rule out. `src/gpu.c` finds the driver and its runtime compiler
when the tool runs, so no build depends on CUDA. The kernels run one hidden unit a thread, with the
output added up a warp of 32 filters at a time, so the width must be a multiple of 32, and their
forward pass is `forward()`'s, which still scores the held-out positions on the CPU. Gradients are
added in fixed point, which makes a fit repeatable to the byte however the card orders its adds.

The trainer keeps the games, not the positions: at four bytes a ply a billion positions' games
fit in memory, where the positions made from them, at about 80 bytes each, would not. Each game is
replayed once when it is read, to count what the fit takes from it, and then again whenever its
block comes round: an epoch shuffles the games and makes their positions a block at a time, on
every thread, each game's into slots its count fixes, so a fit is the same whatever the threads.
