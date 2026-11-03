# Experiments

Every measurement goes here, with the conditions that produced it. Machine unless noted:
Ryzen 7 5800X, Windows 11, one thread. Up to E6 that is gcc 12.2 `-O3 -flto -march=native`, and
from E7 clang 15.0.7 `-O3 -march=native` as one translation unit.

Bench signature now: **5871664 nodes**, identical from clang native, clang generic and gcc
`popcnt`. It was 49104542 at the end of the skeleton, E9 and E10 moved it to 44927798, the fitted
evaluation of E15 and E17 to 10797140, the search work of E20 to E25 to 9465377, the network of E34
to 11191478, and the search work of E36 moved it here.

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

## E14: how the placeholder lost

The 39 losses of E12 replayed through `tools/gamestats.py`, from the game records alone:

| Measure | Yapper | Tiltak |
|---|---|---|
| First capstone, median ply | 29 | 9 |
| Stones in reserve at ply 80 (25 games got there) | 13.4 | 3.3 |
| Spreads a game | 22.6 | 14.2 |
| Under own tops at ply 80, own / enemy stones | 1.3 / 11.8 | 6.2 / 6.0 |

24 were lost on flats, by a median of 2 with komi, and 15 by road, 12 of those on a spread that
the threat term and `quiesce` cannot see. Yapper read above +3 at some point in 16 of the lost
games, and its score moved a median 0.69 flats from one of its moves to the next. Tiltak's own
score passed 0.7 for good by a median of ply 17. That is its opinion on a scale not confirmed
here, but it says the games were decided long before the endgame.

Scores in PTN comments read from white's side. A first version of this analysis took them as
the mover's own, and had Yapper ahead in lost games it knew it was losing. Worth checking in any
tool that reads them.

## E15: a fitted evaluation

The evaluation was split into terms and weights first, with no change in behaviour: bench stayed
exactly 44927798. Three deliberate bugs confirmed the new selftest gates fail when they should.

A hand-set capstone value, tested alone at 2+0.02 against that build, reached **+21.5 +/- 25.2**
over 648 games with an LLR of +0.58. It was stopped there, inconclusive, because datagen cannot
share the machine with a match and a fit was about to replace the value anyway. It makes no claim.

Then the 40 terms of `eval.h`, fitted to 60,000 games of self-play at 5000 nodes a move: 4.7M
positions kept, 33.1 games/s on 15 jobs, 92 seconds to fit. Held-out loss fell from 0.620 to
0.560.

At 2+0.02 against the original evaluation: **PASSED**, 418 games, +371 -44 =3, **+365.2 +/- 52.1**,
pairs [1, 0, 42, 3, 163]. Bench is 8932865 at 4.77M nps, a fifth of the nodes at the same depth.
That is the search pruning far harder, since a side to move now reads about a flat better than
its opponent. It has not been checked for what it costs.

The real racetrack against Tiltak at 5+0.05, 100 games: **+32 -66 =2, -123 Elo [-197, -47]**, from
+1 -39 in E6 and E12, with no illegal moves or time losses. It now loses on flats, 58 of the 66,
trailing by about 3 flats through the middlegame, and plays 6.0 walls a game to Tiltak's 1.3.

The scale matters less than expected. Fitted at 620, the best fit for the hand-set weights, a
flat came out near 228. Refitting at 310 put it back near 100 and bench barely moved, from 7881622
to 8932865, so the size of the side-to-move term is the likelier cause of the drop in nodes. The
scale stays at 310.

## E16: road distance, a failed test

Fewest stones to finish a road for each side, out to three, split by who moves next. It cost 20%
of the search's speed, from 4.85 to 3.9 Mnps, and held-out loss improved by only 0.0006.

Against E15 at 2+0.02: **FAILED**, 698 games, +206 -340 =152, **-67.5 +/- 20.2**, pairs [63, 81, 154,
29, 22]. Dropped, and bench went back to exactly 8932865. This was expected to be the largest
gain of the plan, and it is the clearest loss.

## E17: generations of data

Each round plays 60,000 games with the best engine so far, at 5000 nodes a move and 15 jobs, then
fits again. All at scale 310, and SPRTs at 2+0.02 with [0, 5] against the previous best. Tiltak
is the real racetrack at 5+0.05, as in E6.

| Fit on | Games ended by road | Against the last best | Against Tiltak |
|---|---|---|---|
| Gen 1 (E15) | 61% | **PASSED** +365.2 +/- 52.1 | +32 -66 =2, -123 [-197, -47] |
| Gen 2 alone | 3% | not run | +21 -174 =5 |
| Gens 1-2 | | **PASSED** +171.0 +/- 33.3 | +26 -67 =7 |
| Gens 1-3 | 52% | **PASSED** +397.5 +/- 55.8 | +69 -114 =17, -79 [-128, -30] |
| Gens 1-4 | 39% | **PASSED** +89.7 +/- 23.2 | +73 -105 =22, -56 [-103, -8] |
| Gens 1-5 | 37% | INCONCLUSIVE +32.4 +/- 14.4 | +79 -101 =20, -38 [-85, +9] |

The last SPRT hit the 2000-game cap at an LLR of +2.75 against a bound of +2.94, so the cap ended
it rather than the test. Gen 5 was kept: nothing says it is worse, and it is the best result
against Tiltak. The loop stopped there because it has converged. Gens 4 and 5 ended almost
identically, White scoring 49.7% and 49.5%, and the last fit moved held-out loss by 0.0004. More of
the same data will not move these terms, so any further gain needs new terms or the search.

The road share is of the games that generation played, which is what the next fit reads. Gen 2
shows the danger: the gen 1 engine won by racing stones down, so against itself it barely built a
road, and a fit on those games alone forgot them. Mixing in gen 1 kept the correction it brought
to walls, 6.0 a game down to 2.2, and gen 3 then came out balanced by itself.

Self-play overstates all of this: gens 1-2 read +171 against gen 1 and level with it against
Tiltak. Gen 3's top flats and walls are now level with Tiltak's at every ply checked, and flat
losses are down to a median of one flat. It still differs in two ways: at ply 80 it has 7.1 stones
in reserve to Tiltak's 4.1, and holds 11.2 enemy stones under its tops to Tiltak's 6.8. What
remains is mostly roads: 50 of the 114 losses, 48 of them on a spread.

## E18: the evaluation base, all of it together

The final build, gens 1-5, against the evaluation it started from: **PASSED**, 414 games at 10+0.1
on 6x6, **+414 -0**. Sizes share weights fitted on 6x6 alone, and held up at 2+0.02: **+100 -0** on
5x5 with no komi, and **+60 -0** on 8x8 with 2 komi. No game in any of the three ended on a forfeit.
A perfect score has no Elo, so the +2400 the runner prints is its cap and means nothing.

Against Tiltak the same day, at 5+0.05 through the real racetrack:

| Build | Score | Elo |
|---|---|---|
| Placeholder, E12 | +1 -39 | about -636 |
| Final, 200 games | +79 -101 =20 | **-38 [-85, +9]** |

This is the first result whose interval includes level, and Tiltak is still ahead on the point
estimate. 40 of the 101 losses were by road, 37 of those on a spread, and the search has not been
touched for this evaluation.

300,000 games made 26.2M positions in 105 MB. `make check` passes, and bench is 10797140 from all
three builds tried. The cost of the harder pruning noted in E15 is still unmeasured, and the two
`THREAT` weights are still hand-set, since no position a fit reads has them.

## E19: what the search was doing

An instrumented copy, 120 self-play positions from gen 5 at 300,000 nodes each, before any change:

| Measure | |
|---|---|
| Leaves standing pat with a road by spread on the board for the side to move | **18.9%** |
| The same for the other side | 12.8% |
| Interior nodes cut by reverse futility | 60% |
| Null move tried, and cutting when tried | 1.25%, 16% |
| Beta cutoffs on the first move, and on a killer | 86%, 71% |
| Reductions needing a search again | 0.33% |
| Aspiration fails, each way | 19% |

Doubling the nodes, 200,000 against 100,000 a move, 400 games: **+205.0 +/- 35.0**, and **+218.6 +/-
38.8** on the build of E20.

Reverse futility looked like the fault, since the side to move reads 1.4 flats better. It is not.
With it and null move taken out, a node at the current margin of 90 a ply really fails beta about
1% of the time at depths 1 to 5 and 2% at 6, so the drop in bench noted in E15 is sound on this
measure.

## E20: roads by spread at the leaf

`movegen_road_spread()` answers whether a spread finishes a road without generating or playing
anything but the few lines that could. A road's first new square has to touch the group at one
edge and its last the group at the other, and the spread's squares are one line, so a line that
does not touch both cannot do it. Selftest holds it to a brute force count at every position.

Against E18 at 2+0.02: **PASSED**, 870 games, +488 -294 =88, **+78.8 +/- 22.0**, pairs [47, 27, 179,
49, 133]. That was with it playing out each pattern, at 4.03 Mnps against 4.80, and reading the top
stones off the stack's bits instead then made 4.29 Mnps with bench unchanged at 9409521.

The real racetrack against Tiltak at 5+0.05, 200 games: **+92 -85 =23, +12 [-34, +58]**, the first
result ahead of it, from -38. Road losses fell from 40 to 14 and those by spread from 37 to 12. What
is left is flats, 71 of the 85 losses by a median of one flat. Those are not lost late: Tiltak's own
score is at its final level 40 plies from the end, while Yapper's reads about -0.5 and only falls
past -1 in the last 16.

## E21: late move pruning, a failed direction

With it taken out, how often a move still cuts at a non-PV node at depth 3:

| Move | 1 | 2-3 | 4-7 | 8-15 | 16-31 | 32-63 | 64+ |
|---|---|---|---|---|---|---|---|
| Share cutting | 56% | 2.6% | 0.7% | 0.2% | 0.06% | 0.03% | 0.02% |

Moves past the 32nd were two thirds of that depth's work, so a much tighter limit looked free. At
2+0.02 against E20:

| Limit | Bench | Result |
|---|---|---|
| `4 + 4d^2` to depth 6, in place of `12 + 8d^2` to depth 4 | 4587776 | INCONCLUSIVE, 2000 games, **+2.1 +/- 13.8** |
| The same from depth 2, keeping any move that makes a threat | 5589263 | **FAILED**, 1358 games, **-42.9 +/- 17.1** |

Half the nodes for nothing. Past the 8th move, moves that make a threat are 3.7% of the moves and
34% of the cutoffs, and walls 12.6% and 0.9%, which is what suggested keeping the first. It did not
save it, and asking after every late move took the speed down to 2.96 Mnps. Both dropped.

## E22: a longer clock

Yapper spends 5.46 of about 7.25 seconds a game at 5+0.05, and Tiltak 6.67, with 1.65 times as long
on its first ten moves. Budgeting on 20 moves left instead of 30 at 2+0.02 against E20: INCONCLUSIVE,
2000 games, **+10.8 +/- 13.7**, LLR +0.85. Not kept. It moved time from the end of the game to the
start as well as adding some, and E25, which went the other way, passed.

## E23: three that did nothing

Treating a road by spread like a road by placement at interior nodes, so that nothing is pruned or
reduced under one, reached **-25.9 +/- 14.0** over 2000 games at the cap, with an LLR of -2.79
against a bound of -2.94. Counted as a failure. It cost 9% of the speed and made those nodes full
width, which is more than it gave back.

Refitting without the positions where a spread wins on the spot changed nothing, not one weight.
Those positions were never in the fit: the search finds the win at the root, so their score is
forced, and the fit already leaves those out.

Storing leaf stand pats and reverse futility exits in the table left the node count exactly the
same and took 3% longer, so the probes cost whatever they saved.

## E24: where a node's time goes

Cycle counts by part of a node, on the 120 positions of E19 at depth 8. These are real middlegames
with tall stacks, and bench is not: it put the road check at 21% of the time, where these put it at
38%. Its 570 cycles a call at an interior node also carry the stall of the table probe before it,
since the counter does not wait for memory. Times here are the search alone.

| Change | Nodes | Seconds |
|---|---|---|
| E20 | 16481834 | 5.03 |
| History of what answered what | 15890198 | 4.80 |
| Table move and killers tried before a list exists, and a prefetch of the child's table bucket | 16116606 | 4.78 |
| Looking for a road by spread only once pruning has passed the node by | 15505248 | 3.68 |
| Lone stones counted without a flood | 15505248 | 3.56 |

Everything down to the deferred road check, at 2+0.02 against E20: **PASSED**, 802 games, +456 -259
=87, **+87.1 +/- 23.1**, pairs [41, 25, 159, 48, 128]. Deferring the check is the row that mattered,
and it is sound: a position already failing high can only be confirmed by finding a win. Lone stones
left bench unchanged, so they are a pure speedup. A filter on how few hops a pattern can have moved
nothing and was dropped.

That 27% cut in time reads as about +99 at the rate of E19, which is close to the +87 measured.

## E25: the rest of the pass, and where it leaves us

At 2+0.02 against the last best:

| Change | Result |
|---|---|
| A node answering a road skips the placements that cannot stop it, as `quiesce` did | **PASSED** a non-regression test at [-20, 5], 836 games, +6.2 +/- 20.6, with 6.8% fewer nodes and 2.5% less time |
| A win on flats by the last stone or square, seen at a leaf | INCONCLUSIVE, 2000 games, +1.9 +/- 13.6. Dropped |
| The clock shared out over the moves left, from the shorter reserve | **PASSED**, 1498 games, +750 -571 =177, **+41.7 +/- 15.9**, pairs [90, 77, 326, 76, 180] |
| A shell sort of a list past its fourth pick | 7.4% more nodes, because ties lost the generator's order. Dropped |
| `pick_next()` in two passes, the first of them vectorised | Bench unchanged, 6.3% faster |
| Reductions a quarter stronger, `/ 2.0` for `/ 2.5` | INCONCLUSIVE, 2000 games, +930 -775 =295, **+27.0 +/- 13.9**, LLR +2.42, bench 5392249. Not kept |

The last one stopped at the cap with its interval above zero, and work stopped before it could play
Tiltak. It is left out so that every result below describes the code as it stands. It is the first
thing to try next.

The real racetrack against Tiltak at 5+0.05:

| Build | Score | Elo |
|---|---|---|
| E18 | +79 -101 =20 | -38 [-85, +9] |
| E20 | +92 -85 =23 | +12 [-34, +58] |
| E24 and the answer filter | +111 -75 =14 | +63 [+14, +111] |
| Final, 200 games | +132 -46 =22 | +159 [+107, +210] |
| Final, 800 games | **+487 -228 =85** | **+116 [+92, +140]** |

The 800 games are the number: the 200 ran high. That is about +154 on E18, against an aim of +200.

The final build against E18's, with no illegal moves or forfeits:

| Conditions | Result |
|---|---|
| 6x6, 10+0.1 | **PASSED**, 424 games, +336 -48 =40, **+287.6 +/- 39.4**, pairs [2, 2, 43, 36, 129] |
| 5x5, 2+0.02, no komi | 200 games, +129 -55 =16, +135.0 +/- 38.7 |
| 8x8, 2+0.02, 2 komi | 120 games, +109 -10 =1, +407.3 +/- 114.3 |

`make check` and `make datacheck` pass, with takref replaying 100 of 100 games. Bench is 9465377
from all three builds at about 6.3 Mnps. Five deliberate bugs in the road check each failed selftest
with hundreds of failures or more. `make perftdiff` passed, 101,986 positions with 0 differences in
44 minutes. The board and generator only gained the road check, and `board_road_placements()` moved
onto it.

What is left of the 228 losses is flats: 196 of them, and 32 by road, 28 of those on a spread. In
every one of the 196 Tiltak ran out of stones first, while Yapper held a median of 3, and in 138 of
Yapper's 143 flat wins it was Yapper that ran out first. Whoever gets there chooses when the game
ends. In the 200 games, flats on the board are level through ply 60 and Yapper leads slightly at ply
80, but with 6.2 stones in hand to Tiltak's 4.7 and 10.3 enemy stones under its tops to 7.3. Whether
that is the evaluation or the search has not been settled.

One game of the 5242 played on this clock was lost on time, in the reductions test at 2+0.02, by a
side already reading -18 at move 48. It is the only one this work has seen. The clock now runs lower
late in a game by design, so this is a real if small risk, and it has not been looked into further.
Yapper spends 5.93 seconds a game at 5+0.05 to Tiltak's 6.52, so some of the clock still goes unused.

## E26: the first match against syntaks

The build of E25 through the real racetrack at 5+0.05, as for Tiltak, 100 games: **+1 -99, -798
[-1266, -453]**, with no time losses or illegal moves. syntaks takes one thread and 64 MB by default,
so the conditions match, and it spent 5.76 seconds a game to Yapper's 5.52.

51 games were lost by road, 50 of those on a spread, and 48 on flats. It reports depth 14 to Yapper's
10 in the opening and 12 to 10 in the middlegame, though two engines need not count depth alike. It
reads itself 3 flats ahead by a median of ply 35, on its own scale. One sampled game has the two
disagreeing by about 2 flats on the same position near ply 30, so this looks like judgement as well
as depth, and neither has been measured apart. Beating Tiltak by 116 does not make Yapper close to
the top.

## E27: generation 6, and a refit on it

The first data from an engine that sees a road by spread at the leaf: 100,000 games from the build
of E25, frozen as its own binary, at 5000 nodes a move on 15 jobs. 9.05M positions, 4.18 bytes
each, 49.6 games/s, and takref replayed 500 games with no failures. Only 24.8% ended by road, from
37% in gen 5: two sides that see spreads build fewer roads that land.

So the fit used gens 1 to 6 together, at scale 310, and held-out loss went from 0.501065 to
0.500399. Bench is 9125938.

| Test | Result |
|---|---|
| Against E25 at 2+0.02 | **PASSED**, 1236 games, +630 -443 =163, **+53.0 +/- 18.1**, pairs [73, 66, 242, 75, 162] |
| Against Tiltak at 5+0.05, 400 games | +255 -96 =49, **+146 [+110, +181]** |

Road losses to Tiltak were 9 of 96, so mixing in the older generations kept roads in. This is the
baseline the first network is gated against.

## E28: the first network against E27

v0 from the `nnue` branch: E27's search, with a net for 6x6 trained on gens 1 to 6. Bench 11032593.
Against E27 on 6x6 with 2 komi:

| Conditions | Result |
|---|---|
| 100,000 nodes a move each | **PASSED**, 1270 games, +693 -499 =78, **+53.5 +/- 18.1**, pairs [90, 27, 294, 47, 177] |
| 2+0.02 | INCONCLUSIVE, 2000 games, +912 -945 =143, **-5.7 +/- 14.6**, LLR -0.74, pairs [217, 77, 438, 58, 210] |

It judges better and plays level, because it is slower. Timed at the search's two call sites on the
120 positions of E19 at depth 8, two alternating runs agreeing within 2%:

| | E27 | v0 |
|---|---|---|
| Cycles per evaluation | 314 | 530 |
| Mnps | 4.37 | 3.55 |
| Nodes to depth 8 | 15.72M | 18.90M |
| Seconds to depth 8 | 3.60 | 5.30 |

Bench put it at 13% slower, and these put it at 19% in nodes per second, plus a fifth more nodes to
reach the same depth. At the rate of E19, the 19% alone is worth about -65, which is about what the
net's judgement gains.

Most of that was a bug, found on the branch: the root's accumulator was cleared every iteration and
never built again, so 45% of evaluations rebuilt from scratch. Fixed, with bench unchanged, it costs
289 cycles an evaluation on the same positions and runs at the same nodes per second as E27. Against
E27 again at 2+0.02: INCONCLUSIVE at the cap, 2000 games, +1003 -843 =154, **+27.9 +/- 14.3**, LLR
+2.38, pairs [165, 66, 454, 74, 241]. Its interval is above zero, but it did not pass.

## E29: generation 7, a million games from the network

1,000,000 games from v0 with E32's fix, at 5000 nodes a move on 15 jobs, seed 7000: 86.7M
positions, 4.18 bytes each, 362.7 MB, in 5.75 hours. 48.3 games/s against gen 6's 49.6, so a
network teacher costs next to nothing at this depth. takref replayed 5000 games with no failures.

| | Gen 6 | Gen 7 |
|---|---|---|
| Games | 100,000 | 1,000,000 |
| Ended by road | 24.8% | **16.7%**, white 10.0%, black 6.7% |
| Ended by flats | | 75.7%, white 35.2%, black 40.5% |
| Draws | | 7.6% |
| White scored | | 49.0% |
| Median plies | | 88, longest 181 |

Roads fell again, as from gen 5 to gen 6. Gen 7 is 77% of the positions in gens 1 to 7, so a fit
on all of them sees fewer roads land than v0's did.

## E30: what a network would cost, measured before building one

An instrumented copy of the E25 search on 120 gen 5 positions at 100,000 nodes each, run on one
thread at low priority beside a match, so read the times to about 30%. `evaluate()` was called on
75% of nodes, at 98 ns a call and 23% of the search's time, at 320 ns a node.

A table hit skips the evaluation, so only 57% of evaluations had an evaluated parent, and the rest
reached 2 to 8 plies back. Rows an accumulator would touch per evaluation, carried from the nearest
evaluated ancestor:

| Inputs | Rows an evaluation | Rows in a full build |
|---|---|---|
| Top stone only, by square | 4.68 | 23.4 |
| **One token per stack, top 3 or top 6 stones** | **5.27** | 23.4 |
| Each of the top 6 stones by depth | 8.86 | 34.0 |
| 12 lines of 6 squares, 4 states | 8.87 | 11.8 |
| 25 2x2 patches | 14.81 | 24.1 |
| 20 two-rank bands, a colour each | 16.45 | 19.5 |

A whole stack as one token costs a tenth more than its top stone alone, and less than encoding
stones by depth, which renumbers a stack whenever a stone lands on it. Patterns over lines change
so often across a carry that they gain little from being kept up, and are better read afresh.

Over the 26.2M positions on disk, 76% of occupied squares are lone stones, 92.4% are 3 high or
less and 98.6% 6 or less. Of the 20,736 rows of square and token on 6x6, 17,706 were seen, and 538
of them serve 90% of reads. A capstone moves on 9.5% of its owner's moves. A canonical rotation by
the fullest quadrant would change on 18.9% of plies, which is why the symmetries are in the weights
instead.

The cost of an evaluation in plain loops, rows drawn from those frequencies at random, 5 rows each:

| Hidden units | Accumulators | ns, update and squared activation |
|---|---|---|
| 128 | 1 | 35 |
| 256 | 1 | 77 |
| 256 | 2 | 140 |
| 512 | 2 | 309 |
| 1024 | 2 | 570 |

At about 205 Elo a doubling of nodes, E19, a network costing 300 ns would start about 110 Elo down
on speed alone, which is why the first one has 256 units and one accumulator.

## E31: the network's machinery, with nothing trained

`src/nn.c`, the search's frames, `yapper nntrain` and `make release`, as ARCHITECTURE.md describes.

With no `netN.h`, bench is exactly 9465377, as before, so the search is unchanged. Selftest passes
585,968,615 checks, including random networks on every size. One deliberate bug per network check
confirmed each fails: an orientation left untied failed the transforms, a carry that forgot to take
black tokens away failed the carried accumulator and its score, and colours not folded under a
black top failed the colour swap.

The release build is 291,479 bytes against 450,689 and benches 9465377 too.

A smoke test, not a candidate: gens 3-5 at one position in four, 2.8M positions, 3 epochs on 3
threads beside datagen, in 100 seconds. Held out against results it reached 0.5083 to the
hand-written evaluation's 0.5034, which was fitted on these same games.

Fitted on floating point weights, the engine read 12.6 hundredths from its fit on average and 67
at worst, which is the rounding of 16-bit weights adding up over 256 squared units. Fitted on the
rounded weights themselves it reads 0.49 on average and 1 at worst, and the held-out loss did not
move. Built with that network, bench is 9956630 from clang native, gcc and a build with no CPU
assumptions alike.

clang sized the output loop by its 32-bit sum, so it ran 128-bit vectors where gcc ran 256: 32
multiply-adds where 16 do. A clang-only pragma takes it to 16, and gcc never sees it. The speed
against the hand-written evaluation could not be measured: datagen had 15 cores, and bench swung
from 2.2 to 3.6 Mnps between runs of the same binary.

## E32: the first network, v0

Fitted on gens 1-6 together, since gen 6 alone ended only 24.8% of its games by road: 25.9M
positions, 1.73M held out by game, lambda 0.5, 15 epochs on 15 threads, in 6 minutes. Held out
against results it reached 0.4950 to hce6's 0.5004, though hce6 was fitted on these games. The
engine reads 0.48 hundredths from its fit on average and 1 at worst. Built on hce6's weights for
every other size, so that only the 6x6 evaluation differs; bench 11032593.

Against hce6, E28 on master: at 100,000 nodes a move **PASSED**, 1270 games, **+53.5 +/- 18.1**. At
2+0.02 **INCONCLUSIVE** at the cap, 2000 games, **-5.7 +/- 14.6**. Better judgement, paid back in
speed: on 120 gen 5 middlegames to depth 8 it cost 530 cycles an evaluation to hce6's 314, ran 19%
fewer nodes a second, and searched 20% more nodes to the same depth.

Most of that cost was a mistake. Each iteration cleared the root's frame, and the root is a table
hit from the second iteration on, so it was never built again. A chain with no evaluated node
above it then went back to the root, found nothing and built from scratch: 45% of evaluations,
where E30 had counted 0.001%. E30's model never cleared its root, which is why it missed this.
Building the root's frame once a search, with each make clearing its child's frame instead, took
an evaluation from 470 cycles to 289 and the search from 3.73 to 4.37 Mnps on the same positions,
nodes unchanged at 18,902,916 and bench unchanged at 11032593.

Applying all of a carry's rows in one pass over the accumulator, rather than a pass a row, was
slower: 346 cycles with slices of 128, 372 with 64, against 289. Dropped.

With the fix, against hce6 at 2+0.02: **INCONCLUSIVE** at the cap, 2000 games, +1003 -843 =154,
**+27.9 +/- 14.3**, LLR +2.38 against a bound of +2.94, pairs [165, 66, 454, 74, 241]. The interval
is above zero, as gen 5's was in E17 when it was kept. v0 is the teacher for gen 7.

## E33: v1, the same network fitted with gen 7

v0's settings unchanged, on gens 1 to 7: 91.1M positions, 6.07M held out by game, lambda 0.5, 15
epochs, in 22 minutes. Held out against results, v1 reached 0.4654 to the hand-written
evaluation's 0.4782; the held-out games now include gen 7, so these do not compare with E32's. The
engine reads 0.49 hundredths from its fit on average and 1 at worst. Only `src/net6.h` differs
from 29c3c14. Bench 10745633, and `make release` the same.

Against v0 as committed in 29c3c14, on 6x6 with 2 komi:

| Conditions | Result |
|---|---|
| 100,000 nodes a move each | **PASSED**, 424 games, +362 -44 =18, **+338.0 +/- 47.4**, pairs [2, 1, 39, 17, 153] |
| 2+0.02 | **PASSED**, 424 games, +364 -37 =23, **+355.5 +/- 48.5**, pairs [2, 0, 34, 21, 155] |
| Tiltak at 5+0.05, 800 games | +724 -49 =27, **+428 [+384, +471]**, against hce6's +146 in E27 |
| syntaks at 5+0.05, 100 games | +1 -99, as in E26 |

No game in either SPRT was lost on time. The only change is more data, from a better teacher, and
it is the largest gain of any experiment here. Tiltak, which shares nothing with the self-play,
moved by about as much.

Gen 7's fewer roads did not cost roads. v1 won 583 games against Tiltak by road and lost 1 that
way, where hce6 lost 9 of its 96. Of the 49 losses, 48 were on flats, and in 34 of the 49 v1 had
been above +1 in the last third of the game. Against syntaks, 30 losses were by road, 28 of them by
spread, and 69 on flats, and v1 was above +1 late in 9 of 99.

These first read 36 and 12. racetrack drops mate scores, so a search that proved the result left
the score of an iteration before the proof: every v1 score recorded in 0.00s at depth 5 or less
fell within 7 plies of the end. `tools/gamestats.py` now skips scores below depth 3 or in no
measurable time.

On E19's 120 positions to depth 8, three runs each, alternating:

| | v0 | v1 |
|---|---|---|
| Cycles per evaluation | 294 | 290 |
| Nodes to depth 8 | 18.90M | 16.07M |
| Seconds to depth 8 | 4.39 | 3.60 |

One v1 run of four read 4.27 seconds with the same nodes; the others agreed within 0.3%. The
evaluation costs what v0's did, but the tree to the same depth is 15% smaller, close to hce6's
15.72M in E28. The larger tree in E28 belonged to v0's judgement, not to having a network.

## E34: generation 8, and v2

1,000,007 games from v1 at 5000 nodes a move on 15 jobs. Seeds 8000-8014 were stopped at 636,257
games when the plan changed, then 363,750 more came from seeds 8015-8029, so no game repeats.
88.7M positions, 4.18 bytes each, and takref replayed 5,001 games with no failures. 48.7 games/s,
and 45.1 for the second part, which shared the machine with a stray fit; datagen counts nodes, so
the games are the same either way.

| | Gen 7 | Gen 8 |
|---|---|---|
| Ended by road | 16.7% | 16.8%, white 10.7%, black 6.1% |
| Ended by flats | 75.7% | 73.8%, white 33.1%, black 40.7% |
| Draws | 7.6% | 9.3% |
| White scored | 49.0% | 48.5% |
| Median plies | 88 | 91 |

v2 is v1's network fitted on gens 1 to 8: 157.7M positions, 10.5M held out by game, lambda 0.5,
15 epochs. Held out against results it reached 0.4658 to the hand-written evaluation's 0.4840; the
held-out games now include gen 8, so these do not compare with E33's. The engine reads 0.50
hundredths from its fit on average and 1 at worst. Bench 11191478.

| Conditions | Result |
|---|---|
| Against v1, 100,000 nodes a move | **PASSED**, 662 games, +383 -192 =87, **+103.2 +/- 24.9**, pairs [25, 23, 126, 50, 107] |
| Against v1, 2+0.02 | **PASSED**, 576 games, +355 -139 =82, **+137.0 +/- 28.4**, pairs [19, 20, 83, 58, 108] |
| syntaks at 5+0.05, 400 games | +9 -381 =10, **-576 [-665, -481]** |

The first match against syntaks long enough to read: E26 and E33 lost 99 of 100, which says only
that the gap is wider than about 450. The aim for the day was -300 by 18:00, and it was missed.
Of the 381 losses, 110 were by road, 106 of them by spread, and 271 on flats by a median of 2
flats with komi. v2 was above +1 late in 61 of them.

On one position at 2 seconds on one thread, syntaks searched 824k nodes a second and reached depth
18 at 1.65M nodes, where v1 searched 3.0M a second and reached depth 13 at 3.6M. The gap is in how
selective the search is, not in speed.

## E35: each side's stones left, as an input to the network, a failed test

v1 lost 48 of its 49 games against Tiltak on flats, E33. At its last score above +1 searched to
depth 3 or more, it was usually behind on the board, in 21 of 33 by 2 to 14 half flats with komi.
Tiltak usually had fewer stones left, v1 had 5 to 11 of its own stones buried under its tops
against Tiltak's 1 to 3, and all 47 games that ended with empty squares ended with Tiltak placing
its last stone. v1 counted buried stones as flats to come when the other side could end the game
first. Its hidden layer never saw the reserves: they reached the output only through the bucket,
which takes the smaller reserve and so cannot say who is short, and through one linear term.

v3, 75fb3a9 on the `nnue` branch, gives the hidden layer each side's stones left, 0 to 14 exactly
and 15 or more as one, the way shogi networks see pieces in hand. v1 with its hand weights at zero
benches 10745633, unchanged. Selftest passes 586,084,324 checks, and two planted bugs, a carry that
skipped the hands and black's hand added rather than subtracted, failed it 137k and 102k times.

Fitted beside v2 on the same data and settings: 0.4654 against results to v2's 0.4658, and 0.4979
against the targets to 0.4984. The engine reads 0.49 hundredths from its fit on average and 1 at
worst. Bench 9877540. Against v2:

| Conditions | Result |
|---|---|
| 100,000 nodes a move | INCONCLUSIVE at the cap, 2000 games, +771 -917 =312, **-25.4 +/- 13.9**, pairs [204, 151, 375, 127, 143] |
| 2+0.02 | **FAILED**, 1766 games, +670 -835 =261, **-32.6 +/- 15.0**, pairs [196, 131, 321, 112, 123] |
| syntaks at 5+0.05, 400 games | +14 -383 =3, -557 [-648, -464], against v2's -576 |

It judged its lost games better, above +1 late in 31 of 383 losses to syntaks against v2's 61 of
381, and played worse. One of its 14 wins against syntaks was syntaks losing on time while v3 read
-19. Not kept: v2 is the network.

## E36: a more selective search for the network

E34 left Yapper reaching depth 13 where syntaks reaches 18 on fewer nodes. On 40 positions from the
v2-syntaks games at 1 second each, Yapper finished depth 12.85 on 2.82M nodes, with each iteration
costing about 2.0 times the last from depth 6 on. syntaks finished 19.5 on 1.22M, at 1.7 times
falling to 1.5. It needed a sixth of the nodes at depth 5, though the two need not count depth
alike. They chose the same move in 16 of 40.

An instrumented copy of v2's search on 60 of those positions at 2M nodes each:

| Depth left | 1 | 2 | 3 | 4 | 5 | 6 | 8 | 10 |
|---|---|---|---|---|---|---|---|---|
| Share of interior nodes | 79% | 12% | 4.9% | 2.1% | 0.8% | 0.4% | 0.07% | 0.01% |
| Cut by reverse futility | 82.8% | 52.1% | 37.2% | 25.0% | 14.0% | 7.2% | | |
| Null move cutting when tried | | | 1.3% | 4.4% | 10.6% | 17.2% | 27.3% | 32.6% |
| Moves searched at an all-node | 25 | 45 | 71 | 76 | 76 | 76 | 76 | 79 |
| Cutoffs on the first move | 77% | 80% | 87% | 91% | 92% | 94% | 95% | 96% |
| Reduced moves searched again | | | 0.35% | 0.21% | 0.24% | 0.16% | 0.13% | 0.15% |

Interior nodes were 80M of the 117M counted, the rest leaf searches. Late moves were nearly always reduced
and almost never needed a second look. Move-count pruning stopped at depth 4 and bound only at depths
1 and 2, so an all-node deeper than that searched about 75 of its moves.

Each change was tested against the last one kept, at 2+0.02 on 6x6 with 2 komi. The first used
[0, 5] with the usual cap of 2000 games. The rest used [0, 10] with a cap of 3000: that settles a
gain of 25 in about half the games, and a smaller one was not worth the time in this session.

| | Change | Bench | Result |
|---|---|---|---|
| ns1 | Reductions `/ 2.0` for `/ 2.5`, E25's last item | 5466538 | INCONCLUSIVE at the cap, 2000 games, +841 -704 =455, **+23.8 +/- 13.0**, pairs [112, 161, 372, 188, 167]. Kept, with E25's +27.0 +/- 13.9 for the same change |
| ns2 | One more ply of reduction at a node expected to cut | 5673872 | INCONCLUSIVE at the cap, 3000 games, +1194 -1094 =712, **+11.6 +/- 10.6**, pairs [183, 267, 545, 277, 228]. Kept |
| ns3 | No reduction for a late move that leaves a road to answer, found once and passed to the child | 5475681 | **FAILED**, 838 games, +296 -360 =182, **-26.6 +/- 20.2**, pairs [73, 76, 159, 64, 47] |
| ns4 | Improving, the evaluation above its own two plies up: reverse futility a ply looser, reductions a ply stronger without it | 2785363 | **FAILED**, 2144 games, +802 -843 =499, **-6.6 +/- 12.2**, pairs [138, 212, 411, 175, 136] |
| ns5 | Move-count pruning to depth 6, after `12 + 6d^2` moves when improving and `6 + 3d^2` when not, for `12 + 8d^2` to depth 4 | 3744360 | **PASSED**, 2592 games, +1047 -935 =610, **+15.0 +/- 11.4**, pairs [157, 224, 461, 258, 196] |
| ns6 | Singular extensions: from depth 6, a table move whose others all fall `2d` below its score is searched a ply deeper, and a node whose others still reach beta is cut | 5871664 | **PASSED**, 2140 games, +850 -747 =543, **+16.7 +/- 12.4**, pairs [114, 197, 396, 198, 165] |

ns1 and ns2 stopped at the cap with their intervals above zero, so the build as a whole was tested
against v2's search, and that settles them too:

| Conditions | Result |
|---|---|
| Against E34's v2, 2+0.02, [0, 10] | **PASSED**, 386 games, +198 -108 =80, **+82.5 +/- 31.3**, pairs [14, 22, 69, 36, 52] |
| syntaks at 5+0.05, 400 games | +6 -389 =5, **-665 [-784, -541]**, against v2's -576 [-665, -481] |

Against syntaks it did not show. The two intervals overlap almost entirely, and at this gap 400 games
hold only a few points, 8 here to v2's 14, so the match cannot tell the builds apart in either
direction. How the games were lost did move:

| | v2, E34 | E36 |
|---|---|---|
| Lost by road, by spread | 110, 106 | **133, 129** |
| Lost on flats, median margin in half flats | 271, 4 | 256, 4 |
| Above +1 late in a lost game | 61 of 381 | 61 of 389 |

The more selective search loses more often to a road by spread, 34% of its losses against 29%.
That is about 1.6 standard errors, so it is a lead and not a finding. Self-play would not see it,
because both sides share the blind spot. A match against syntaks will not settle which build is
better until it is close enough to score: a handicap, such as less time for syntaks, would put the
score where a few hundred games can measure it.

Two of the failures say the same as E21 and E23: in Tak, a search that spares the moves that
threaten a road loses more to the cost than it gains in what it sees. ns3 spared the moves that E21
found to be 3.7% of late moves and a third of their cutoffs, and it lost 27. ns4 halved bench
and lost. A smaller tree is not a stronger one, and how the cut is made matters more than how much
is cut. The improving flag did pay once it only decided how many moves to search.

`make release` benches 5871664 as well. One candidate was first built from stale source, because a
failed `diff` ended the shell chain before the new file was written. Its bench equalled the build
before it, which gave it away before any game was played.

## Mistakes worth not repeating

- **200 games is a sketch.** A match against Tiltak read +159 and was called the target reached.
  800 games on the same build read +116. Both intervals held the truth, and only one was narrow
  enough to report as a result.
- **One timing run proves nothing.** The same binary read 3.90 and 3.25 seconds minutes apart.
  Repeat a timing, alternating the builds, before reading anything into it.
- **A score in racetrack's PTN is not always the search's last.** It drops mate scores, so a proven
  loss can be recorded as +2.56 from depth 1. Read the depth and time beside every score.
- **Bench is not a profile of a game.** Its 13 positions put the road check at under half its real
  share, and only cycle counts on positions from real games showed what the search spends.
- **A low rate of use does not make a move safe to drop.** Moves past the 32nd cut less than 0.1%
  of the time and were two thirds of the work, and cutting them gained nothing: E21.
- **A better fit is not a stronger engine.** E16 lowered the held-out loss and lost 67 Elo.
- **A network fitted in floating point is not the one that plays.** Rounding to 16 bits put the
  engine 12.6 hundredths from its fit on average, E31. Fitting on the rounded weights took that to
  under one.
- **A model of the search is only as good as its edge cases.** E30's count of full rebuilds was
  0.001% because its root never lost its frame; the real search cleared it every iteration and
  rebuilt 45% of the time, E32. Count the real thing before trusting a cost model.
- **Copying a file over a source does not rebuild it.** `Copy-Item` keeps the file's old date, so
  `make` saw nothing to do and a second candidate was the first one under another name. Identical
  bench counts from two sets of weights gave it away. Check bench after every swap.
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
