# Experiments

Every measurement goes here, with the conditions that produced it. Machine unless noted:
Ryzen 7 5800X, Windows 11, one thread. Up to E6 that is gcc 12.2 `-O3 -flto -march=native`, and
from E7 clang 15.0.7 `-O3 -march=native` as one translation unit.

Bench signature now: **44927798 nodes**, identical from clang and gcc, native, `popcnt` and
generic. It was 49104542 at the end of the skeleton, and E9 and E10 are what moved it.

## Speed

Nodes per second, single thread, with the hash and road check running as they do in search.
These numbers are from before E7, and E8 has the current ones.

| Position | Depth | Nodes | make/unmake every leaf | Bulk count at the leaves |
|---|---|---|---|---|
| 6x6 start | 5 | 1,253,506,520 | 112 M/s | 3.1 G/s |
| 6x6 endgame | 4 | 395,359,484 | 48 M/s | 1.5 G/s |
| 8x8 start | 5 | 26,642,455,192 | 118 M/s | 6.0 G/s |

The endgame is the honest number: the start positions are nearly all placements. The
full published suite, depth 5 of that endgame included (48,986,506,534 nodes), takes 36
seconds. Search runs at about 3.6 M/s with the placeholder evaluation.

The race against other engines is E5, then E8.

## Correctness

| Check | Result |
|---|---|
| `published.perft`, 9 positions, full depth | 0 failures |
| `sealed.perft`, 144 positions, sizes 3-8 | 0 failures |
| selftest | 576,610,929 checks, 0 failures |
| perftdiff, 48 games per size, depth 3 | 101,986 positions, 0 differences |
| UB trap build (`-fsanitize=undefined`), selftest and perft | clean |

## Harness known-answer tests

A test that cannot pass a clear gain and reject a null means nothing, so these came first.
All on 6x6, 2 komi, `tests/books/6s_4ply.txt`, 7 concurrent.

| # | Match | Bounds | Expected | Result | Games | Elo |
|---|---|---|---|---|---|---|
| E1 | 20k nodes vs 5k nodes | [0, 5] | PASS | **PASSED** | 462 | +207.3 +/- 35.2 |
| E2 | 5k nodes vs 20k nodes | [0, 5] | FAIL | **FAILED** | 454 | -208.7 +/- 35.5 |
| E3 | same binary, 1+0.01 | [0, 20] | not PASS | **FAILED** | 476 | -22.7 +/- 29.0 |

E1 and E2 are the same match with the roles swapped, and came back mirror images. E3's
interval includes zero, as a null's should.

## E4: time management

`set_time_budget()` could spend almost the whole clock on one move and reserved nothing for
getting the move back, so a low clock flagged. It now keeps 20 ms aside and never stakes
more than half of what is left. Bench unchanged.

| Match | Before | After |
|---|---|---|
| 8x8, 2+0.02 | 7 flags in 48 games | 0 in 60 |
| 8x8, 1+0.01 | - | 3 in 40 |
| 6x6, 1+0.01 | - | 3 in 476 |

Moving the runner from threads to processes was not what fixed it: 5 flags remained
afterwards. Below 2+0.02 on 8x8 the increment is shorter than this runner's turnaround.

## E5: move generation against the baselines

`make speed`, 2026-11-01, best of three, nothing else running. Mnps on 6x6.

| Position | Depth | Yapper full | Topaz full | Tiltak full | Yapper bulk | Tiltak bulk |
|---|---|---|---|---|---|---|
| Start | 5 | **103.3** | 44.8 | 15.9 | **2915.7** | 221.3 |
| Midgame | 4 | **54.6** | 23.9 | 9.3 | **958.1** | 66.3 |
| Late | 4 | **48.3** | 19.0 | 8.4 | **2062.5** | 107.4 |
| Endgame | 4 | **48.1** | 19.3 | 8.5 | **1550.7** | 90.4 |

Yapper leads by 2.1 to 2.5 times on full make/unmake, and 13 to 19 times on bulk counting.

Each baseline was timed through its own perft: Tiltak fc4f52f and Topaz 2989527, with rustc
1.90 and `-C target-cpu=native`, as Yapper uses `-march=native`. Read it with these in mind:

- Topaz has no bulk mode, so there is only a full number for it.
- Topaz's perft does not stop at finished games, so it reports more nodes (454M against 395M
  on the endgame) and does no game-end check at them. That flatters it, so the lead is
  understated if anything.
- Tiltak's full mode carries per-node checking overhead, so bulk is its fairer number.
- Node counts matched Tiltak exactly on all four, two of them positions neither suite had
  covered, which is one more independent check of the rules.
- This is perft, not search. It says nothing about strength.

## E6: first games against another engine

The real racetrack, Yapper against Tiltak at 5+0.05 on 6x6 with 2 komi, 40 games from
`tests/books/6s_4ply.txt`: **+1 -39**, about -636 Elo [-1106, -289]. No illegal moves,
crashes or time losses, so TEI holds up. The score is the placeholder evaluation, and no
strength work is planned until move generation is settled.

## E7: where the time went in make and unmake

Three experiments set the direction, on a build that still ran at 48 Mnps on the endgame:

- Stubbing out the hash gained 50% on spreads. A hash with half the multiplies gained only 10%,
  so the multiplies were not the cost.
- One move made and unmade in a loop, so perfectly predicted, cost 16 ns for a 1-hop spread, and
  another 5 ns per hop. Shuffling the moves added 3 ns. So it was not branch misses or stalls.
- The disassembly explained both: 290 instructions for a 1-hop spread, most of them spills. The
  compiler had run out of registers, and every placement paid to save them too because it
  shared a function with spreads.

Then, in order, on the race positions, full / bulk Mnps. Bulk below 100 ms is noisy, so read
those to two figures. All of this kept bench and move order exactly, except where marked.

| Change | Start | Midgame | Late | Endgame |
|---|---|---|---|---|
| Baseline | 112 / 3100 | 54 / 960 | 49 / 2000 | 48 / 1500 |
| `tall`, line table for reach, road filter, owners in registers, state as one block | 98 / 4100 | 54 / 2000 | 49 / 3200 | 48 / 2900 |
| Spreads out of line, state copied in two moves | 159 / 4800 | 59 / 2100 | 52 / 3400 | 51 / 3100 |
| Stacks carry their height, hash as a sum (E9) | 163 / 4800 | 77 / 2300 | 72 / 3700 | 69 / 3400 |
| No calls in the fast spread: towers flagged once per move | 168 / 4600 | 90 / 2500 | 86 / 4200 | 82 / 3800 |
| Moves on an arena in perft: 16 KB frames cost a Windows stack probe at every node | 166 / 6100 | 87 / 2700 | 85 / 4700 | 81 / 4300 |
| Save once per node instead of once per move | 179 / 6000 | 89 / 2700 | 87 / 4500 | 82 / 4100 |
| `flatten` on `perft_node`, flood out of line | 200 / 7100 | 99 / 3000 | 98 / 5100 | 93 / 4700 |
| Placements a rank at a time | 215 / 7300 | 103 / 3100 | 100 / 5000 | 94 / 4600 |
| Lone stones a direction at a time (E10), loops split by kind, no `stm` to flip, keys in the board | 261 / 8300 | 126 / 3200 | 113 / 5300 | 108 / 4800 |
| clang, one translation unit | 345 / 9700 | 130 / 3400 | 116 / 5500 | 111 / 5000 |
| Lone stones make and unmake on a path of their own | 347 / 10000 | 149 / 3400 | 119 / 5600 | 118 / 5100 |

The last row follows the leaves. Lone steps are 7% of them on the start, 35% on the midgame, 10%
on the late position and 17% on the endgame. One-hop spreads from stacks are another quarter of
the last three, and have no shortcut yet: unlike lone steps, they are not listed together.

Tried and dropped, because they did not earn their place:

| Idea | Result |
|---|---|
| gcc PGO | No gain on top of `flatten`, and 8% off bulk |
| clang PGO | The profile runtime is not in this install |
| Other optimiser flags, 10 sets of them | All within 3%, which is what a rebuild moves anyway |
| `flatten` on `search` | The compile had not finished after 4 CPU minutes |
| Testing both road filters and branching once | 5 to 15% off bulk: the branch was predicting fine |
| Pool sorted by hops, to help the hop branch | No change |
| Splitting `shr mem, cl` into three steps | No change |
| 16 or 8 byte state copies | 16 no change, 8 slower: store forwarding is not the limit |
| Reading the parent's state from the Undo | On the start position, +17% bulk on gcc but -12% full on clang |
| A lone step's unmake reading the mover from the board, not the Undo | No change |

## E8: move generation against the baselines

`make speed`, 2026-11-01, best of three, nothing else running. Mnps on 6x6, and the baselines
are the same builds as E5.

| Position | Depth | Yapper full | Topaz full | Tiltak full | Yapper bulk | Tiltak bulk |
|---|---|---|---|---|---|---|
| Start | 5 | **346.5** | 46.0 | 16.8 | **10038.8** | 233.0 |
| Midgame | 4 | **148.6** | 24.6 | 9.7 | **3442.1** | 68.4 |
| Late | 4 | **119.3** | 19.7 | 8.7 | **5598.9** | 107.6 |
| Endgame | 4 | **118.3** | 19.2 | 8.5 | **5132.4** | 91.3 |

Yapper leads these two by 6.0 to 7.5 times on full make/unmake, and 43 to 56 times on bulk
counting. It was 2.1 to 2.5 and 13 to 19 before E7. The notes under E5 still apply. Node counts again matched
Tiltak exactly on all four.

Raw generation, with no make at all, is 0.44 to 0.53 ns a move on the midgame, late and endgame
positions, and 0.29 on a position from the fourth ply.

Other sizes from the start position, in Mnps, against the build from before E7. Starts are nearly
all placements, so these flatter any board the way the 6x6 start does.

| Board | Depth | Nodes | Full, before | Full, now | Bulk, before | Bulk, now |
|---|---|---|---|---|---|---|
| 8x8 | 5 | 26,642,455,192 | 120.5 | **387.8** | 6,552.7 | **20,502.4** |
| 7x7 | 5 | 6,813,380,628 | - | **372.8** | - | **14,134.0** |
| 5x5 | 6 | 11,293,470,152 | 101.0 | **310.3** | 1,962.9 | **5,831.6** |
| 4x4 | 7 | 5,231,815,136 | - | **212.9** | - | **2,918.5** |

Those deep runs are from just before the lone-step row of E7, which does nothing on a start
position. Shallower, on the final build, against Tiltak, the one baseline raced on other sizes:

| Board | Depth | Nodes | Yapper full | Tiltak full | Yapper bulk | Tiltak bulk |
|---|---|---|---|---|---|---|
| 8x8 | 4 | 142,512,336 | **389.5** | 17.9 | **24853.0** | 385.5 |
| 7x7 | 4 | 48,051,008 | **390.2** | 17.1 | **18802.2** | 328.5 |
| 5x5 | 5 | 187,855,252 | **327.0** | 16.4 | **6712.5** | 189.5 |

That is about 20 times on full. Yapper's bulk runs here took under 30 ms, so read them to one
figure, or take the deep table above.

`make ARCH=generic` assumes nothing about the CPU, so popcount falls back to software and vectors
to SSE2. On the race positions:

| Position | Full | Share of native | Bulk | Share of native |
|---|---|---|---|---|
| Start | 304.9 | 88% | 7342.2 | 73% |
| Midgame | 124.6 | 84% | 2778.5 | 81% |
| Late | 94.6 | 79% | 4278.7 | 76% |
| Endgame | 93.6 | 79% | 4012.2 | 78% |

Even so it is 4.8 to 6.6 times Topaz on full, and 8.0 to 9.2 times syntaks on bulk, with both of
those built for this exact CPU.

The two newer engines race on bulk alone, and that result is E13.

## E9: stacks that carry their height, and a hash that is a sum

Dropping `height[]` for a sentinel bit took a load and a store out of every hop, and hashing
the change as `(new - old) * key` took each square from two full mixes to one multiply. This
changes every hash, so bench and the table's behaviour both move: a behavioural change.

6x6, 2 komi, `tests/books/6s_4ply.txt`, 7 concurrent, bounds [-20, +5], against the skeleton:

| TC | Result | Games | Elo | Pairs |
|---|---|---|---|---|
| 1+0.01 | **PASSED** | 138 | +76.8 +/- 49.8 | [6, 0, 40, 4, 19] |
| 2+0.02, new seed | **PASSED** | 464 | +20.2 +/- 29.7 | [42, 10, 113, 13, 54] |

The first was too fast and too large to believe, so the second was run to check it. It agrees on
the verdict and not on the size, which is the early-stop bias TESTING.md warns about. The bounds
are coarse on purpose. Pairs here are far more polarised than a chess engine's, and [-10, +2]
projected to hours. These tests say "does not lose strength", and nothing finer.

## E10: lone stones generated in bulk

Lone stones used to go through the per-stack path like everything else. Generating them a
direction and a rank at a time halves raw generation on the stack-heavy positions, from about
100 ns to about 50 ns a position. It also means search sees lone stones' moves first, grouped
by direction, rather than in square order, so bench moved, from 50077923 to 44927798.

Same conditions, 2+0.02, against E9: **PASSED**, 380 games, +23.8 +/- 31.5, pairs [28, 13, 98, 7,
44].

## E11: a failed test, and why nothing was lost

Taking the side to move out of the hash, and out of the board altogether, saves about seven
instructions a move. As first written it changed the table's keys, so it was tested: **FAILED**,
1374 games, -17.2 +/- 17.7, pairs [167, 31, 329, 23, 137], same conditions as E10.

There is no logic in that change to lose Elo, but that is not a reason to rerun it. Instead the
side key is added back where the table reads the key, with the same value as before, which
makes the keys identical. Repetition never compares different sides, so bench went back to
exactly 44927798. The speed stayed, and with no change in behaviour there is nothing left to
test. The failed variant is gone.

## E12: all of it together

The final build against the skeleton it started from, same conditions as E10: **PASSED**, 250
games, +43.3 +/- 40.0, pairs [18, 6, 60, 9, 32].

The real racetrack against Tiltak at 5+0.05, as in E6, 40 games: **+1 -39**, exactly the score
of E6, with no illegal moves, crashes or time losses, and nothing about any in its debug log. Move
generation does not make up for the evaluation, and it was never going to.

Search runs at about 4.6 M/s, up from 3.7. That is modest because the evaluation and the
road-threat floods dominate a node, not move generation.

## E13: the newer engines

`Ciekce/syntaks` 905022f and `87flowers/pentakle` c30915d, built 2026-11-01 as black boxes with
`-C target-cpu=native`, source deleted unread. Rust 1.99.1, and nightly for pentakle.

`make speed`, same run as E8. Bulk Mnps:

| Position | Depth | Yapper | syntaks | pentakle | Tiltak | Lead over the fastest |
|---|---|---|---|---|---|---|
| Start | 5 | **10038.8** | 798.2 | 727.8 | 233.0 | 12.6x |
| Midgame | 4 | **3442.1** | 346.7 | 314.8 | 68.4 | 9.9x |
| Late | 4 | **5598.9** | 493.0 | 404.8 | 107.6 | 11.4x |
| Endgame | 4 | **5132.4** | 447.1 | 359.3 | 91.3 | 11.5x |

Both are single-threaded, with 1.69 CPU seconds in 1.72 of wall clock for syntaks and 1.73 in 1.79
for pentakle, and the rates they report match an outside clock to within 1%.

Neither has a separate full count, and whether their one perft makes its leaves had to be found
out, because it decides which column they belong in. A count that stops at the last ply speeds
up as that ply gets busier, and making every leaf does not. So, on a position built so that White
has far more moves than Black, in Mnps:

| Last ply | Moves there | Yapper full | Yapper bulk | syntaks | pentakle |
|---|---|---|---|---|---|
| White, depth 3 | 225 | 119.2 | 4352.4 | 395.0 | 382.5 |
| Black, depth 4 | 43 | 143.3 | 2225.7 | 313.2 | 303.0 |
| White, depth 5 | 217 | 90.4 | 4298.2 | 492.1 | 462.5 |

Making leaves slows down on White's plies, which are expensive spreads. Both rivals speed up, as
a count does, so they are read as bulk. That is an inference from timing, and nothing confirms
it directly. The position:
`21,21,21,21,21,2/21,x,21,21,2,21/1,21,21,2,21,1/1,21,2,21,21,1/21,2,21,21,x,21/2,21,1,21,1,21 1 40`

Their counts need care. syntaks keeps going past a finished game, as Topaz does. pentakle agrees
with Yapper on all four race positions, but on the position above it says 1,302,224 at depth 3
where takref and Yapper both say 1,293,989. The cause is unknown, and looking for it would mean
reading its source.

This is about speed alone: no match has been played against either, so nothing here ranks them
or says which is strongest.

## Mistakes worth not repeating

- **perftdiff is not background work.** It takes every core, which was forgotten once: a duel
  run beside it read 35 Mnps on a position that runs at 145, and bench read 612k nps.

- **A compiler can delete the thing being timed.** A PGO build reported 517 Mnps on the start
  position. It had inlined make and unmake together and, with nothing reading the board between
  them, thrown most of both away. `board_touched()` in perft.c now forces the position to exist
  in memory at every leaf, and the plain numbers barely moved.
- **Measuring a part in a loop over 100 boards measures L2.** Each board is over a kilobyte, so
  those timings were dominated by cache misses a real tree never has.
- **Windows will not start a relative path with forward slashes.** The first SPRT here played no
  games at all and reported success as an exit code. `tools/match.py` now resolves the path, but
  read the log of any run that finished suspiciously fast.
- **A stale binary times as a rival.** A failed build left the previous wrapper in the output
  folder and it was copied under the wrong name. Sizes matched, which is what gave it away.
- **A run alongside a match ruins the match.** An E3 that shared the machine with perftdiff
  recorded 470 time losses in 1200 games. It was discarded and rerun alone.
- **Narrow bounds take a while even for a huge gain.** With [0, 5], a 200 Elo gain still
  needs about 235 pairs, because the LLR grows linearly. A cap of 400 games read as
  INCONCLUSIVE at +201 Elo, and that was the cap, not the test.
- **A fixed score window for an opening book** rejected nearly everything, because 2 komi
  puts every opening near -200 with this evaluation. mkbook now keeps those nearest the
  median.
