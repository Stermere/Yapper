# Experiments

Every measurement goes here, with the conditions that produced it. Machine unless noted:
Ryzen 7 5800X, Windows 11, one thread. Up to E6 that is gcc 12.2 `-O3 -flto -march=native`, and
from E7 clang 15.0.7 `-O3 -march=native` as one translation unit.

Bench signature now: **1388639 nodes**, identical from clang native, clang generic and gcc
`popcnt`. It was 49104542 at the end of the skeleton, E9 and E10 moved it to 44927798, the fitted
evaluation of E15 and E17 to 10797140, the search work of E20 to E25 to 9465377, the network of E34
to 11191478, the search work of E36 to 5871664, E38's networks to 3204012, E39's one network to
3378307, E40's wider network and history to 3296722, E42's refit on gen 10 to 2723456, and its
tighter move-count pruning moved it here.

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

## E37: more varied data, and fitting on the GPU

Measured on what was there before changing anything.

**Repeats are rare.** Of 300,000 gen 8 games, 0.07% of positions after ply 10 repeat an earlier
game's line of moves, and 24% do in the first 10 plies. More random openings would not remove
repeats, since there are almost none. What they can add is structures the network's own play never
builds.

**What a node budget buys.** On E19's 120 middlegames, from an empty table, one search of 3M
nodes, reading off each iteration where a soft budget would have stopped it:

| Soft budget | Depth | Nodes used | Win chance off the 3M search, mean |
|---|---|---|---|
| 2,500 | 4.4 | 4.2k | 5.9% |
| 5,000 | 5.3 | 7.7k | 5.6% |
| 10,000 | 6.2 | 17k | 5.2% |
| 20,000 | 7.1 | 37k | 4.4% |
| 50,000 | 8.1 | 82k | 3.7% |

Four times the budget costs 4.8 times the nodes and takes a fifth off the error of a score. This
cannot say which buys more strength per hour, so Gen-1Big runs both: below.

**The score half of the target is less sure than the results.** On 2.35M gen 8 positions after the
opening, the scale that best turns the search's score into the game's result is 240, not the 310
`nntrain` uses: 0.4450 against 0.4537, and 220 to 240 in every third of the game. So
`sigmoid(score / 310)` pulls the target towards a draw that the results do not. Not changed: the
search's margins are in the same units, so this needs its own fit and SPRT.

**Datagen.** Openings are 4 to 16 random plies, from 4 to 9. The 2000-node search that judged an
opening is gone: the game's first search does it, and its move is played. On one ply in ten a
random move, chosen by kind as in the opening, is searched as well, and played in place of the
search's choice if it comes within 40 hundredths of it. It is stored with no score, so a scored
ply's move is still the search's own, and the format and magic are unchanged.

On 6x6 at 5000 nodes from E36's build, 15 jobs: 48.9 games a second, as gen 8's 48.7. 20% of
openings were thrown away, against 9% with the shorter openings; each costs one search. Of the
random moves searched, 11 to 13.5% came within the margin, one deviation in 100 to 111 searched
plies, for 8% more search. At 1 in 16 it was one in 158. `make datacheck` passes, and takref
replayed 250 games with no failures.

Games a second per job, on 15 jobs: 3x3 21.4, 4x4 9.2, 5x5 4.8, 6x6 3.3, 7x7 2.0, 8x8 1.4. At
20,000 nodes, 6x6 plays 0.69.

**Fitting on the GPU.** `nntrain` on the CPU fitted 1.05M positions a second on 15 threads, and
v2's fit took about 40 minutes. On an RTX 3070, through `src/gpu.c`:

| Version | Positions a second |
|---|---|
| CPU, 15 threads | 1.05M |
| GPU, every gradient added in 64-bit fixed point | 5.8M |
| GPU, a filter's gradients in 32-bit | about 18M |

The 64-bit adds were two thirds of the card's time: without them an epoch took a third as long.
A filter's gradient moves by at most 4 in a batch, so 2^28 in 32 bits cannot overflow, and the
fit did not move: 10 epochs on 5.65M positions ended at 0.490104 against 0.490128, and 0.478982
against 0.479399 held out against results. Laying out a batch was then the limit, because threads
were started for every batch of 16,384. Laying out 16 batches at a time fixed that.

Against the CPU on the same 5.65M positions, 2 epochs: held out 0.516669 against 0.516719, and
against results 0.494333 against 0.494421. The engine reads 0.49 hundredths from either fit on
average and 1 at worst. Two GPU fits gave the same bytes, as did 4 threads and 15, and `--cpu`
gives exactly the network the trainer gave before. Bench is 5871664 from clang native, clang
generic, gcc `popcnt` and `make release`.

v2's own fit again, gens 1-8 with its settings, took 4 minutes 4 seconds on the GPU against about
40 on the CPU. A minute of that was reading 157.7M positions, and an epoch was 13 seconds with the
10.5M held-out positions scored on the CPU. It reached 0.4979 against the targets and 0.4652
against results, to v2's 0.4984 and 0.4658, and the engine reads 0.50 hundredths from it on
average and 1 at worst. The held-out loss moved by up to 0.003 from one epoch to the next, even in
the last few, so which network a fit ends on is partly luck.

**Gen-1Big**, from 23:25 on 2026-11-02, from E36's search with v2 frozen as
`external/baselines/gen1big.exe`, into `external/data/gen1big/`. Gen 1 for the other sizes on 15
jobs at 5000 nodes, one after another: 200,000 games each of 3x3, 4x4 and 5x5, then 150,000 each of
7x7 and 8x8. Then 6x6, generation 9 in its headers: 600,000 games at 5000 nodes on 10 jobs and
64,000 at 20,000 on 5, side by side, so that five jobs of each are equal time.

## E38: a network for every size, and the hand-written evaluation retired

**Gen-1Big** ran from 23:25 to 08:58, as planned in E37, and `datacheck` replayed 8,634 of its
games through takref with no failures.

| Size | Games | Games a second | Median plies | Roads | Flats | Draws | A deviation in |
|---|---|---|---|---|---|---|---|
| 3x3 | 200,000 | 105.8 | 22 | 43.3% | 42.0% | 14.7% | 344 plies |
| 4x4 | 200,000 | 148.2 | 41 | 17.0% | 67.7% | 15.3% | 120 |
| 5x5 | 200,000 | 76.7 | 62 | 24.1% | 66.9% | 9.0% | 98 |
| 6x6, 5k on 10 jobs | 600,000 | 32.6 | 92 | 12.8% | 76.1% | 11.1% | 89 |
| 6x6, 20k on 5 jobs | 64,000 | 3.6 | 93 | 10.1% | 76.5% | 13.3% | 93 |
| 7x7 | 150,000 | 34.5 | 126 | 29.6% | 64.8% | 5.6% | 56 |
| 8x8 | 150,000 | 25.8 | 158 | 31.1% | 63.2% | 5.7% | 43 |

Every size but 6x6 was played by the hand-written evaluation, which was fitted on 6x6 alone. A
flatter evaluation finds more random moves within 40 hundredths of the best, which is why 7x7 and
8x8 deviate most.

**The other sizes' first networks**, v2's network and settings, lambda 0.5, on GPU fits of a
minute or two:

| Size | Positions | Epochs | Held out against results | The hand-written evaluation's |
|---|---|---|---|---|
| 3x3 | 975k | 15 | 0.3962 | 0.5340 |
| 4x4 | 4.06M | 15 | 0.3788 | 0.4657 |
| 5x5 | 7.39M | 15 | 0.4238 | 0.4639 |
| 7x7 | 14.0M | 3 | 0.5396, and 0.5453 on 15 | 0.5549 |
| 8x8 | 18.4M | 3 | 0.5861, and 0.5996 on 15 | 0.6097 |

On 7x7 and 8x8 the held-out loss was lowest after one or two epochs and rose from there while the
fit's kept falling: 150,000 games are too few for 49 or 64 squares of filters. Three epochs, on a
cosine of their own, beat fifteen. The engine reads within 0.51 hundredths of every fit on average
and 1 at worst.

New books, as `mkbook.py` builds them by default, judged by these networks: `4s_4ply.txt`
(median 139, kept 66 to 211), `7s_4ply.txt` (10, from -9 to 29) and `8s_4ply.txt` (21, from 8 to
34). The 4x4 median says white is well ahead after four plies with 2 komi.

Against the hand-written evaluation, E36's search for both, 2+0.02, 2 komi:

| Size | Result |
|---|---|
| 4x4 | **PASSED**, 422 games, +229 -46 =147, **+161.3 +/- 22.0**, pairs [1, 4, 59, 105, 42] |
| 5x5 | **PASSED**, 420 games, +308 -62 =50, **+233.2 +/- 34.6**, pairs [1, 7, 55, 39, 108] |
| 7x7 | **PASSED**, 498 games, +374 -117 =7, **+198.4 +/- 35.1**, pairs [13, 1, 90, 6, 139] |
| 8x8 | **PASSED**, 1456 games, +825 -614 =17, **+50.7 +/- 17.6**, pairs [124, 6, 360, 11, 227], 2 of them lost on time |
| 3x3 | 408 games, every pair 1-1: the side to move first won every game |

3x3 at a real clock is played out by the search, so no evaluation can be told apart there. At 200
nodes a move it can, and the network **FAILED**: 408 games, +178 -213 =17, **-29.9 +/- 11.5**,
pairs [9, 17, 178, 0, 0]. Only 975k of 4.5M positions were fitted on, 5 a game, because a 5000-node
search already sees the result of most of the rest, and a fit leaves out every position with a
forced score. A 200-node search lives in exactly those positions.

So 200,000 more 3x3 games were played at 500 nodes, by the hand-written evaluation, and the
network refitted on both sets: 2.06M positions, reaching 0.3392 held out against results to that
evaluation's 0.5053. At 200 nodes and again at 50, every one of 408 games went to the side that
moved first, as at 2+0.02: pairs [0, 0, 204, 0, 0]. With 2 komi and these openings, no budget that
was tried tells two evaluations apart on 3x3, where the first network had lost 26 pairs. The
refitted network went in.

**6x6, and whether 5k is too few.** Three fits with v2's settings, each on gens 1-8 and part of
gen 9:

| Fit | Gen 9 games in it | Positions | Held out against results |
|---|---|---|---|
| A | 300,000 at 5k, seeds 9000-9004: five jobs' worth | 177.2M | 0.4644 |
| B | 64,000 at 20k: five jobs' worth | 162.0M | 0.4644 |
| C | all 664,000 | 200.9M | 0.4636 |

The held-out games differ between fits, so these do not compare with each other.

| Match, 2+0.02 unless said | Result |
|---|---|
| A against B | **FAILED**, 994 games, +329 -494 =171, **-58.2 +/- 19.5**, pairs [114, 99, 174, 58, 52] |
| C against v2 | **INCONCLUSIVE** at the cap, 2000 games, +790 -877 =333, **-15.1 +/- 13.9**, pairs [195, 139, 380, 130, 156] |
| B against v2 | **INCONCLUSIVE** at the cap, 2000 games, +886 -778 =336, **+18.8 +/- 13.8**, pairs [152, 123, 374, 167, 184] |
| B against v2, 100,000 nodes a move | **INCONCLUSIVE** at the cap, 2000 games, +829 -793 =378, **+6.3 +/- 13.4**, pairs [159, 128, 385, 174, 154] |

At equal time, 64,000 games at 20,000 nodes taught far more than 300,000 at 5,000. Gen 9's 5k
games look harmful as well: C has B's games and more, and read -15 against v2 where B read +19 and
+6. Gen 8 was 5k games too and gave v2 its +137, so the harm would be in what changed since: E36's search, which is
more selective and may need more nodes before its scores mean as much, or E37's longer openings
and deviations, which the 20k games share. This cannot say which. Datagen for 6x6 should run at
20k from here.

B is not shown to be better than v2: one interval is above zero and the other holds it, so v2 stays.
How far apart two fits of the same games play has never been measured, and until it is, a gap of
15 between fits says little.

**The hand-written evaluation is gone.** `eval.c` keeps what the network is scored on besides the
stacks, and the two threat values, hand-set at -60 and -220 for the opening and the end alike, so
their blend was always exactly that and is now a constant. `weights.h`, `tune.c`, the trace and
selftest's check of it, `yapper tune`, and nntrain's comparison against the hand-written score
went with it. A size with no network now counts flats, which only a build whose network has
changed shape would ever play.

With every size on a network, nothing played the hand-written evaluation any more, so deleting it
had to be a pure change, and it measured as one: bench 3204012 from the source with it and without,
and from `make release`, and the same evaluation of one position on each of 3x3, 4x4, 7x7 and 8x8.
`make check` and `make datacheck` pass, and `nntrain` writes the same bytes as before, on the GPU
and with `--cpu`. The new networks had moved bench from 5871664 to 3204012, as bench has positions
on other sizes. It has none on 3x3, so that network was checked by an evaluation instead.

## E39: one network for every size, and a better fit from the same games

The aim was more from 6x6's games without new ones, and one network that plays every size. Matches
are at 100,000 nodes a move, 2 komi, E36's search, unless said, so that fits could run beside
them: two networks of one shape cost the same to run, so a fixed budget is fair between them.

**The trainer first.** A game is now held out by its place in its own file, so two fits that
read a file are scored on the same games of it, and the report scores each group of games, one
size, generation and budget, on its own. `--seed` varies the first weights and the order of the
positions. Every loss below is on one group's held-out games, against results.

**Averaging the weights.** E37 saw the held-out loss move by up to 0.003 between epochs, even at
the end. The network written is now the mean of the weights over the last tenth of the steps. On
B1, gens 1-8 and gen 9's 20k games, the mean scored 0.4640 where the last step's weights scored
0.4651, and it beat every epoch's own. Against those last-step weights: **INCONCLUSIVE** at the
cap, 2000 games, +846 -756 =398, **+15.6 +/- 13.7**, pairs [158, 123, 356, 197, 166]. Kept: its
interval is above zero, and the fit is better on the same games.

**How far apart two fits play.** B2 is B1 with seed 1. Their held-out losses differ by 0.0004 at
most, and in play, a fixed 2000 games: +896 -791 =313, **+18.3 +/- 14.2**, pairs [164, 121, 368,
140, 207]. Three seeds of the final recipe later spread wider still, below. Two fits of the same
games can play 15 to 35 apart, as far as most changes here move them, so one fit against another
says little unless something else agrees, and choosing among seeds is itself worth something.

**Which 6x6 games.**

| Fit | Games | Positions | Gen 7 | Gen 8 | Gen 9, 20k |
|---|---|---|---|---|---|
| B1 | gens 1-8, gen 9's 20k | 162.0M | 0.4516 | 0.4691 | 0.4467 |
| B2 | the same, seed 1 | 162.0M | 0.4517 | 0.4690 | 0.4463 |
| D1 | gens 7-8, gen 9's 20k | 136.0M | 0.4502 | 0.4669 | 0.4438 |
| E1 | gen 8, gen 9's 20k | 70.9M | | 0.4657 | 0.4402 |

| Match | Result |
|---|---|
| D1 against B1 | **INCONCLUSIVE** at the cap, 2000 games, +890 -800 =310, **+15.6 +/- 14.0**, pairs [159, 125, 381, 137, 198] |
| E1 against D1 | **FAILED**, 800 games, +256 -437 =107, **-80.0 +/- 22.8**, pairs [118, 59, 148, 36, 39] |
| D1 against v2 | **INCONCLUSIVE** at the cap, 2000 games, +867 -823 =310, **+7.6 +/- 13.6**, pairs [165, 113, 407, 143, 172] |

Gens 1-6, 400,000 games from the hand-written evaluation, can go, and gen 9's 5k games already
had, E38: D1 scores better on every newer generation, and the match leans its way. Gen 7 cannot.
E1 scored best of all on the newest games, by far more than a seed moves them, and lost 80. A fit
on the last generations learns the positions their play reaches and forgets the rest, which no
loss on those games can show.

**lambda.** On D1's games:

| lambda | Against lambda 0.5 |
|---|---|
| 0.3 | **FAILED**, 1712 games, +663 -826 =223, **-33.2 +/- 15.2**, pairs [191, 112, 346, 83, 124] |
| 0.7 | **INCONCLUSIVE** at the cap, 2000 games, +889 -795 =316, **+16.3 +/- 13.6**, pairs [143, 135, 391, 147, 184] |

lambda 0.3 scored best against results and lost: a target nearer the results always scores
nearer them, so the loss cannot choose `lambda`. Half the learning rate scored 0.4429 on gen 9's
20k games against 0.4430 to 0.4438 from three seeds at the usual rate, which is within them.

**One network for every size.** The shared grid of the `nnue` branch, brought onto the GPU
trainer without E35's stones in hand: an 8x8 grid of filters, each board on it with its edges on
the grid's edges, and each size with its own bias, output and skip. Selftest plays random networks
on it, and dropping the grid's transpose for the smaller boards failed it 155,380 times. The GPU
and the CPU fit the same network to within float rounding, and the engine reads within 0.51
hundredths of every size's fit on average and 1 at worst.

Fitting every size at once, 8x8's held-out loss was lowest after the first epoch and rose from
there, as on its own in E38. Weighting its loss down would not help, since the optimiser rescales
the steps of the weights only 8x8 reaches, so `--share` puts a part of a size's positions into each
epoch, drawn afresh each time: a fifth of 8x8's and a third of 7x7's.

Each size's own network refitted with this trainer, on E38's games and settings, against U3, one
network on D1's 6x6 games and every other size's at lambda 0.5:

| Size | Its own | Shared, U3 |
|---|---|---|
| 3x3 | 0.3531, and 0.3246 at 500 nodes | 0.3350, 0.3091 |
| 4x4 | 0.3737 | 0.3756 |
| 5x5 | 0.4134 | 0.4108 |
| 6x6, gen 9's 20k | 0.4438, D1 | 0.4440 |
| 7x7 | 0.5391 | 0.5328 |
| 8x8 | 0.5856 | 0.5814 |

U3 against E38's networks:

| Size | Result |
|---|---|
| 8x8 | **PASSED**, 586 games, +393 -183 =10, **+130.3 +/- 28.0**, pairs [21, 3, 138, 7, 124] |
| 7x7 | **PASSED**, 514 games, +386 -116 =12, **+202.8 +/- 36.1**, pairs [16, 3, 81, 9, 148] |
| 5x5 | **PASSED**, 862 games, +452 -296 =114, **+63.6 +/- 19.3**, pairs [38, 23, 208, 69, 93] |
| 6x6, v2 | **INCONCLUSIVE** at the cap, 2000 games, +869 -793 =338, **+13.2 +/- 14.0**, pairs [161, 134, 365, 148, 192] |
| 6x6, against D1 | **INCONCLUSIVE** at the cap, 2000 games, +831 -868 =301, **-6.4 +/- 13.6**, pairs [172, 139, 402, 128, 159] |
| 4x4 | **INCONCLUSIVE** at the cap, 2000 games, +515 -558 =927, **-7.5 +/- 9.0**, pairs [36, 238, 482, 221, 23] |

The sizes with the fewest games for their squares gain most from the one with the most. 6x6
plays as its own fit does, within the seeds. A first shared fit, U1, on E1's 6x6 games and with no
shares, had played 8x8 at -6.8 +/- 19.9 over 1126 games: the shares and the 6x6 games together
made that +130.

**lambda by size.** U3 refitted with lambda 0.7, and on 6x6 with 0.9, against U3:

| Size | lambda 0.7 | lambda 0.9 |
|---|---|---|
| 8x8 | **PASSED**, 1010 games, +600 -379 =31, **+77.3 +/- 21.9**, pairs [75, 10, 219, 21, 180] | |
| 7x7 | **PASSED**, 1046 games, +594 -385 =67, **+70.4 +/- 21.0**, pairs [70, 29, 221, 28, 175] | |
| 6x6 | **INCONCLUSIVE** at the cap, 2000 games, +852 -800 =348, **+9.0 +/- 13.7**, pairs [156, 142, 372, 154, 176] | **FAILED**, 1744 games, +663 -827 =254, **-32.8 +/- 15.1**, pairs [191, 125, 342, 85, 129] |
| 5x5 | **INCONCLUSIVE** at the cap, 2000 games, +873 -801 =326, **+12.5 +/- 12.5**, pairs [121, 130, 454, 146, 149] | |
| 4x4 | **FAILED**, 1282 games, +316 -369 =597, **-14.4 +/- 10.4**, pairs [17, 163, 324, 130, 7] | |

On 6x6 lambda 0.7 has read +16.3 and +9.0 in two separate fits. The larger the board, the more it
gained, and 4x4, the smallest that can be measured, lost. `--lambda` gives a size its own: 0.7, and 0.5 on 3x3
and 4x4.

**The network.** That recipe, three seeds:

| Seed | Gen 9, 20k | 4x4 | 7x7 | 8x8 | Against seed 0, 6x6 |
|---|---|---|---|---|---|
| 0, F0 | 0.4496 | 0.3741 | 0.5361 | 0.5863 | |
| 1, F1 | 0.4502 | 0.3739 | 0.5360 | 0.5851 | **FAILED**, 1538 games, +565 -728 =245, **-37.0 +/- 16.0**, pairs [171, 116, 291, 87, 104] |
| 2, F2 | 0.4503 | 0.3736 | 0.5352 | 0.5854 | **INCONCLUSIVE** at the cap, 2000 games, +781 -860 =359, **-13.7 +/- 13.8**, pairs [183, 155, 371, 140, 151] |

F0 is the network, `src/net.h`. Against E38's:

| Size | Result |
|---|---|
| 6x6, v2, 100,000 nodes | **INCONCLUSIVE** at the cap, 2000 games, +882 -785 =333, **+16.9 +/- 13.8**, pairs [155, 123, 380, 154, 188] |
| 4x4, 100,000 nodes | **PASSED**, 1990 games, +570 -488 =932, **+14.3 +/- 8.8**, pairs [20, 192, 507, 238, 38] |
| 6x6, v2, 2+0.02 | **INCONCLUSIVE** at the cap, 2000 games, +896 -760 =344, **+23.7 +/- 13.9**, pairs [146, 127, 368, 163, 196] |
| 4x4, 2+0.02 | **INCONCLUSIVE** at the cap, 2000 games, +496 -454 =1050, **+7.3 +/- 8.2**, pairs [15, 205, 520, 243, 17] |

With 4x4 back at 0.5, F0 beat 4x4's own network at 100,000 nodes where U3 had not. F0 itself was
not matched on 5x5, 7x7 and 8x8: U3 stands for it there, with lambda 0.7's gains on top. Every
size that can be measured plays at least as well on the one network as on its own, 5x5, 7x7 and
8x8 far better, and 3x3 scores better on its held-out games.

The per-size networks and their code are gone: `src/net.h` is the only network, `nntrain` always
fits every size in the games it is given, and the engine has one path. The trainer, without its
size argument, refits F0 to the same bytes. Bench 3378307 from clang, gcc and `make release`,
`make check` and `make datacheck` pass, and selftest passes 584,534,452 checks.

On 6x6 the gain is small: +16.9 at 100,000 nodes and +23.7 at 2+0.02, neither a passed SPRT,
though both intervals are above zero. Gens 7-9 hold little that v2 did not already have, and what
the recipe added, lambda 0.7 for 0.5 and the best of three seeds, is about the size of the seeds'
own spread. The large gains are on the sizes whose games are still all the hand-written
evaluation's.

Two short checks against outside engines, 6x6 at 5+0.05 with 2 komi through racetrack, 100 games
each, which say where F0 stands rather than measure a change:

| Opponent | Result | Earlier |
|---|---|---|
| Tiltak | +95 -4 =1, **+530 [+354, +714]** | v1 +428 [+384, +471] over 800 games, E33 |
| syntaks | +1 -97 =2, **-676 [-965, -426]** | v2 -576 [-665, -481] and E36 -665 [-784, -541], 400 games each |

Against syntaks, 28 games were lost by road, 27 of them by spread, and 69 on flats by a median of 4
half flats with komi; F0 was above +1 late in 15 of its 97 losses, against v2's 61 of 381. Neither
match is long enough to set beside the earlier ones, and at this gap no length of match against
syntaks would be.

A candidate was once built after the per-size networks were removed, with its weights under the
old file name: it compiled, played every size on flats, and lost 274 games of 274 before it was
stopped. Its bench, 3529232, was new and so passed for a different network.

## E40: a handicapped syntaks, a wider network and history by the whole move

The aim is to come within 100 of syntaks at full strength, from E39's -676. At that gap no match
can tell two builds apart, so the measure is syntaks on a fraction of its clock. Everything against
syntaks here is through racetrack on 6x6 with 2 komi, from `6s_4ply.txt` shuffled, 7 games at once
and nothing else running, with Yapper at 5+0.05 and syntaks at 1/k of it.

**The stick.** E39's build against syntaks at 1/k:

| k | syntaks's clock | Games | Result |
|---|---|---|---|
| 4 | 1.25+0.0125 | 100 | +17 -77 =6, **-240 [-325, -153]** |
| 8 | 0.625+0.00625 | 100 | +29 -65 =6, **-130 [-204, -56]** |
| 16 | 0.3125+0.003125 | 100 | +64 -27 =9, **+134 [+61, +206]** |
| 32 | 0.156+0.0016 | 100 | +92 -4 =4, **+477 [+329, +625]**, 8 games lost on time |
| 8 | 0.625+0.00625 | 600 | +233 -327 =40, **-54 [-82, -26]**, score 0.422 |

k=8 is the stick. Three of its 600 games ended on the clock, two of them syntaks's, where k=32
lost 8 of 100. At 1/4 of the time syntaks reached Yapper's median depth, 10, and still won by 240,
so the gap is in what a node is worth more than in how deep the search goes. At full time both
sides use about 6 s of a game's 7.5. Of the base's 95 road losses, 90 were finished by a spread,
and its 231 losses on flats were by a median of 4 half flats.

**A wider network.** E39's recipe and games, seed 0, with only `NN_FILTERS` changed. The kernels
had added the output up in one warp, a filter a lane, which only works at exactly 32: they now
add a warp at a time and then the warps in order, the same arithmetic at 32. The engine sums the
units 256 at a time, since 1024 activations times weights could overflow 32 bits, and a `net.h`
now carries its width so that one of another width cannot compile: C fills a short initialiser
with zeros. Bench was unchanged by all of it, 3378307.

| Fit | 6x6 gen 9, 20k | 4x4 | 7x7 | 8x8 | Speed on bench |
|---|---|---|---|---|---|
| F0, 32 filters | 0.4496 | 0.3741 | 0.5361 | 0.5863 | 4.85M nps |
| 64 filters | 0.4459 | 0.3690 | 0.5356 | 0.5853 | 4.13M, 0.85 times |
| 128 filters | 0.4437 | 0.3651 | 0.5355 | 0.5873 | 3.30M, 0.68 times |

6x6, 4x4 and 5x5 gained most, and 8x8 not at all at 128. The engine reads within 0.5 hundredths
of each fit on average and 1 at worst, on every size. A fit takes about a minute an epoch at 64
filters and 1.3 at 128.

**History by the whole move.** History was kept by square and kind, so every spread from a square
in one direction shared a slot, and a spread east shared one with a flat placed there. Keyed by
the whole 16-bit move instead, 512 kB, bench fell from 3378307 to 2492576 nodes.

| Match | Result |
|---|---|
| Whole-move history against E39, 100,000 nodes | **PASSED**, 588 games, +343 -145 =100, **+121.7 +/- 26.5**, pairs [18, 23, 92, 65, 96] |
| 64 filters against E39, 100,000 nodes | **PASSED**, 810 games, +416 -234 =160, **+79.4 +/- 21.8**, pairs [37, 37, 143, 83, 105] |
| History in the reductions, less by history / 8192, against whole-move history, 100,000 nodes | stopped at 778 games, +315 -297 =166, **+8.0 +/- 21.3** |

Against syntaks at k=8, both with whole-move history:

| Build | Result | Road losses, by spread | Losses on flats |
|---|---|---|---|
| E39 | +233 -327 =40, **-54 [-82, -26]**, 0.422 | 90 | 231 |
| 64 filters | +287 -261 =52, **+15 [-12, +42]**, 0.522 | 68 | 188 |
| 128 filters | +330 -227 =43, **+60 [+32, +87]**, 0.586 | 56 | 168 |

128 filters is the network: 45 above 64 against syntaks despite running at 0.8 of its speed, and
about 114 above E39. Fewer road losses by spread came with it, though nothing here looks for them.

At a time control, 128 filters with whole-move history against E39, 2+0.02: **PASSED**, 504
games, +320 -107 =77, **+156.7 +/- 30.3**, pairs [10, 20, 71, 49, 102].

Bench 3296722 from clang native, clang generic, gcc `popcnt` and `make release`. `make check`
and `make datacheck` pass, and selftest passes 584,540,980 checks. Without `-march=native` the
build runs at 0.7 of native speed, where E39's ran at 0.8: the wider loops want the wider
vectors.

Built and not yet measured: the other side's road by spread looked for before reverse futility
trusts a node, and in the leaf search before a stand pat above alpha is trusted; a two-ply
continuation history; internal iterative reduction.

## E41: four search variants on E40, and Topaz

Each against E40 at 2+0.02, 7 games at once:

| Variant | Bench | Result |
|---|---|---|
| Reverse futility asks first whether the other side has a road by spread | 3125225 | **INCONCLUSIVE** at the cap, 2000 games, +704 -813 =483, **-19.0 +/- 12.8**, pairs [149, 202, 366, 175, 108] |
| The leaf search asks the same before trusting a stand pat above alpha, and plays out every reply if so | 3651819 | **FAILED**, 560 games, +138 -303 =119, **-105.5 +/- 24.7**, pairs [74, 67, 103, 22, 14] |
| A follow-up history, by this side's move before, beside the one by the other side's | 3693396 | **INCONCLUSIVE** at the cap, 2000 games, +733 -795 =472, **-10.8 +/- 13.1**, pairs [149, 187, 379, 147, 138] |
| Internal iterative reduction: a ply less at a PV or cut node from depth 4 with no table move | 2746378 | **INCONCLUSIVE** at the cap, 2000 games, +729 -763 =508, **-5.9 +/- 13.0**, pairs [142, 184, 372, 170, 132] |

None is kept, and `search.c` is E40's. The four are kept as a diff behind switches in
`external/build/search-variants-e41.diff`.

Most of E40's road losses to syntaks are still by spread, but looking for the other side's spread
where the search trusts a quiet position costs more than it finds: a leaf that has to answer one
must try every move, since any spread or wall might stop it, and that is nearly a ply more of
search at every such leaf. The reverse futility test costs a spread search at each node it
would cut, and those are the nodes the search can least afford.

**Topaz, as a second stick.** The perft build kept since E8 is only Topaz's perft example. Its
main branch, 2989527, needs a network kept outside the repository, and none of those published
fit it, so the engine is the newer commit dcb8cd2 on its `back` branch, of 2026-10-30, with that
day's network: built as a black box like syntaks, the source deleted unread. E40 against both
at full strength, 5+0.05, 200 games each:

| Opponent | Result | Score | Road losses, by spread | Losses on flats, median margin |
|---|---|---|---|---|
| Topaz | +55 -111 =34, **-99 [-146, -52]** | 0.360 | 14 of 15 | 96, 2 half flats |
| syntaks | +10 -181 =9, **-442 [-532, -348]** | 0.072 | 49 of 52 | 129, 4 half flats |

Against syntaks that is up from E39's -676 [-965, -426] over 100 games. Topaz at full strength
scores near enough to half to measure a change without a handicap, and both sides reach a median
depth of 10. Most losses to it are close flat games.

## E42: gen 10, its refit, and a second layer that did not pay

**Gen 10.** 120,000 games of 6x6 at 20,000 nodes from E40, seeds 10000 to 10014 on 15 jobs, in 4.9
hours: 6.8 games a second, where E39's 32 filters made 10.8. 11.0M positions. takref replayed 600
of them without a failure, and they read like gen 9's 20,000-node games: a median of 93 plies, 9%
roads and 13% draws.

**F1** is E40's fit with gen 10 added: 190M positions in 13 minutes. It scores better on every 6x6
group, though gen 10 adds only 6% to the positions:

| Held out, against results | Gen 7 | Gen 8 | Gen 9, 20k | Gen 10 |
|---|---|---|---|---|
| E40's network | 0.4507 | 0.4663 | 0.4437 | |
| F1 | 0.4503 | 0.4655 | 0.4429 | 0.4337 |

| Match | Result |
|---|---|
| F1 against E40, 2+0.02 | **INCONCLUSIVE** at the cap, 2000 games, +839 -771 =390, **+11.8 +/- 13.5**, pairs [152, 131, 378, 175, 164] |
| The same fit at seed 1, against E40 | **INCONCLUSIVE** at the cap, 2000 games, +804 -770 =426, **+5.9 +/- 13.5**, pairs [145, 172, 352, 166, 165] |
| F1 against syntaks at 1/8 of 5+0.05, 600 games, beside a GPU fit | +355 -187 =58, **+99 [+71, +127]**, score 0.640 |
| The same, with nothing else running | +298 -250 =52, **+27 [+0, +55]**, score 0.540 |
| F1 against Topaz at 5+0.05, 200 games, beside a GPU fit | +76 -98 =26, **-38 [-85, +8]**, score 0.445 |
| The same, with nothing else running | +50 -123 =27, **-132 [-181, -82]**, score 0.318 |
| F1 against syntaks at 5+0.05, 400 games, nothing else running | +25 -362 =13, **-427 [-488, -362]**, score 0.079 |
| E40 against syntaks at 1/8, 600 games, nothing else running | +313 -245 =42, **+39 [+11, +67]**, score 0.557 |
| E40 against Topaz at 5+0.05, 200 games, nothing else running | +64 -116 =20, **-92 [-140, -43]**, score 0.370 |

**A match against another engine depends on the machine being quiet.** The same F1 binary read 72
higher at an eighth and 94 higher against Topaz while a GPU fit held two of the sixteen threads,
which is far past chance for either. A busy machine seems to cost the other engine more than
Yapper, and most of all syntaks on an eighth of the clock, with its 10 ms `MoveOverhead`. E40's
+60 [+32, +87] and -99 [-146, -52] were not taken on a known quiet machine either. Run again alone,
E40 leads F1 by 12 at an eighth and by 40 against Topaz. Both are inside the noise, but they lean
the other way from self-play's +11.8.

F1 is kept on its self-play and its fit, as E38 kept D1. Of its 362 losses to syntaks at full
strength, 264 were on flats by a median of 4 half flats, and 95 of the 98 roads were finished by a
spread. In 60 of them Yapper scored itself above +1 in the game's last third.

**A second layer.** Each filter's eight units are pooled into one, the sum of their clipped squares.
Turning the board only permutes the eight, so the pool cannot change. Sixteen units then read the
128 pools and the skip inputs, clipped to [0, 1], with weights per size and bucket, and the output
adds them to the straight sum of the pools it had before. The engine cuts each pool to 16 bits,
`(sum + 16) >> 5`, which the fit reads as `NN_QA^2 / 2^16` of it, so both layers are 16-bit
products into 32-bit sums. The engine read within 0.50 hundredths of the fit on average and 2 at
worst. Selftest passed its random networks through the layer on every size, and the GPU and CPU
fits agreed to float rounding. It fitted better on every 6x6 group, by 0.0012 to 0.0016, about
three times what one seed moves the loss.

| Second layer | Result |
|---|---|
| Against F1, 2+0.02 | **FAILED**, 1436 games, +494 -648 =294, **-37.4 +/- 16.1**, pairs [146, 132, 253, 104, 83] |
| Against F1, 100,000 nodes each, 600 games | +263 -238 =99, **+14.5 +/- 25.1**, pairs [44, 41, 119, 38, 58] |
| Against syntaks at 1/8, 600 games, beside F1's rerun on a quiet machine | +300 -249 =51, **+29 [+2, +56]**, F1 +27 |
| Against Topaz at 5+0.05, 200 games, the same | +51 -123 =26, **-130 [-179, -80]**, F1 -132 |

The fit was worth about 15 at equal nodes, and the speed cost more than that. As first written the
engine ran at 0.64 of F1's nodes a second. Timed on its own, F1 scores a position in 50 ns and the
layer took 210. Inlined, clang never stored the pools as 16-bit numbers: it widened all 128 once,
spilled them, and packed them back for every row with half-width multiply-adds. Pooling in a
function of its own, four rows a pass, and the full-width pragma on every loop brought it to 158 ns
and 0.72 of F1's speed, with the bench unchanged. That is about 75 ns for the rows and 35 more
for pooling than F1's whole score. Two orientations of a filter side by side, so that one
multiply-add squares both, timed slower. Eight units fitted between, at 0.50460 held out against
16's 0.50434 and F1's 0.50484, and ran at 0.82 of F1. Neither is kept, and the code is in
`external/build/l2`.

Across E40 to E42 a fit's gain played at about 10 Elo at equal nodes for each 0.001 of held-out
loss: the width, gen 10 and the second layer all fall near it. A network that costs speed has to
buy more loss than that. 256 filters needed the trainer's kernel to take two hidden units a thread,
since 2048 is over CUDA's 1024 to a block. That kernel is kept in `external/build/w256`, and it
matches the CPU to float rounding.

**The recipe**, on F1's games and seed:

| Change | Result |
|---|---|
| lambda 0.85 on 6x6, for 0.7 | **INCONCLUSIVE** at the cap against F1, 2000 games, +767 -816 =417, **-8.5 +/- 13.5**, pairs [170, 157, 367, 164, 142] |
| 25 epochs, for 15 | 0.50497 held out against F1's 0.50484, so not played |
| 256 filters, with the kernel taking two units a thread | 6x6 held out between F1's and the second layer's. On a quiet machine, +285 -273 =42, **+6 [-20, +34]** against syntaks at 1/8, and +39 -128 =33, **-166 [-215, -115]** against Topaz, at about 0.65 of F1's speed |

**The clock**, each against F1 at 2+0.02:

| Change | Result |
|---|---|
| Three times the budget at the start, falling to none extra by half the stones down | **INCONCLUSIVE** at the cap, 2000 games, +706 -835 =459, **-22.4 +/- 12.9**, pairs [166, 178, 384, 163, 109] |
| Half the budget at the start, the same way | **INCONCLUSIVE** at the cap, 2000 games, +718 -770 =512, **-9.0 +/- 12.2**, pairs [121, 204, 385, 186, 104] |
| No new iteration past 60% of the budget, for 50% | **INCONCLUSIVE** at the cap, 2000 games, +727 -767 =506, **-6.9 +/- 12.4**, pairs [127, 187, 396, 179, 111] |

syntaks spends its clock on the opening, as below, but the same did not help Yapper, and neither
did the opposite. The budget stays as it was.

**The search**, at 2+0.02:

| Change | Bench | Result |
|---|---|---|
| Move-count pruning at half: after `3 + 1.5d^2` moves, `6 + 3d^2` when improving | 1388639 | **INCONCLUSIVE** at the cap against F1, 2000 games, +815 -702 =483, **+19.7 +/- 12.9**, pairs [116, 164, 365, 201, 154]. Kept |
| Reductions `/ 1.75`, for `/ 2.0` | 1788389 | **INCONCLUSIVE** at the cap against F1, 2000 games, +755 -762 =483, **-1.2 +/- 12.9**, pairs [128, 195, 370, 170, 137] |
| Pruning at a quarter, `2 + d^2` and `4 + 2d^2`, on the half | 1346183 | **INCONCLUSIVE** at the cap against the half, 2000 games, +758 -744 =498, **+2.4 +/- 12.9**, pairs [131, 188, 348, 202, 131] |
| The half to depth 8, for 6 | 1399090 | **INCONCLUSIVE** at the cap against the half, 2000 games, +738 -731 =531, **+1.2 +/- 12.7**, pairs [132, 180, 356, 213, 119] |

On a quiet machine the half read +301 -255 =44, **+26 [+0, +54]** against syntaks at 1/8 and +61
-110 =29, **-86 [-133, -38]** against Topaz, where F1 read +27 and -132. With E36's ns5 that makes
two steps where tighter move-count pruning gained, and the third did not.

F1's network and the half both stopped at the cap, so the build as a whole was tested against
E40, as E36 settled ns1 and ns2:

| Conditions | Result |
|---|---|
| E42 against E40, 2+0.02 | **PASSED**, 1556 games, +692 -528 =336, **+36.8 +/- 14.8**, pairs [81, 112, 287, 158, 140] |

Against the other engines on a quiet machine, E42 sits within the noise of E40, which 600 and 200
games cannot resolve at this size. Self-play's +36.8 is the result that settles it.

**Where syntaks spends.** Both engines use about 6.1 to 6.4 s of a 7.5 s game. syntaks spends 0.36
s a move over the first 15 plies to Yapper's 0.12, and less than Yapper from ply 30 on. It reports
2 to 3 plies more throughout, even where it spends less. Given one second on a position just after
the book, syntaks reached depth 17 on 1.48M nodes at 1.52M a second, each ply 1.5 times the last.
Yapper reached 14 on 1.93M at 3.6M a second, at 1.8 to 2.4 times. That is E34's and E36's gap,
still there. syntaks's `MoveOverhead` is 10 ms by default, which at 1/8 is more than its 6.25 ms
increment, so that handicap takes more from it than the clock alone says.

## E43: what the network costs, and what it cannot see

Measurements of E42's network from an instrumented copy of the source, kept with its diff in
`external/build/e43`. Nothing here changes the engine, and the copy benches 1388639, as E42 does.

**Where a node's time goes.** 120 positions from gen 10 games, each searched to 200,000 nodes from
an empty table on a quiet machine, with cycles counted around the network inside the search. The
search ran at 1263 to 1271 cycles a node, and the counters added 3%.

| Part | Cycles an evaluation | Share of the search |
|---|---|---|
| Bringing the accumulator up to date | 615 | 33.7% |
| Scoring it | 172 | 9.4% |

71% of nodes evaluate. A frame comes from an ancestor 2.91 plies back on average, through 3.88
changed squares and 5.27 rows of 1024. Most of the network's cost is the update, not the score.

The table a search reads repeats each filter row once per symmetry, 21 MB on 6x6. Reading the
trainer's layout instead, an eighth of the size, ran at 1290 to 1307 cycles a node, so the update
is not waiting on memory. Applying all of a carry's rows in one pass, an orientation at a time in
eight vector registers, from the ancestor's accumulator straight into the frame, ran at 1197 and
1211 cycles against 1263 and 1271: **about 5% faster**, with the update at 526 cycles, the bench
unchanged and selftest clean. E32's single pass was slower because the compiler did not keep the
slice in registers. The vector types that movegen already uses make it do so.

That pass is now `nn_derive()`. Its vectors are as wide as the target's registers: eight of 32
bytes on SSE took all sixteen registers and spilled, and gcc with POPCNT, CI's build, ran 14%
slower than before until they were cut to 16 bytes there. Then, alternating builds on the same
machine:

| Build | Before | After |
|---|---|---|
| clang native, the 120 positions, cycles a node, median of 4 | 1339 | 1260, **6% faster** |
| gcc POPCNT, bench nps, 3 runs | 2.34M to 2.37M | 2.79M to 2.81M, **+19%** |
| clang generic, bench nps, 3 runs | 2.35M to 2.45M | 2.59M to 2.62M, **+8%** |

Bench is 1388639 from clang native, clang generic, gcc POPCNT and `make release`, perft and
selftest pass, and gcc's selftest passes too. A pure speedup, so no match was played.

**What the units do.** On the 532,651 positions of gen 10's held-out games, from the side to
move's view, 77.7% of units are below zero, 2.5% are at the clip and 19.8% are between. Every
position has an eighth to a quarter of the layer live. No unit is never live, and 56 are below
zero in 95% of positions. The largest accumulator in all 8.5M positions of gen 10 is 2280, where
16 bits hold 32767.

**Where the score comes from.** The hidden layer's part has a standard deviation of 288 hundredths
and the linear skip's 445. They correlate with the search's score at 0.42 and 0.80, the flat lead
alone at 0.48 and their sum at 0.96. The lead, reserves and komi, linear within each bucket, carry
most of the score, and the hidden layer is a positional correction on top of them.

**Where it falls short of the search**, by bucket on the same positions, against results:

| Bucket | Positions | Network | Skip alone | 20,000-node search | Network minus search | Mean difference from the search |
|---|---|---|---|---|---|---|
| 0, the end | 20,102 | 0.4238 | 0.5339 | 0.3404 | 0.0835 | 245 |
| 1 | 40,164 | 0.3320 | 0.4306 | 0.2831 | 0.0489 | 205 |
| 2 | 42,772 | 0.3351 | 0.4344 | 0.2998 | 0.0353 | 158 |
| 3 | 37,286 | 0.3614 | 0.4623 | 0.3265 | 0.0349 | 137 |
| 4 | 61,974 | 0.3797 | 0.4788 | 0.3493 | 0.0304 | 123 |
| 5 | 78,145 | 0.4236 | 0.5197 | 0.3940 | 0.0296 | 106 |
| 6 | 107,573 | 0.4644 | 0.5620 | 0.4396 | 0.0248 | 89 |
| 7, the start | 144,635 | 0.5168 | 0.6185 | 0.5015 | 0.0153 | 59 |

The network trails the search by 0.015 at the start and 0.084 at the end, where the two differ by
2.45 flats on average. Part of that is the search seeing the end, but it is also where the games
are lost: most losses to syntaks are flat games by 2 to 4 half flats, E42. The hidden layer is
worth about 0.1 of loss over the skip alone in every bucket.

**What the stacks are fitted on.** Each filter row, by cell and token, was counted over the 144M
6x6 positions of gens 7 to 10 that a fit reads, and then over gen 10's held-out positions. Every
lone stone, 78% of what a position reads, reads a row seen more than a million times. Of the stacks,
85.5% read such a row and 2.65% one seen fewer than 100,000 times. 319 of the 18,432 filter rows
never moved from their random start, and no position, in a game or a search, read one. 6x6's token
table is not short of data.

**What it cannot see.** A black top subtracts the row that the same stack under a white top adds,
so every unit weighs a stack of the other side's at exactly minus the same stack of its own. A
unit can only measure the difference between the sides, never a total: how full the board or a
line is reaches the network only through the eight buckets. A wall of the other side's across a
road, and a wall of this side's in the same place, are equal and opposite in every unit, though
only one of them blocks. E35's stones in hand went into the hidden layer under the same rule.

**Clips.** The fit holds biases within 1.98, and 19 of 8x8's 128 are at that limit, with 3 of
7x7's and 1 of 4x4's. Accumulators would leave room for far more. Output weights span -34 to 21
on 6x6 and -15 to 16 on 8x8, of the 127 the fixed point allows, so most are within a few steps of
1/64. 5713 filter weights, 0.24%, are at their limit of 1.5.

## E44: a view for each side

E43 found that one view, which a black top subtracted from, could only weigh the difference
between the sides. Now each side has a view of its own, with weights for a stack of its own and
for one of the other side's, and the output weighs the side to move's view apart from the other.
Both views are the same weights, so swapping the colours and the side to move swaps the views. At
64 filters a view, the accumulator is 1024 wide as before, a changed stack still costs one row of
1024, and the table is the same size: a stack's row holds its owner's view first, and a black top
sends the halves the other way.

Selftest passes 584,543,208 checks with random networks of two views on every size. A score that
read white's view whoever was to move failed 101,832 of them, and an update that took every stack
for black's failed 222,132. The GPU and the CPU fit one file for two epochs to 0.601785 and
0.601804 held out, and the engine reads within 0.50 hundredths of every size's fit, 1 at worst.

The fit is F1's, E42: the same games, `lambda`, shares, epochs and seed.

| Held out, against results | F1, one view of 128 | Two views of 64 | Change |
|---|---|---|---|
| 6x6 gen 10 | 0.4337 | 0.4292 | **-0.0045** |
| 6x6 gen 9, 20k | 0.4429 | 0.4382 | -0.0046 |
| 6x6 gen 8 | 0.4655 | 0.4616 | -0.0040 |
| 6x6 gen 7 | 0.4503 | 0.4471 | -0.0032 |
| 3x3, 4x4, 5x5 | 0.3197, 0.3655, 0.4136 | 0.3141, 0.3602, 0.4089 | -0.005 |
| 7x7, 8x8 | 0.5353, 0.5867 | 0.5333, 0.5869 | -0.002, 0.000 |

Against the blended targets the gain was only 0.0010, against results 0.0033: the scores in the
targets are a one-view network's.

| Match | Result |
|---|---|
| Against E43's build, 2+0.02, [0, 10] | **PASSED**, 368 games, +202 -111 =55, **+87.7 +/- 31.9**, pairs [14, 16, 68, 37, 49] |
| syntaks at 1/8 of 5+0.05, 600 games, nothing else running | +335 -194 =71, **+83 [+55, +110]**, score 0.618, against E42's +26 |
| Topaz at 5+0.05, 200 games, the same | +84 -74 =42, **+17 [-27, +61]**, score 0.525, against E42's -86 |

The first time Yapper has scored above half against Topaz at full strength. By E42's rule of 10
Elo at equal nodes for each 0.001 of held-out loss, the fit was worth about 45; it played twice
that.

**What it costs.** On E43's 120 positions, the update costs 545 cycles an evaluation to the one
view's 535. Scoring each view in a loop of its own took 217 cycles to the one view's 180, and one
loop over both, a step adding a unit of each, takes 196. The search then runs at 1252 cycles a
node to 1232, 1.5% slower. The matches above played the two-loop build, the same evaluation 1%
slower.

Bench 1347007 from clang native, clang generic, gcc POPCNT and `make release`. Perft and
selftest pass, gcc's selftest too.

## E45: output buckets by the race to the end, a neutral test

E43 found the network furthest behind the search at the end of the game, and most losses are
close flat games. The buckets were the smaller of the stones either side has left and the empty
squares, which cannot say who places last. These were the placements left if both sides only
place, `min(empty, 2 * mine - 1, 2 * theirs)`, odd when the side to move places last: 16 buckets,
each count from 1 to 8 alone, then 9-12, 13-18, 19-26 and more, each split by who places last.
The rest is E44's network, fit and seed.

| Held out, against results | E44 | Race buckets |
|---|---|---|
| 6x6 gen 10 | 0.4292 | 0.4280 |
| 6x6 gen 9, 20k | 0.4382 | 0.4388 |
| 6x6 gen 8, gen 7 | 0.4616, 0.4471 | 0.4617, 0.4471 |
| 4x4, 5x5 | 0.3602, 0.4089 | 0.3606, 0.4103 |
| 7x7, 8x8 | 0.5333, 0.5869 | 0.5327, 0.5858 |

Against E44, 2+0.02, [0, 10]: **INCONCLUSIVE** at the cap, 2000 games, +759 -745 =496,
**+2.4 +/- 12.7**, pairs [128, 174, 382, 188, 128]. Not kept. What a flat is worth at the end is
not what the buckets were missing, or the search already sees who places last.

## E46: units multiplied in pairs, a neutral test

The output weighed each unit squared. Here it weighs filter f times filter f + 32, of one view
and orientation, so the symmetries still hold: half as many products, each of two patterns. The
fit is E44's.

| Held out, against results | E44 | Pairs |
|---|---|---|
| 6x6 gen 10, gen 9 | 0.4292, 0.4382 | 0.4296, 0.4394 |
| 6x6 gen 8, gen 7 | 0.4616, 0.4471 | 0.4617, 0.4473 |
| 4x4, 5x5 | 0.3602, 0.4089 | 0.3612, 0.4093 |
| 7x7, 8x8 | 0.5333, 0.5869 | 0.5325, 0.5859 |

Worse on every 6x6 group, better on 7x7 and 8x8. The score took 188 cycles to 208, both views in
one loop, and the search 1257 cycles a node to 1279. Against E44, 2+0.02, [0, 10]:
**INCONCLUSIVE** at the cap, 2000 games, +769 -792 =439, **-4.0 +/- 12.8**, pairs [138, 178,
381, 175, 128]. Not kept: what the speed bought, the fit gave back.

## E47: shared rows in the fit, for the sizes with few games

Two blocks only the fit has: a row for each token, as its own side's or the other's, that every
cell shares, and a row for each cell and top type. A filter weight plays as its own plus those
two, rounded, so the engine is given the sum and nothing in it changes. A shared row's gradient is
the sum of its filter weights' gradients, added after each batch rather than in the kernel. The
shared block first started 32 past a multiple of 64, and the CPU read its filters from the whole
array's index: on one file for two epochs the CPU then fitted 0.588026 held out to the GPU's
0.583373. Fixed, 0.583393 to 0.583373. That file alone without the shared rows fitted 0.601785.

On E44's fit:

| Held out, against results | E44 | Shared rows |
|---|---|---|
| 6x6 gen 10, gen 9 | 0.4292, 0.4382 | 0.4291, 0.4387 |
| 6x6 gen 8, gen 7 | 0.4616, 0.4471 | 0.4618, 0.4470 |
| 3x3, 4x4, 5x5 | 0.3141, 0.3602, 0.4089 | 0.3128, 0.3600, 0.4101 |
| 7x7, 8x8 | 0.5333, 0.5869 | 0.5325, **0.5847** |

6x6 is where E43 found the rows well fed, and nothing moved there. 8x8, with the fewest games for
its squares, gained 0.0022. Not played yet: no 6x6 match could see it, and it belongs with the
next fit, on new games of 7x7 and 8x8. The trainer is in `external/build/p5`.

## E48: a head of two layers

The user asked for a standard chess idea: small layers between the main one and the output.
Each filter's eight units, squared, are pooled into one, in each view, so the 128 pools cannot
change when the board is turned. 16 clipped units read the pools and the lead, reserves and komi,
32 clipped units read those, and the output adds the 32 to the straight sum of the pools it had
before, which keeps the network's old judgement as the head's starting point. Every size and
bucket has its own head. In the engine a pool is cut to 16 bits, as in E42, and each layer's
units are cut back to 16 bits before the next reads them, so every product is 16 by 16 bits into
32 and the score fits 64. Selftest passes 584,523,449 checks with random heads on every size.

On E44's fit:

| Held out, against results | E44 | The head |
|---|---|---|
| 6x6 gen 10 | 0.4292 | **0.4254** |
| 6x6 gen 9, 20k | 0.4382 | 0.4349 |
| 6x6 gen 8, gen 7 | 0.4616, 0.4471 | 0.4592, 0.4452 |
| 3x3, 4x4, 5x5 | 0.3141, 0.3602, 0.4089 | 0.2883, 0.3528, 0.4072 |
| 7x7, 8x8 | 0.5333, 0.5869 | 0.5327, 0.5857 |

The engine reads within 0.52 hundredths of every size's fit, 2 at worst. E42's head of one layer
on one view fitted 0.0016 better on 6x6; this one 0.0038, and against E44 at 100,000 nodes a move
each, [0, 10]: **PASSED**, 338 games, +175 -93 =70, **+86.0 +/- 31.3**, pairs [9, 19, 61, 41, 39].
E42's played +14.5 there.

**What it costs, and what the compiler made of it.** As first written the score took 980 cycles to
E44's 197, and the search ran at 0.69 of the speed: four rows of the first layer in one loop were
widened to 32-bit multiplies, and the second layer was built a word at a time. Each part was
timed alone, and the candidates, on data in L1, in cycles a call:

| Part | clang | gcc |
|---|---|---|
| Pooling both views, squares summed in 32 bits | 269 | 214 |
| The same, each square cut to 16 bits first, in GCC's vectors | **122** | 158 |
| First layer, four rows a loop | 227 | 93 |
| First layer, a row a call | **128** | 131 |
| Second layer, rows inline | 257 | 289 |
| Second layer, a row a call | **117** | 149 |
| Second layer, by column | 124 | 801 |

Cutting each square before the sum moves a pool by at most 1 of 2048. With the three in bold, the
score takes about 520 cycles and the search runs at about 0.8 of E44's speed.

| Match | Result |
|---|---|
| Against E44, 2+0.02, [0, 10] | **PASSED**, 512 games, +243 -164 =105, **+54.0 +/- 24.7**, pairs [22, 25, 107, 56, 46] |
| syntaks at 1/8 of 5+0.05, 600 games, nothing else running | +349 -174 =77, **+104 [+76, +131]**, score 0.646, against E44's +83 |
| Topaz at 5+0.05, 200 games, the same | +93 -65 =42, **+48 [+3, +93]**, score 0.570, against E44's +17 |
| syntaks at 5+0.05, 400 games, the same | +25 -356 =19, **-410 [-467, -349]**, score 0.083, against E42's -427 |

Against syntaks at full strength nothing measurable moved, where at an eighth of its clock E42 to
E48 gained 78 and Topaz 134. At full time syntaks searches two to three plies deeper, E42, and
that is where the rest of the gap is.

The GPU and the CPU fit one file for two epochs to 0.601273 and 0.601208 held out. Bench 1495587
from clang native, clang generic, gcc POPCNT and `make release`; perft and selftest pass, gcc's
selftest too.

## E49: gen 11, and E48 fine-tuned on it

**Gen 11.** Rounds over 5x5 to 8x8 at 20,000 nodes a move from E48, each round 50,000 games of
6x6, 22,000 of 7x7, 17,000 of 8x8 and 12,000 of 5x5, on 15 jobs, with seeds of 110000 + 1000 a
size + 100 a round. A round took 5.7 hours, at 6.1, 4.0, 3.0 and 10.3 games a second. Four rounds
were planned; the user stopped it after two to try the games: 202,000 games, 21.5M positions.
takref replayed 1010 of them without a failure. Roads end fewer games than before: 8% on 6x6,
where gen 10 had 9%, and 12% and 11% on 7x7 and 8x8, where the gen 1 games of those sizes had 30%.

**Fine-tuning.** `nntrain --warm` starts from the network the binary was built with, and `--rate`
sets the first step's rate. A warm fit at a rate of 1e-12 wrote back `net.h` byte for byte. Trials
on a quarter of the positions, with E48's games and the first round, for 2 or 3 epochs:

| Held out, against results | Start | 0.0005, gen 11 4× | 0.002, gen 11 once | 0.001, gen 11 4× |
|---|---|---|---|---|
| 6x6 gen 11 | 0.4237 | **0.4196** | 0.4223 | 0.4196 |
| 7x7 gen 11 | 0.4639 | **0.4571** | 0.4592 | 0.4570 |
| 8x8 gen 11 | 0.5190 | **0.5125** | 0.5132 | 0.5125 |
| 6x6 gen 10 | 0.4260 | **0.4227** | 0.4252 | 0.4227 |

Listed once, a quarter of gen 11 is drawn from other positions, and its start reads 0.4232, 0.4628
and 0.5180. Gen 11 is a twentieth of the positions, and listed once it hardly moved the network,
even at the full rate. Listed four times it moved every group of games, the old ones included, and the rate
made no difference. Listing a file again weighs it again; its held-out games are counted again
too, so a group's mean does not change.

The fit: E48 warm, E48's games and both rounds listed four times, every position, at 0.0005 for 2
epochs. 254M positions, in 4 minutes:

| Held out, against results | E48 | E49 |
|---|---|---|
| 6x6 gen 11, gen 10 | 0.4230, 0.4254 | **0.4160, 0.4199** |
| 6x6 gen 9, 8, 7 | 0.4349, 0.4592, 0.4452 | 0.4302, 0.4577, 0.4441 |
| 7x7 gen 11, gen 1 | 0.4724, 0.5327 | **0.4630**, 0.5310 |
| 8x8 gen 11, gen 1 | 0.5138, 0.5857 | **0.5041**, 0.5846 |
| 5x5 gen 11, gen 1 | 0.4043, 0.4072 | **0.3876**, 0.3985 |
| 3x3, 4x4 | 0.2883, 0.3528 | 0.2881, 0.3532 |

The engine reads within 0.52 hundredths of the fit on average, 0.68 on 3x3, and 5 at worst there.

| Match | Result |
|---|---|
| Against E48, 2+0.02, [0, 10] | **PASSED**, 1324 games, +518 -429 =377, **+23.4 +/- 15.4**, pairs [68, 113, 231, 162, 88] |
| The same games fitted from nothing, against E49, 2+0.02, [0, 10] | **FAILED**, 680 games, +222 -278 =180, **-28.7 +/- 20.5**, pairs [47, 69, 146, 49, 29] |
| syntaks at 1/8 of 5+0.05, 600 games, nothing else running | +395 -150 =55, **+150 [+120, +179]**, score 0.704, against E48's +104 |
| syntaks at 1/4 of 5+0.05, 600 games, the same | +252 -282 =66, **-17 [-44, +9]**, score 0.475: the stick from here on |
| Topaz at 5+0.05, 200 games, the same | +112 -54 =34, **+103 [+55, +150]**, score 0.645, against E48's +48 |
| syntaks at 5+0.05, 400 games, the same | +27 -351 =22, **-391 [-446, -334]**, score 0.095, against E48's -410 |

The fit from nothing took the usual 15 epochs at 0.002, in 21 minutes, and held out level with
E49 on 6x6, 0.4162 on gen 11 and 0.4201 on gen 10, a little behind on 7x7's 0.4636 and 8x8's
0.5077, and ahead on 5x5's 0.3862. It still lost on 6x6, though by no more than two seeds of one
fit can play apart, E39, so this says the fine-tune is no worse, not that it is better. It is a
fifth of the time.

Against the other engines the 23 of self-play read as about 50: 46 more at an eighth of syntaks's
clock and 55 more against Topaz. syntaks at full strength moved 19, inside either interval. At
0.704, an eighth no longer measures much, so syntaks at a quarter, 1.25+0.0125, is the stick
for the next changes. Doubling its clock cost E49 167.

The match is on 6x6, where the fit moved least; 7x7, 8x8 and 5x5 moved more and have not been
played. Gen 10, added by a fit from nothing, moved its groups by about 0.001 and played +11.8 at
the cap, E42. Bench 1209000 from `make` and `make release`, and selftest passes.

## E50: move ordering and search from Tak's own structure

Eleven ideas, each built on something only Tak has: roads and their ends, spread patterns and the
stack's bits, tops and reserves. Nothing taken from chess. Each was a switch on E49's search, tested
alone against the build with every switch off, which benched E49's 1209000.

**Measured first**, on 150 positions from E49's games against syntaks at a quarter and against
Topaz, every 7th ply from the 12th, each searched to depth 11 from an empty table. Time is the
search alone:

| Change | Nodes | Time |
|---|---|---|
| A road to answer plays only what can stop it, below | -20% | -7.4% |
| A block placed by whether the other side can cover it | -8.5% | -8% |
| A spread scored by the tops it wins | -2% | +5% |
| Placements that leave a road to finish | +1% | 0% |
| Placements where the other side would leave one | +5% | +5% |
| Walls with no purpose last | -1% | +1% |
| Placements first when ahead near the end, spreads when behind | +2% | +5% |

**The answer filter** is exact. With the other side able to place a road on squares T, a spread
can only help if, for every square of T, it lands on the square or on a stone of a group the
square would join; anything else leaves that road to be placed, and the child returns a win on
the spot. With two squares in T, no placement can help at all. The search already had no road of
its own by then, and `last_stones()` keeps out the flat count, so this is the same tree less
nodes that lose at once. Selftest plays every move it turns away and requires the road to be
there after it. A copy that turned away every spread failed 47,518 of those checks.

At 2+0.02 on 6x6 with 2 komi, 7 games at once, against the switches off:

| Change | Bench | Result |
|---|---|---|
| The answer filter, a non-regression test at [-20, 5] | 1053668 | **PASSED**, 356 games, +134 -115 =107, **+18.6 +/- 28.6**, pairs [18, 29, 66, 46, 19] |
| Block type: a wall or capstone first on a square the other side can reach, a flat first otherwise | 1518872 | INCONCLUSIVE at the cap, 2000 games, +707 -704 =589, **+0.5 +/- 12.0** |
| Near: +8192 for a flat or capstone that leaves a road to finish, the mask from the ends a node already has | 1370690 | INCONCLUSIVE at the cap, 2000 games, +738 -710 =552, **+4.9 +/- 12.3** |
| Swing: 256 a top for a spread, ours less theirs, before against after | 1776565 | INCONCLUSIVE at the cap, 2000 games, +733 -701 =566, **+5.6 +/- 12.5** |
| Block: +4096 for any placement where the other side's mask is | 1283760 | INCONCLUSIVE at the cap, 2000 games, +717 -691 =592, **+4.5 +/- 12.4** |
| Walls: -4096 for a wall beside nothing of theirs and off their mask | 1471502 | INCONCLUSIVE at the cap, 2000 games, +692 -718 =590, **-4.5 +/- 12.5** |
| End: +4096 to flat placements when ahead, to spreads when behind, with 6 stones or squares left | 1305733 | INCONCLUSIVE at the cap, 2000 games, +730 -724 =546, **+1.0 +/- 12.3** |
| Near, block and swing together | 1879457 | **PASSED**, 1632 games, +627 -535 =470, **+19.6 +/- 13.8**, pairs [83, 139, 310, 171, 113] |

E21 found that past the 8th move, moves that make a threat are 3.7% of the moves and a third of
the cutoffs. Sparing them from pruning and reductions lost, E21 and E36. Sorting them earlier was
untried, and measures about +5. History already learns most of what one static prior says, and
the three together say enough more to pass.

**Three that lost**, against a rebuild of the switches off from the same tree:

| Change | Result |
|---|---|
| Tinue: a prover for forced roads, below. A road the side to move can force is played at once, and the move each depth prefers is set aside if it hands the other side one | **FAILED**, 1550 games, +538 -580 =432, **-9.4 +/- 13.6** |
| Silhouettes: at depth 2 or less off the PV, a spread that leaves the same tops as one already failed low is skipped | **FAILED**, 650 games, +201 -257 =192, **-30.0 +/- 21.1** |
| A leaf whose stand pat would count asks whether the other side has a road by spread, and if so plays only walls and capstones on its lines and spreads that touch them or its road stones | **FAILED**, 522 games, +154 -226 =142, **-48.2 +/- 25.8** |

The prover's attacker plays only moves that leave a road to finish, and its defender only the
answer filter's moves, so its trees are narrow, and anything it leaves out can only make a proof
fail. Selftest checked every proof it found on random games of 3x3 to 6x6 by trying every move
for both sides, and a prover whose defender never spread failed 6,056 of those. On the 150
positions it found no forced road the search had missed, and set aside root moves that handed one
over at plies 68, 75 and 82. Asking again at each depth cost 48% of the search's time, because a
question cut short by its budget remembers nothing; asking each move once cost 3%. The few roads
it finds do not pay for asking at every move.

Spreads that leave the same tops, by line, hops, whose stone ends on each and on the source, were
8.8% of the spreads searched at depth 2 or less off the PV and 11.7% deeper, and such a repeat beat
alpha 0.12% and 0.05% of the time. Skipping them at depth 2 or less took 4.8% off the time, and
lost 30: what a spread buries matters, and a skipped move does not count towards move-count
pruning, which lets later moves in.

The leaf search is E41's -105.5 with the reply set narrowed. Only a wall or capstone on its line
stops a spread among the placements, since it lands on a flat, and only a spread that touches the
line or the road stones can change either. It still cost 19% of the time at depth 11, and lost.
That is the third leaf that looks for the other side's spreads and loses.

Two more went no further than measuring. Whether the side to move has a road by a spread, kept in
the top bit of a table entry's depth, left bench unchanged and moved the time by -2.3% and +0.3% in
two runs: the check after the prunes is no longer a large part of a node. Asking the prover of
every root move, not only the preferred one, was estimated at about 6 ms a move at 2+0.02, a tenth
of the clock, and not built.

**The answer filter and the three priors together**, as staged, bench 1621886 from `make` and
`make release`:

| Match | Result |
|---|---|
| Against the switches off, 2+0.02, [0, 10] | **PASSED**, 940 games, +373 -294 =273, **+29.3 +/- 17.6**, pairs [36, 88, 169, 115, 62] |
| syntaks at 1/4 of 5+0.05, 600 games, nothing else running | +238 -295 =67, **-33 [-60, -6]** |
| E49 again, straight after, the same | +232 -302 =66, -40 [-67, -13], where E49's own run read -17 |
| Topaz at 5+0.05, 200 games, the same | +100 -55 =45, **+79 [+34, +123]** |
| E49 again, the same | +97 -55 =48, +74 [+29, +117], where E49's own run read +103 |

Against E49 run beside it, E50 is 7 and 5 ahead, inside either interval, and E49 itself moved by
23 and 29 since its own run: a gauntlet only compares with one run under the same conditions.
Against syntaks, E50 lost 209 games on flats to E49's 225 and won 152 there to 143, and was above
+1 late in 40 of its losses to 52. It lost 86 by road to 76, 82 of them by spread to 66: about 1.3
standard errors, so a lead and not a finding, but the way E36's more selective search went too.

The other variants are kept behind their switches in `external/build/tak/search-variants-e50.diff`,
with the prover's `tinue.c` and `tinue.h` beside it. `make check` passes.

## E51: a move prior fitted on the games' own moves

**Where the ordering stood.** An instrumented E50, on the 150 positions of E50 at depth 11. The
cutting move's place among the moves tried, at nodes off the PV that cut:

| Depth left | Cuts | 1st | 2nd | 3-4 | 5-8 | 9-16 | 17+ |
|---|---|---|---|---|---|---|---|
| 1 | 2433038 | 81.5% | 8.7% | 7.7% | 1.6% | 0.3% | 0.2% |
| 2 | 950960 | 77.1% | 10.3% | 7.3% | 3.8% | 1.3% | 0.2% |
| 3-4 | 900990 | 82.7% | 8.6% | 5.1% | 2.0% | 1.1% | 0.6% |
| 5+ | 229329 | 86.2% | 6.7% | 4.1% | 1.5% | 0.7% | 0.9% |

At one in 16 of the nodes where move-count pruning stopped, the moves it would have left were
searched anyway: in 11.0% of 113,679, one of them cut. Of those 12,478 moves, 8507 were spreads,
7477 of which won tops, and 3971 placements, 6 of them near squares. 44% sat 17 or more moves past
the limit.

**The prior.** `policy.c` scores a move by a weight for each feature it has, all read the same
under the board's symmetries and from either side: for a placement, its stone by the game's phase,
its ring, a corner, how many of each kind of neighbour it has, whether it is on either side's near
squares, the other side's road squares or either side's ends, and how the flat race stands near
the end; for a spread, its carry, hops, whether it lifts the whole stack, the tops it wins by
phase, the stone on top, what it leaves behind, the tallest of the other side's stacks it covers,
how many near squares and road squares it lands on, a crush, the race and the ring it ends on. 262
weights. `yapper policyfit` fits them as a softmax over each scored ply's legal moves, aimed at the
move the search played, by Adagrad, on half of the 6x6 games of gens 10 and 11 and one game in 16
held out. 9.0M positions, 3 epochs, 5.5 minutes on one thread. The engine's own `policy_logit()` on
the rounded weights scores the held-out games as the fit does:

| Held out, 1.12M positions | Top 1 | Top 3 | Top 8 | Mean rank | Loss |
|---|---|---|---|---|---|
| The fitted prior | 14.4% | 32.6% | 58.7% | 10.42 | 3.2507, uniform 4.5324 |
| E50's hand-set prior | 8.7% | 21.7% | 43.7% | 15.79 | |

The loss moved 0.003 after the first epoch: a weight per feature is all this can learn from any
amount of games. Selftest holds every move's prior unchanged under the eight symmetries and a
colour swap, on random weights; a ring read off the file alone failed 3.6M of those checks.

**In the ordering**, from depth 1 or 2, the prior times a weight in place of E50's hand-set terms,
the bonus for a road square to answer kept. On the 150 positions at depth 11, every weight
searched more nodes and took longer than E50: 5% to 17% and 26% to 43%. The matches say otherwise,
at 2+0.02, [0, 10]:

| Change | Bench | Result |
|---|---|---|
| The prior at 1024 a nat from depth 1, against E50 | 1224480 | **PASSED**, 608 games, +262 -188 =158, **+42.5 +/- 22.0**, pairs [19, 53, 115, 69, 48] |
| 2048 a nat, against 1024 | 1319817 | Stopped at 834 games as neutral, +298 -304 =232, **-2.5 +/- 19.1** |
| 512 a nat, against 1024 | 1316764 | **FAILED**, 582 games, +180 -244 =158, **-38.4 +/- 23.4** |
| From depth 2, with E50's ordering at depth 1, against 1024 from depth 1 | 1614296 | **PASSED**, 1612 games, +613 -520 =479, **+20.1 +/- 14.0**, pairs [77, 163, 264, 194, 108] |

A tree that takes longer to reach a depth was the stronger one: what the prior changes is which
moves the pruning and the reductions keep. At depth 1, where four interior nodes in five are, a
prior for every move costs more than it gives, and E50's terms there are the better trade.

**In the pruning**, on the prior from depth 2, against it:

| Change | Bench | Result |
|---|---|---|
| Spare: where move-count pruning stops, the moves left within half a nat of the node's best prior are still searched | 1514873 | INCONCLUSIVE at the cap, 2000 games, +711 -690 =599, **+3.6 +/- 12.4** |
| Floor: at depth 2 off the PV, after one move, a move six nats below the node's best prior is skipped | 1617786 | INCONCLUSIVE at the cap, 2000 games, +725 -713 =562, **+2.1 +/- 12.4** |

Neither is kept. With the prior in the order, the moves the pruning leaves are already the ones it
rates least, and asking it again when the pruning stops adds nothing measurable.

**As staged**, the prior from depth 2 at 1024 a nat, bench 1614296 from `make` and `make release`:

| Match | Result |
|---|---|
| syntaks at 1/4 of 5+0.05, 600 games, nothing else running | +234 -296 =70, **-36 [-62, -9]** |
| E50 again, straight after, the same | +212 -302 =86, -52 [-79, -25], where E50's own run read -33 |
| Topaz at 5+0.05, 200 games, the same | +122 -56 =22, **+119 [+68, +168]** |
| E50 again, the same | +92 -59 =49, +57 [+13, +101], where E50's own run read +79 |

16 and 62 ahead of E50 run beside it, about the 60 that the two passes add up to in self-play.
Against syntaks it lost 75 games by road to E50's 82, 71 of them by spread to 79, and won 152 on
flats to 130.

The prior was fitted on 6x6 alone and plays every size; no other size has been played with it. The
variants are kept in `external/build/pol/search-variants-e51.diff`.

## E52: the move prior as a network, and history halved each search

**Where E51's prior stood.** On 231k held-out 6x6 positions of gens 10-11, 99.3 legal moves each,
22.6 placements and 76.7 spreads. The searched move was a spread 42.7% of the time, and that was
where the prior was weakest:

| Target | Share | Top 1 | Mean rank |
|---|---|---|---|
| Flat | 48.5% | 17.5% | 5.8 |
| Wall | 7.4% | 10.5% | 15.3 |
| Capstone | 1.4% | 41.2% | 7.6 |
| Spread | 42.7% | 11.0% | 14.7 |

It cost 17 ns a node and 20 ns a move, 2.0 us a position. On 150 held-out positions searched to
1M nodes, half the nodes that reach the move loop build a list; a list costs 0.6 us at depth 1 with
E50's hand-set terms and 1.9 us from depth 2, 12.5% of the search's time.

**Capacity.** A fitter that reads every position into memory on all threads and fits minibatches by
Adam made a fit 18 seconds, on 2.25M positions (one game in 8 of gens 10-11) and 281k held out.
Added to E51's 262 features, one group at a time, in its linear form:

| Linear, held out | Loss | Top 1 | Mean rank |
|---|---|---|---|
| E51's features | 3.249 | 14.4% | 10.41 |
| + how many placements each side's best road through the square needs | 3.111 | 16.6% | 9.47 |
| + where the last move was: on, beside, two away | 3.180 | 16.3% | 9.87 |
| + the line of road stones through it, ours and theirs it cuts | 3.224 | 15.0% | 10.21 |
| + how a spread lands: stones dropped last, landings on empty, ours, theirs, height, ours carried | 3.231 | 14.5% | 10.17 |
| + ten square classes | 3.246 | 14.4% | 10.36 |
| All five | 3.017 | 18.5% | 8.70 |

Then a hidden layer of clipped units beside the linear term, the position's own features (phase,
race, each side's road distance, lead, capstones in hand, empty squares) summed into every move's
units:

| Hidden layer | Loss |
|---|---|
| E51's features, 16 units | 3.109 |
| All five groups, 16 units | 2.836 |
| + the position's features | 2.829 |
| 8 / 32 / 64 units | 2.851 / 2.814 / 2.804 |
| 16 units, + our own last move, + the last move's kind | 2.804 |

Squared activation and 8 epochs fitted no better. Taking one group out of the 16-unit fit cost
0.067 for the last move, 0.061 for the road distance, 0.022 for the spread's landing, 0.018 for the
lines, 0.011 for the classes and 0.006 for the position. Small features that always come together,
crossed into one (a placement's five road flags by stone and phase, a spread's landing counts, its
hops by its last drop, the last two moves with the last one's kind), fitted slightly better and
read 15% fewer features. Rings and corners are subsumed by the classes and went.

**As staged**, `policy.c` is 1314 features, each a row of 16 lanes: 15 clipped units and a linear
lane, in 16 bits at 128 a unit. Fitted on 6x6 gens 10-11, one game in 8, 4 epochs: held-out loss
2.7987, and the engine's own integer prior 2.7990, top 1 21.9%, top 3 44.8%, top 8 71.6%, mean
rank 7.36. It was never fitted on the other sizes:

| Held out | E52 | E51 | E52 fitted on every size |
|---|---|---|---|
| 5x5 | 2.540 | 2.946 | 2.535 |
| 6x6 | 2.778 | 3.234 | 2.780 |
| 7x7 | 3.166 | 3.560 | 3.058 |
| 8x8 | 3.505 | 3.815 | 3.313 |

The fit on every size added gen 11's 5x5, 7x7 and 8x8 and has not played.

**Cost.** Read naively the features took 40 ns of the 47 a move, the road distance 14 of them and
185 ns a node. Every step below left every logit the same, checked by a checksum over all held-out
moves and by bench:

- The road distance as bitboards: for each side and edge, what k placements reach, flooding at
  each level only from what it added, then the squares within 0, 1 and 2 of the side's best as
  three sets a node, so a move's distance is three bit tests.
- Both sides' eight edges flooded at once in one 8-lane GCC vector: the node's context 38% less.
- A node's moves scored at once, `policy_logits()`, with what they share summed once: a
  placement's square (10.9 squares for 22.9 placements), a spread's source (12.2) and carry
  (17.5), the squares a stack's spread lands on, kept per source and direction and hops (14.9 for
  48.9 spreads), and a single stone's step, which depends only on where it lands (19.8 for 27.5).

On a quiet machine the prior costs 2.7 us a position against E51's 2.0, a list from depth 2 3.85
us against 1.92, and the order 24.1% of the search's time against 16.7%. Bench nps fell from 2.13M
to 1.98M, 0.93 times. Selftest holds scoring at once equal to scoring each move alone, and every
prior unchanged under the eight symmetries and a colour swap, now with the last two moves turned
with the board, on random weights.

**In the tree.** At cuts by a move from the list at depth 2 or more, 150 positions to 300k nodes,
each cleared before its search, the cutting move's rank:

| Ranked by | First | Top 2 | Top 4 |
|---|---|---|---|
| E52's prior alone | 32.0% | 48.0% | 66.1% |
| E51's prior alone | 22.1% | 35.3% | 53.0% |
| History alone | 35.6% | 50.1% | 64.1% |
| The search's order with E52 | 53.5% | 70.4% | 85.6% |
| The search's order with E51, in its own tree | 52.0% | 68.6% | 83.9% |

On the same tree, history with E52's prior at 1024 a nat put the cut first 53.5%, with E51's 47.4%,
both best at 1024 to 2048. History already holds much of what the better prior knows, and the tree
adapts to whichever order explores it. E52's logits spread 1.63 times E51's, a standard deviation of
3.19 nats to 1.96. Weighing the prior up for moves whose history is near zero changed nothing.

**In play**, 2+0.02, [0, 10], against E51 unless said:

| Change | Bench | Result |
|---|---|---|
| c1: the network prior, a move at a time | 1493414 | **FAILED**, 916 games, -19.7 +/- 17.8 |
| c2a: the same, a node's moves at once | 1493414 | INCONCLUSIVE at the cap, 2000 games, +0.3 +/- 12.2 |
| c2b: c2a at 640 a nat | 1462328 | **FAILED**, 448 games, -51.6 +/- 26.4 |
| d1: c2a, a ply more reduction for moves 4 nats below the best prior, a ply less within 1 | 1543852 | **FAILED**, 1468 games, -13.3 +/- 15.4 |
| d2: d1's more reduction only | 1269633 | **FAILED**, 902 games, -20.8 +/- 18.1 |
| d3: c2a from depth 3, E51's prior at depth 2 | 1364646 | **FAILED**, 1638 games, -10.0 +/- 13.9 |
| h1: c2a with history and replies halved at the start of each search, against c2a | 1493414 | **PASSED**, 1218 games, **+26.0 +/- 16.4** |
| c3: the staged features and fit, at 1024 a nat | 1465868 | INCONCLUSIVE at the cap, 2000 games, -0.7 +/- 12.3 |
| h0: E51 with history halved | 1614296 | **FAILED**, 1798 games, -9.3 +/- 13.6 |
| h3: c3 with history halved | 1465868 | INCONCLUSIVE at the cap, 2000 games, +10.9 +/- 12.5 |
| h4: h3 at 1536 a nat | 1250165 | **PASSED**, 1002 games, **+28.1 +/- 17.3**, pairs [44, 81, 199, 104, 73] |

History carried from earlier moves at full weight before this, cleared only by `teinewgame`, a
komi change, bench and datagen. Halving it at each search lets the prior order a new position until
the search has evidence of its own, which pays with this prior and costs 9 with E51's. Bench clears
history before every position, so it cannot see the change. The heavier weight followed probes with
history warm, every own ply of a game searched in turn, where 1536 to 2048 ordered best.

Committed: h4, bench 1250165 from `make` and `make release`, binary external/baselines/e52.exe.
The fitter is `yapper policyfit` with threads, DATA.md. Its gauntlets are in E53. Not yet run: the
fit on every size. The work copy, the probes and the candidates are in external/build/pa, pc, pd,
pf and ph.

## E53: a review without the history, and stronger reductions

**How the changes were found.** A subagent read `src/` and the rules but not this file, STATUS.md
or ARCHITECTURE.md, so that what earlier experiments concluded could not steer it. Nine of its
suggestions were built, each one change on E52. Two of them are Tak's own, r1 and r2, and the rest
are generic search and time knobs. The one defect it found was real: a road to answer skips the
moves that cannot stop it, and history still penalised them as if they had failed (r4).

**Against E52**, 2+0.02, [0, 10]:

| Change | Bench | Result |
|---|---|---|
| r1: a side that ends the game on the count, by a flat on its last stone or on the last empty square, scores the win at once in search and quiescence | 1248872 | INCONCLUSIVE at the cap, 2000 games, +728 -712 =560, +2.8 +/- 12.3, pairs [113, 190, 379, 204, 114] |
| r2: r1, and no reverse futility, null move or move-count pruning while the side to move is on its last stones, against r1 | 1247270 | **FAILED**, 422 games, +120 -181 =121, -50.6 +/- 26.0, pairs [31, 56, 81, 29, 14] |
| r3: no null move | 1232932 | INCONCLUSIVE at the cap, 2000 games, +744 -714 =542, +5.2 +/- 12.5, pairs [114, 199, 348, 221, 118] |
| r4: history spares the moves a road answer skipped | 1273229 | INCONCLUSIVE at the cap, 2000 games, +703 -699 =598, +0.7 +/- 12.3, pairs [111, 212, 343, 230, 104] |
| r5: the stand pat 50 lower when the other side can finish a road by a spread | 1290195 | **FAILED**, 1078 games, +365 -426 =287, -19.7 +/- 17.7, pairs [78, 125, 182, 88, 66] |
| r6: a quarter more of the clock a move | 1250165 | INCONCLUSIVE at the cap, 2000 games, +763 -669 =568, +16.3 +/- 12.3, pairs [92, 200, 364, 210, 134] |
| r7: the killers moved two plies toward the root at each search | 1250165 | **FAILED**, 1596 games, +564 -614 =418, -10.9 +/- 14.3, pairs [101, 183, 281, 131, 102] |
| r8: reductions `/ 1.6`, for `/ 2.0` | 1060614 | INCONCLUSIVE at the cap, 2000 games, +754 -671 =575, +14.4 +/- 12.4, pairs [103, 181, 378, 206, 132] |
| r9: at the first leaf ply with no road to answer, spreads that win two tops or more are searched | 1388465 | INCONCLUSIVE at the cap, 2000 games, +721 -731 =548, -1.7 +/- 12.4, pairs [117, 195, 387, 183, 118] |

r6 and r8 ended with their intervals above zero, so each ran again from nothing, with another
opening order (`--seed 2`) and a cap of 4000:

| Again | Result |
|---|---|
| r8 | **PASSED**, 694 games, +291 -206 =197, **+42.8 +/- 22.0**, pairs [31, 59, 111, 86, 60] |
| r6 | INCONCLUSIVE at the cap, 4000 games, +1456 -1404 =1140, +4.5 +/- 8.7, pairs [231, 366, 749, 428, 226] |

r8 passed on its second run, which is a second chance to pass, so the size to believe is the two
runs together: 2694 games, **+21.7 +/- 10.8**. r6's two runs make 6000 games at +8.5 +/- 7.1,
which [0, 10] cannot pass at the cap. E42 found `/ 1.75` neutral against F1, -1.2 +/- 12.9. E52's
prior has come since, and a guess, not measured, is that a better order leaves the late moves less
often best, so reducing them harder costs less. Bench fell 15%, to 1060614.

In r1's 2000 games one was lost on time. The engine averaged 0.06 s a move on the 0.02 increment
until its clock sat near the 20 ms it keeps back, and then one slow reply flagged. Nothing in r1
touches the clock, and older match logs hold the same kind of loss.

**As staged**, r8, bench 1060614 from `make` and `make release`, binary
external/baselines/e53.exe, against E52 run beside it:

| Match | Result |
|---|---|
| syntaks at 1/4 of 5+0.05, 600 games, nothing else running | +285 -245 =70, **+23 [-3, +49]** |
| E52 again, straight after, the same | +289 -226 =85, +36 [+10, +63], where E52's own run read -27 |
| Topaz at 5+0.05, 200 games, the same | +121 -47 =32, **+134 [+85, +182]** |
| E52 again, the same | +119 -35 =46, +155 [+107, +201], where E52's own run read +143 |

13 and 21 behind E52 run beside it, both inside either interval and both the other way from
self-play: against other engines the gain is not seen yet. E52 itself moved 63 against syntaks
between the afternoon and the evening of one day, so only a run beside it compares.

E52's own gauntlets, that afternoon, read +238 -285 =77, -27 [-53, +0] against syntaks at a
quarter and +119 -41 =39, +143 [+94, +190] against Topaz over 199 games, with E51 straight after
at +236 -290 =74, -31 [-58, -4] and +120 -47 =33, +132 [+83, +180]: E52 4 and 11 ahead. The
variants are in external/build/rv, the work notes in external/build/pa/notes.md.

## E54: every tunable in one list, and a tuner

No change to play. The 45 numbers the search plays by, its margins and depths, reductions,
singular extension, aspiration, history, the prior's weight and depth, the hand-set order a ply
from the leaves, the evaluation's price for a road threat and the clock, are each a line in
`src/tune.h`: `X(name, default, min, max)`. A normal build makes them enum constants; `make
TUNE=on` makes them variables and TEI spin options. Where a value wanted finer steps for a tuner,
its units were chosen to give the old integer arithmetic exactly: reductions as hundredths
divided out in floating point, which are the same doubles as 0.5 and 1.6, the singular margin in
sixteenths a ply, and move-count pruning in quarters. `tools/tune.py` is SPSA on fishtest's
schedule over match.py's referee, one pair an iteration, with workers that do not wait for each
other. Nothing has been tuned.

| Check | Result |
|---|---|
| bench, `make`, `make release`, `make TUNE=on` | 1060614 each, as before |
| `setoption name LmrDivisor value 200` in a fresh tune build | 1250165, E52's bench, before E53's `/ 1.6` |
| Speed, five benches each, alternated | old 1.900 M nps, `make` 1.902 M, `make TUNE=on` 1.846 M |
| 60 pairs at 2000 nodes, four at once | played, state and history written |
| 400 pairs at 10,000 nodes, killed at 21 and resumed | 400 pairs, one history row each |
| 16 pairs at 1+0.01, the clock's tunables | no time losses |

**Found on the way.** A bench in a process that has already searched reads 974794, not 1060614:
only the first position differs, and only on the first search a process makes. `policy_context()`
reads the network's bias before `rows_now()` installs the compiled network, so the first prior a
process computes has no bias. Every bench since E52 counts it, and so does the first search of
every engine in a match. Reading the bias after the install gives 974794 on every pass. It moves
bench, so it is left for a change of its own.

## E55: quiet spreads pruned by their own count

The night of 2026-11-06, five ideas from Tak's own structure were built and tested side by side,
E55 to E60. This is the first: a tree that is wide only where Tak is tactical. E21 found that
moves past the 32nd were two thirds of the work and that cutting them all gained nothing, and E51
that the rare late moves which do cut are three in five spreads that win tops, with placements
most of the rest. So a *quiet spread*, one that wins no tops and lands on no square where a
placement would leave either side a road to finish, is skipped after `(8 + 2 d^2) / 4` moves at
depth 10 and below, twice that when improving, while placements and the other spreads go on to
the usual move-count limit. A skipped move still counts towards the limits, since E50's
silhouettes showed that skips which do not let later moves in. The *gate* applies it only at
nodes where neither side is within two placements of a road, where the rare late move that
matters, a defence against a road, cannot exist. Each knob is a tunable: `QuietDepth`,
`QuietBase`, `QuietSlope`, `QuietDanger`, `QuietReduce`, `FutilityDepth`, `FutilityMargin`.

Measured first on bench: road-free nodes are 84% of interior nodes, 176,543 of 209,632, so the
gate is not what limits this. With it, only 15,145 quiet spreads were skipped in 1.07M nodes: at
depth 1 and 2 the move-count limit already stops before the quiet spreads, which the order puts
last, and deeper nodes are few.

Against E54 (master, bench 1060614), 2+0.02, [0, 10], 7 games at once:

| Variant | Bench | Result |
|---|---|---|
| b: the gate off, quiet spreads pruned everywhere | 1004912 | **FAILED**, 1048 games, +339 -379 =330, **-13.3 +/- 16.9**, pairs [60, 129, 176, 109, 50] |
| a: with the gate, on a quiet machine | 1074788 | INCONCLUSIVE at the cap, 2000 games, +695 -691 =614, **+0.7 +/- 12.4**, pairs [114, 204, 360, 208, 114] |
| d: a, and futility to depth 4: a quiet spread or a placement off those squares is skipped when the evaluation plus 150 a ply is still below alpha | 1041602 | **FAILED**, 1880 games, +648 -688 =544, **-7.4 +/- 12.6**, pairs [118, 184, 345, 206, 87] |
| c: a, and quiet spreads reduced a ply more | 1081948 | **FAILED**, 762 games, +235 -292 =235, **-26.0 +/- 20.0**, pairs [56, 84, 131, 81, 29] |

Pruning the quiet spreads everywhere lost, as did reducing them more and the futility margin;
the gate keeps the pruning away from the nodes where it could cost a defence, and there it barely
prunes at all, and is neutral. The quiet spreads are not where the tree's width is: by the order
they are already last, and E42's move count already stops before them below depth 6. What is
left is the same lesson as E21 and E36: the spreads the search still reaches are the ones it
needs.

A first run of `a` read INCONCLUSIVE at the cap, +8.9 +/- 14.0 at 1648 games, and is void: 368 of
its 2000 games were lost on time, 185 by one side and 183 by the other, because the cutoff logger
of E59 and the prototype of E58 shared the machine with it. `b` had none. TESTING.md says to run
nothing beside a match, and the `endings` line of a match's log is where that shows; the rest of
the night's matches ran one stage at a time with the heavy work between them.

## E56: history by the prior's features

Whole-move history was the largest search gain, +121.7 in E40, and halving it so the prior leads
gave +26 more, E52: within a game the search learns what the fitted prior cannot know. But a
16-bit key means a spread earns its reputation on every square separately. Here each of the
prior's 1314 features has a history entry of its own, by side: on a cutoff from a node's list,
the cutting move's features earn the bonus and the tried moves' features the malus, with the
same gravity as history and a limit of 2048 an entry, and a move's order adds the sum over its
features. `policy_logits_history()` sums the entries over the same shared parts that the prior's
rows are summed over, so the order costs one more lane; the updates cost `policy_features()` for
each tried move at those cutoffs. Halved at the start of every search as history is. Bench
1012552, and about 10% slower on the bench, alternated with master under load.

Against E54, 2+0.02, [0, 10]: **FAILED**, 378 games, +106 -176 =96, **-65.1 +/- 29.5**, pairs
[39, 43, 68, 27, 12], 4 of them lost on time. Far more than the speed costs. The flaw is in how
the malus lands: a feature that twenty tried moves share sinks twenty times at a cutoff for the
once it rises when such a move cuts, so the features that placements share sink furthest and the
order drifts towards the spreads with rare features. `e56b` shares one malus over the tried
moves, `-bonus * 2 / (i + 1)` each, bench 1015544: **FAILED** too, 842 games, +268 -327 =247,
**-24.4 +/- 19.4**, pairs [62, 95, 142, 84, 38]. Better by 40 and still well below zero. A
move's sum over its features also grows with how many it has, which favours spreads over
placements, and the 10% of speed is there either way. History by the whole move, with the prior
for what it has not seen, is the better division of labour; dropped.

## E57: roads and reach as inputs to the evaluation's head

The network is a sum of per-square templates, squared and pooled, so it cannot compute
connectivity. Four exact quantities now reach the head beside the lead, reserves and komi: each
side's fewest placements to a road, from the policy's flood, and how many empty squares finish
one at that distance, up to 8 (`NN_TAK_INPUTS`, `policy_road_summary()`). The head's skip rows
widen from 3 to 7 (`NN_SKIP_INPUTS`), a `net.h` carries `NET_SKIP_INPUTS`, the trainer reads the
inputs through `eval_net_inputs()` as the engine does, and selftest holds the evaluation to the
eight symmetries and the colour swap with random weights on the new rows: 987,124,428 checks, 0
failures. The flood runs at every evaluated node: bench nodes unchanged with the new rows at
zero, and the bench 15% slower, 1.55M against 1.31M nps alternated under load.

Fits, each beside a control of the same recipe without the inputs:

| Fit | Control | With the inputs |
|---|---|---|
| E49's warm recipe again, 2 epochs at 0.0005 on all games, 6x6 gen 11 held out | 0.415905 | 0.415704 |
| The same, 6x6 gen 10 | 0.419801 | 0.419597 |
| The same, 8x8 gen 11 | 0.504538 | 0.504112 |
| From nothing on 6x6 gens 10-11, 14.7M positions, 5 epochs, held out against the target | 0.494441 | **0.492990** |
| The same, against results | 0.419360 | **0.416874** |
| The same, 6x6 gen 10 | 0.421626 | 0.419325 |
| The same, 6x6 gen 11 | 0.416686 | 0.413983 |
| Warm at 0.002 for 3 epochs, against results | 0.458833 | 0.458823 |
| The same, 6x6 gen 11 | 0.415905 at 0.0005 | 0.415956 |

Learned from nothing the inputs are worth about 0.0025 against results, two and a half times a
seed's spread; a warm fine-tune from zero rows never learns to use them at either rate, and the
higher rate only disturbs a converged network. By E42's rule 0.0025 is about 25 at equal nodes,
against about 20 to 25 lost to the flood's cost: a wash unless the flood gets cheaper. So two
networks were fitted from nothing on all the games with E48's 15-epoch recipe, one with the
inputs and one without, to be played against each other so that the inputs are measured and not
the lineage:

| From nothing, all games, 15 epochs, against results | Control | With the inputs |
|---|---|---|
| All held out | 0.459165 | 0.459313 |
| 6x6 gen 11 | 0.416171 | 0.416575 |
| 6x6 gen 10 | 0.420054 | 0.419991 |
| 7x7 gen 11 | 0.463586 | 0.462881 |
| 8x8 gen 11 | 0.507679 | 0.507512 |

Nothing: every difference is inside a seed's spread. Given 254M positions and 15 epochs, the
filters learn what the inputs say by themselves, and the 14.7M-position fit had only found a
shortcut. The match was not played, since it could only measure the flood's cost. The idea
stands only in a form that costs nothing, such as a bucket the search already knows the key of,
and the e57 build, its fits and the flood are in `external/build/e57`.

## E58: a local feature-map network, as a prototype

Each square gets a hidden vector from a kernel over the 5x5 window around it: an occupied
neighbour adds the row of its token, as the viewer's own or the other side's, an empty neighbour
the empty row and a square off the board the off-board row. The squares' clipped squares are
pooled per channel in each side's view, 64 channels a view, and the engine's head reads the 128
pools with the lead, reserves and komi, as it reads the pools now. 947,424 parameters.
`external/build/e58/proto.c` fits it on the CPU with Adam, the trainer's batch, rate schedule
and lambda, on the same 14.7M positions and the same held-out games as the control, which is the
engine's own network fitted from nothing by `nntrain` on those files for the same 5 epochs.
Nothing ties the kernel under the board's symmetries here: this measures capacity.

| Epoch | Control, held out / against results | Feature map |
|---|---|---|
| 1 | 0.497270 / 0.427063 | 0.495780 / 0.424536 |
| 2 | 0.494831 / 0.421339 | 0.493131 / 0.419164 |
| 3 | 0.494526 / 0.420205 | 0.491777 / 0.416126 |
| 4 | 0.494449 / 0.419392 | 0.491561 / 0.415850 |
| 5, the control's weights averaged | 0.494441 / 0.419360 | **0.491422 / 0.415588** |

Locality alone is worth 0.0030 against the target and 0.0038 against results, more than the
head gave in E48. An engine version must tie the kernel under the eight symmetries, which at
equal cost leaves far fewer pooled features: `proto_tied.c` fits that, F base filters each seen
through the eight orientations of the window, 8F units a square, pooled over squares and
orientations to F pools a view, so turning the board cannot change the score. The same games,
epochs and recipe, 4 threads beside the gen 12 datagen:

| Epoch | Control | Untied, 64 channels, 128 pools | Tied, F 16, 128 units a square, 32 pools | Tied, F 8, 64 units a square, 16 pools |
|---|---|---|---|---|
| 1 | 0.497270 / 0.427063 | 0.495780 / 0.424536 | 0.497847 / 0.429348 | 0.494089 / 0.422553 |
| 3 | 0.494526 / 0.420205 | 0.491777 / 0.416126 | 0.491713 / 0.417898 | 0.489160 / 0.413202 |
| 5 | 0.494441 / 0.419360 | 0.491422 / 0.415588 | 0.490807 / 0.416761 | **0.488298 / 0.412087** |

The tied networks, with the symmetries built in and a quarter or an eighth of the pooled
features, beat the engine's own architecture, and the smaller one beats everything: F 8 is
**0.0061 better against the target and 0.0073 against results** than the control, 123,000
parameters against the control's 2.4M, which on 14.7M positions is the difference between
learning and memorising. The sharing across translations and orientations is where the capacity
is, and there is no symmetry tax to speak of. By E42's rule 0.007 is about 70 at equal nodes,
more than E44's two views. What it would cost in play at F 8: a changed square touches 25
squares' vectors of 64 units, 1600 adds against today's 1024, the frame is 36 squares by 64
units by two views, 9 KB against 2 KB, and the pooling runs over 4608 units against 1024;
against that, the prior could be read off the same accumulator for a few hundred cycles and run
at depth 1, where E51 found it too dear. The prototype has no weight averaging and the control
has, which flatters the control if anything. Not measured: whether the gain holds on all 254M
positions, where E57's did not, and what the right F is there; the untied run suggests that more
width is not what the data wants.

## E59: the prior fitted on the search's own cutoffs

The prior is fitted on the moves the search chose at the root, and read in the tree, where its
job is to find the refutation. `yapper cutlog` in `external/build/e59` searches one position in
20 of the 6x6 gen 10 and 11 games at 20,000 nodes and records one in 32 of the cutoffs by a
move from a node's list where the prior orders, each as a game in docs/DATA.md's format: the
game's moves to the position, then the search's path to the node, unscored, and the cutting move
scored, so `policyfit` fits it as it fits a played move. The records read as games cut off at the
ply limit; takref accepts every move. Six files were logged before the SPRTs needed the machine:
184,217 searches, 1,334,407 cutoffs, 310 MB. Fitted with `policyfit`'s recipe, every record, 4
epochs: held out 1.25M positions, loss 2.0927, **top 1 37.4%**, top 3 65.6%, top 8 88.0%, mean
rank 4.05, and the engine's own prior on the rounded weights 2.0930. E52's prior predicted the
root's choice first 21.9% of the time; a cutoff is an easier target, since the order that found
it is in the data. Built in place of E52's prior, bench 1195823. Against E54, 2+0.02, [0, 10]:
**FAILED**, 1836 games, +623 -665 =548, **-7.9 +/- 13.0**, pairs [114, 192, 331, 184, 97].

A prior that predicts the cutoffs of the old order better does not order better: what it learned
is largely the old order itself, the first move that happened to reach beta, and the root's
choice carries something the cutoffs do not. Fitting on both at once, or on cutoffs recorded
with the order scrambled, was not tried.

## E60: a generation from the opponents' openings

All training data is self-play from random plies, and E37 noted the limit: it only adds the
structures the engine's own play reaches. `tools/mkopenings.py` takes the games against syntaks
and Topaz in `external/games`, 19,663 of them, and writes the first 10 to 30 plies of each, every
4, as a book of 111,258 openings; `yapper datagen` plays a line of the book in place of the
random plies, the book plies unscored as random plies are, and the format is unchanged. Gen 12:
50,000 games of 6x6 at 20,000 nodes from E54's search and E49's network, on 11 jobs beside the
prototypes, 3.7 games a second, 4.8M positions; takref replayed 250 with no failures. They read
unlike self-play from random plies: a median of 98 plies to gen 11's 93, roads end 5.9% of them
to 8%, and **22.8% are drawn** to 13%, since a position syntaks or Topaz reached by ply 10 to 30
is a level one, and white scores 47.8%.

E49's warm recipe again, 2 epochs at 0.0005, with gen 11 and gen 12 each listed four times:

| Held out, against results | E49 refitted the same way without gen 12 | With gen 12 |
|---|---|---|
| 6x6 gen 11 | 0.415905 | **0.413993** |
| 6x6 gen 10 | 0.419801 | **0.418240** |
| 7x7 gen 11 | 0.462945 | 0.462982 |
| 8x8 gen 11 | 0.504538 | 0.504712 |
| 6x6 gen 12 | | 0.489052 |

A fifth as many games as gen 11 moved 6x6's older groups by 0.0016 to 0.0019, about what gen 11
itself moved gen 10 in E49, and the other sizes not at all, as they had none of it. The engine
reads within 0.50 of the fit on average, 2 at worst. Bench 1301784. Against E54, 2+0.02, [0, 10]:
INCONCLUSIVE at the cap, 2000 games, +750 -681 =569, **+12.0 +/- 12.5**, pairs [104, 201, 348,
216, 131], no time losses. The same shape as the data steps before it, E42's +11.8 and E49's
+23.4, from a fifth of gen 11's games: a source worth drawing on in the next generation, mixed
with self-play from random plies so the roads do not go the way of E17's gen 2.

## E61: where a node's time goes, and where the network and the prior fall short

The user's view after E55 to E60: search changes keep failing because the evaluation and the
prior are not good enough yet for a sharper search to trust, so the architecture is where to look.
Everything here is measured on E54's build, master at bench 1060614, from an instrumented copy
in `external/build/diag` (`yapper diag search | value | policy | depth`, under `-DDIAG`, and
`-DDIAG_TIME` for cycle counters). Its three builds bench 1060614 like master; the counters cost
11% of the speed.

**Time.** 300 positions from gen 11's held-out 6x6 games, every 7th scored ply from the 12th,
each searched to 200,000 nodes from an empty table: 2787 cycles a node with the counters.

| Part | Share of the search | Cycles a node |
|---|---|---|
| The network: bringing the accumulator up to date | 19.4% | 540 |
| The network: scoring it, pooling and head | 19.9% | 555 |
| The prior's network, `policy_logits()` | 16.6% | 461 |
| The prior's context | 0.8% | 23 |
| The rest of the order: history, hand order | 4.4% | 123 |
| Picking the next move | 5.1% | 142 |
| Road ends, gaps and placements | 8.9% | 247 |
| Roads by spread | 4.6% | 129 |
| Make and unmake | 4.8% | 134 |
| Table 1.4%, movegen 1.0%, legality 0.5%, road answers 0.1% | 3.0% | 86 |
| Everything else | 12.5% | |

The two networks take 57% of the search. 86% of nodes evaluate, at 628 cycles to update and 645
to score. A list scored by the prior costs **14,600 cycles for 89.5 moves, 163 a move**, as much
as 11 evaluations; 2.0M lists had the prior and 2.4M, a ply from the leaves, the hand order.

**The order.** 78.4% of cuts come on the first move searched. Killers make 60.3% of all cuts, the
table move 14.4% and the list 25.3%. When the cut comes from the list, after the table move and
the killers have failed, it is the list's first move far less often:

| A cut from the list, at depth 2 to 4: ranked first by | Share |
|---|---|
| The prior alone | 33% to 35% |
| History alone | 29% to 32% (17% to 20% of cut moves have no history at all) |
| History plus prior, as the search orders | **47% to 49%** |

Null move cut on 1.4% of 1.13M tries, reverse futility returned at 21.0M of the 36M interior
nodes, and 1.0% of reduced moves were searched again. A node that refutes nothing searched 55
moves at depth 4 and 78 at 5 and 6. Each iteration took 1.9 to 2.3 times the one before up to
depth 9, where E42 put syntaks at 1.5.

**The prior's accuracy**, against the move the search chose, on half of gen 11's held-out games:

| Held out | Plies | Top 1 | Top 3 | Top 5 | Top 10 | Mean rank |
|---|---|---|---|---|---|---|
| 5x5 | 41,001 | 24.5% | 50.0% | 65.1% | 84.1% | 4.6 |
| 6x6 | 258,246 | **21.6%** | 44.5% | 58.2% | 77.2% | 6.4 |
| 7x7 | 155,785 | 17.9% | 37.4% | 49.6% | 67.8% | 9.3 |
| 8x8 | 148,304 | **14.4%** | 30.8% | 41.5% | 58.5% | 13.3 |
| Flats, all sizes | 297,674 | 20.7% | 46.8% | 62.2% | 81.6% | 5.6 |
| Walls | 39,392 | 10.0% | 26.4% | 38.5% | 58.7% | 12.8 |
| Spreads | 254,648 | 18.4% | 33.5% | 43.2% | 60.2% | 11.6 |

Those games are the ones the network holds out, which policyfit fits on; on the games policyfit
holds out the prior reads the same, 21.7% top 1 on 6x6 and 14.6% on 8x8, so it does not overfit.

**Where the network falls short of the search**, on the 225,948 positions of half of gen 11's
held-out 6x6 games that a fit reads: the network scores 0.4163 against results and the stored
20,000-node search 0.3918, 105 hundredths apart on average, 160 rms. By kind of position, the gap
between the two:

| Position | Fewest | | | | Most |
|---|---|---|---|---|---|
| Tallest stack, 1 to 7+ | 0.0054 | 0.0133 | 0.0203 | 0.0297 | **0.0495** |
| Stacks of 3 or more, 0 to 8+ | 0.0096 | 0.0151 | 0.0238 | 0.0380 | **0.0740** |
| Walls on the board, 0 to 8+ | 0.0112 | 0.0132 | 0.0213 | 0.0353 | **0.0529** |
| Tops the best spread wins for the side to move, 1 to 4+ | 0.0176 | 0.0328 | 0.0340 | | **0.0485** |
| Bucket, the start to the end | 0.0100 | 0.0188 | 0.0248 | 0.0342 | **0.0767** |

Fewest placements to a road do not sort the gap: 0.012 to 0.028 at distances 2 to 5, and 0.030
where neither side has a road within six, which are the crowded boards. The network is also
shrunk towards level: 46 hundredths above the search when 7 or more half flats behind, and 50
below it when as far ahead. The gap grows with what a stack can do to its neighbours and along
its lines, which is what a sum of per-square templates cannot see: E43 found every unit weighs a
square's token alone, and E57 that given the data the templates learn road distance by
themselves.

**What search adds to the network**, on 1200 of those positions, each searched from an empty
table to a fixed depth:

| Depth | Static | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | Stored 20k |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Against results | 0.4365 | 0.4347 | 0.4269 | 0.4232 | 0.4193 | 0.4162 | 0.4125 | 0.4078 | 0.4060 | 0.4010 | 0.3973 | 0.4064 |

A ply is worth 0.002 to 0.005 here, and the second ply most, 0.0078: the reply.

**The network has stopped learning, and its shape is not why.** E57's control, E48's recipe fitted
from nothing on all 254M positions for 15 epochs, held out 0.4609 against results after the first
epoch, 0.4599 after the second, and 0.4592 after the fifteenth, while its fit loss fell from 0.4987
to 0.4914. More epochs, inputs that say where the roads are (E57), 256 filters (E42) and buckets
(E45) all land within a seed of it. On 14.7M positions it overfits outright, 0.4816 fitted against
0.4944 held out, where E58's tied feature map, with a twentieth of the parameters, fitted 0.4870 to
0.4883. That read as the shape being the limit, so the shapes were fitted on every 6x6 game.

A trainer for the feature maps on the GPU, `external/build/arch/gproto.c` with its kernels in
`gproto.cu`, fits E58's networks, the engine's own and a hybrid of the two, by the same recipe,
on the same 150.7M positions of gens 7 to 11 and the same 10.0M held out as `nntrain`. On a slice
it matched the CPU prototype to the fifth decimal, and its version of the engine's network matched
`nntrain`'s fit epoch by epoch, 0.448714 against 0.448732 after the third. Each block is a position;
the kernel's gradient for the map goes through shared memory before it reaches the card's memory,
since every position adds to the same few rows of empty squares and lone flats: written as one
scatter a unit, a step took 66 ms, and 24 ms gathered. An epoch takes 4 to 6 minutes, where the CPU
prototype took 19. Three epochs each:

| On every 6x6 game, held out | Parameters | Against the target | Against results | Fitted minus held out |
|---|---|---|---|---|
| The engine's network, `nntrain` | 2.4M | 0.499287 | 0.448732 | -0.003 |
| The same, on the GPU prototype | 2.4M | 0.499333 | **0.448714** | -0.003 |
| Feature map, 8 filters, the 3x3 box with the whole rank and file | 160k | 0.499195 | 0.449837 | 0.000 |
| Feature map, 8 filters, the 5x5 box | 160k | 0.499303 | 0.449758 | 0.000 |
| Both: the engine's network with the 8-filter map's pools beside its own | 2.5M | 0.499262 | 0.449050 | |
| Feature map, 16 filters, the 3x3 box with the whole rank and file | 278k | **0.498524** | **0.448705** | 0.000 |

E58's 0.0073 was the small set's. On every game the 8-filter map trails by 0.0011 against results,
and the 16-filter one ties the engine's network against results and leads it by 0.0008 against the
target, with a ninth of the parameters and still underfitting, so 32 filters would gain a little
more. Added beside the engine's network, the map adds nothing. Every shape tried lands within
0.0013 of the others: the network seems near what a static evaluation learns from these games, and
the errors it keeps, in crowded positions with captures to make, may be ones only a search finds.

**What a feature map would cost**, from `yapper diag fmcost` in the same copy: the 8-filter map
in the engine's integer form on 300 positions, against the engine's network on the same moves.
Its update adds the changed squares' rows to every square whose window holds them, keeps each
square's pooled part, and moves the pools by the squares it changed:

| Cycles | Today's network | Feature map, 8 filters |
|---|---|---|
| Update from three plies back, as the search's frames are | 906 to 936 | 2945 to 3184 |
| Score | 570 | 537, with a head left unvectorised |
| A whole list's prior | 14,800 to 15,100, `policy.c` | 4,970, read off the frame |

About three times today's update, most of it the rows: each changed square reads 100 rows spread
over a 1.85 MB table. Against a gain of nothing at 8 filters and of 0.0008 against the target at 16,
it does not pay as an evaluation.

**The prior is the weak part.** On 4000 positions of gen 11's 6x6 games that policyfit held out,
how often each names the 20,000-node search's move (`diag vpol`, `diag nngrad`):

| Chooser | Matches |
|---|---|
| `policy.c` | 21.8% |
| The network's best child, its static score alone | 25.4% |
| The same, with a child that leaves a road to finish scored as lost | 27.5% |
| A depth 1 search, 2, 3 and 4 | 34.0%, 40.1%, 42.9%, 47.0% |
| The network linearised at the node, each move scored by the gradient times its rows | 12.9% |

The value network one ply down names the search's move more often than the prior made for the job.
Linearising it would have scored a list for about the prior's cost, but a changed row moves the
clipped units too far for a first-order estimate: it correlates 0.38 with the exact child scores.
More games do not help the prior either: policyfit on one game in 4 of gens 10 and 11, twice what
it fits now, held out 21.87% to one in 8's 21.75%, its fit loss level with its held-out loss. It is
at the ceiling of its features.

A prior read off the 8-filter map's trunk, fitted with the value on gens 10 and 11 for 5 epochs
(`external/build/arch/proto3.c`, the side to move's units of a placement's square pooled over the
orientations, a spread's source and landings read turned to its direction, and policy.c's road
and last-move squares as flags), named 23.4% top 1 and 47.4% top 3 on the positions both held out,
where policy.c names 21.1% and 43.5% there. A policy weight of 1.0 for 0.3 gained nothing and cost
the value 0.008 of held-out loss. The trunk sized for the value is small for the prior; the gain is
real but modest.

So no shape within reach makes the evaluation much better on these games, and the prior is the
weakest part measured: it ranks below what the evaluation knows one ply down, its features are
spent, and it takes 17% of the search. Whether a better order is what the failed search changes
were missing is for an SPRT to say; it is the lever this measurement leaves.

## E62: a prior to match a depth 1 search, and ordering by one

E61 left the prior below the value network one ply down. The user's plan: a prior that names the
20,000-node search's move 30% to 35% of the time would match a depth 1 search more cheaply than
searching every move; failing that, order by a depth 1 search where the tree can pay for it.

**What there is to match.** On the 4000 positions of E61's `diag vpol`, split by whether a road
hangs, since a win on the spot never reaches the order and a road to answer limits it to the
answers:

| Names the search's move | Quiet, 92.5% | A road to answer, 7.5% |
|---|---|---|
| `policy.c` | 20.9% | 32.8% |
| The value network's best child | 26.7% | 37.1% |
| A depth 1 search | **32.4%** | 52.6% |
| A depth 2 search | 38.9% | 54.3% |

The label itself is noisy: a fresh 20,000-node search of the same position names the stored move
56.9% of the time and an 80,000-node one 59.7%, and the two fresh ones agree on 69.9%. The ceiling
is far above any of these.

**A prior fitted on the GPU.** `external/build/arch/gpolicy.c` with its kernels in `gpolicy.cu`,
through a copy of `src/gpu.c` that adds dynamic shared memory: the feature map of E58 as a trunk,
each square read in each of the eight orientations of the board by a layer of rectified units that
also see policy.c's road and last-move squares and the lead, reserves, komi and bucket; a placement
averages its square's frames, a spread reads its source and each landing in the two frames that turn
north to its direction. Everything reads alike under the symmetries. Held out: the games policyfit
holds out, where policy.c is scored on the same positions. Each step on a quarter of gens 10 and 11
for 3 epochs, then all of them:

| Prior | Top 1 | Top 3 |
|---|---|---|
| policy.c, on the same positions | 21.97% | 45.03% |
| Trunk of 8 filters, 32 hidden units | 22.91% | 46.32% |
| ... and what a spread does: the top it leaves at each landing against what it covers, what is left on the source, the tops it wins | 23.76% | 47.79% |
| ... trunk of 16 | 24.28% | 48.65% |
| ... and the value network's 128 pools beside the lead | 25.57% | 50.77% |
| ... on all of gens 10 and 11, 16.7M positions | **28.33%**, quiet 27.54% | **54.76%** |
| policy.c on all of them | 21.92%, quiet 21.10% | 44.99% |

Games did most: four times as many gained 2.8 where every change to the network gained 0.5 to 1.3.
It still underfits, fitted top 1 level with held out, so more games would gain more; the 20,000-node
games not yet read, gen 9's and gen 12's, add about half again. It does not reach a depth 1 search
on quiet positions, 27.5% to 32.4%, and it is not cheap: 288 frames by 256 inputs by 32 units is
about 2.4M multiply-adds a list, 75,000 to 100,000 cycles, half of what a depth 1 search of the list
costs and seven times today's prior. Where it could pay, it competes with the depth 1 search itself.

**Ordering by a depth 1 search** (`external/build/e62`). From `OrderSearchDepth` in `src/tune.h`,
remaining depth and so near the root, each move of the list is played and its reply searched by the
leaf search, and the list goes by that score; the table move and killers stay first, and the old
order breaks ties. Bench, a fixed depth, so fewer nodes is a smaller tree, the leaf searches counted:

| From remaining depth | Never | 3 | 4 | 5 | 6 | 8 |
|---|---|---|---|---|---|---|
| Bench nodes | 1060614 | 2688287 | 1519656 | 1088682 | **974537** | 1003932 |

Against E54, 2+0.02, [0, 10], one change each on the same work copy, `OrderSearchMode`:

| From remaining depth 6 | Bench | Result |
|---|---|---|
| The leaf search's score replaces the order | 974537 | **FAILED**, 790 games, +258 -314 =218, **-24.7 +/- 19.4**, pairs [53, 87, 154, 65, 36] |
| Its score added to the order, 32 history units a hundredth | 1032611 | **FAILED**, 700 games, +218 -278 =204, **-29.9 +/- 21.3**, pairs [55, 75, 125, 65, 30] |
| Only its best move put first, after the table move and killers | 1125857 | **FAILED**, 692 games, +215 -279 =198, **-32.2 +/- 21.8**, pairs [59, 71, 121, 65, 30] |

No game was lost on time. The bench misled: its 13 positions are openings and other sizes. On 180
6x6 positions from these games, every 10th ply from the 14th, each searched to depth 10 from a new
game (`external/build/e62/ttd.py`, over TEI):

| Engine | Nodes to depth 10 | Time |
|---|---|---|
| E54 | 21.19M | 12.8 to 13.5 s |
| Replaces the order | 25.82M, **+22%** | 15.3 s |
| Best move first | 24.93M, +18% | 14.9 s |
| Added to the order | 24.88M, +17% | 14.8 s |

The leaf searches cost about a sixth of the search, and the order they give makes the main tree no
smaller: the version that changes it most is the largest. Naming the deep search's move at the root
is not what orders a node inside the tree, where history has seen which moves refute in this very
search; a depth 1 search names the root move half again as often as the prior and still orders no
better. That speaks against chasing the prior's top 1 as well: a better prior on that measure
need not be a better order, and the one here costs seven times today's. Time to depth on real
positions is the measure to take before an SPRT, not bench.

## E65: four pieces tested for harm, and a refactor

A review of the code for what is more than it needs to be found the network lean. Measured with
counters on bench, in a scratch copy that benches 1060614: the straight sum of the pools has a
standard deviation of 253 hundredths, the head 126 and the skip 553, and the head correlates with
the skip at 0.61, so the straight sum is the main positional path and not a leftover of E48's warm
start. Every one of the 48 heads has every second-layer unit weighted in and out. 3x3's buckets 6
and 7 and 4x4's bucket 7 are still at their starting weights, since no fitted position reaches them.

**The refactor**, committed as the baseline in ed3bbba. One `move_squares()` beside the move accessors, for
`nn_move_squares()` and the prior's copy of it. One `board_flat_lead()` and `board_komi()` for the
four copies of the flat count with komi: `flat_outcome()`, the network's inputs, and twice in the
prior's context, which now keeps it in `PolicyContext`. The hand order's road masks are built only
at depth 1, where they are read; from depth 2 the prior's context built the same two again. Bench
1060614 from clang native and generic, gcc POPCNT, `make release` and `make TUNE=on`. `make check`
passes, perftdiff found 0 differences in 101,986 positions, and gcc's selftest passes 987,121,304
checks.

**Four pieces**, each removed alone and played against the refactor: first a non-regression test,
then, if that passed, a gain test of the removal on a fresh seed. A piece counts as harmful only if
taking it out passes the gain test. 2+0.02 on 6x6 with 2 komi, 7 games at once, nothing else
running, and no game ended on time.

| Removed | Bench | Non-regression, [-20, 5] | Gain, [0, 10], seed 2 |
|---|---|---|---|
| a: the evaluation's price for a road threat, 60 and 220 | 1012070 | PASSED, 1134 games, +1.5 +/- 17.1, pairs [67, 127, 182, 116, 75] | **FAILED**, 1742 games, **-8.2 +/- 13.0**, pairs [97, 193, 325, 166, 90] |
| b: the order's flat, flat-near and capstone terms at depth 1 | 1197290 | PASSED, 882 games, +3.9 +/- 19.2, pairs [54, 87, 150, 95, 55] | **FAILED**, 1504 games, **-11.3 +/- 14.5**, pairs [100, 155, 273, 142, 82] |
| c: the history of what answered what | 1086287 | PASSED, 284 games, +23.3 +/- 31.0, pairs [10, 28, 53, 35, 16] | INCONCLUSIVE at the cap, 2000 games, **-0.5 +/- 12.5**, pairs [116, 209, 356, 200, 119] |
| d: the prior's position features, square classes and lines, against d0 | 1147531 | PASSED, 1286 games, -0.0 +/- 15.3, pairs [72, 129, 242, 127, 73] | INCONCLUSIVE at the cap, 2000 games, **+3.8 +/- 12.4**, pairs [104, 218, 349, 210, 119] |

None is harmful, and taking out any of them costs nothing measurable either. c's +23 came from a
test that stopped at 284 games, and the gain test's 2000 put it at -0.5.

a's price reaches a score the search returns in 0.08% of bench's evaluations, at quiesce's depth
cap or with the last stones going in, since reverse futility, null move and quiesce's stand pat all
pass a node with a road hanging by. It flips `improving` at 0.30% of the nodes that reach
move-count pruning. b's terms date from the first commit and were never measured alone. c's only
measurement was a row of E24's bundle, on the hand-written evaluation, before any prior.

d's two priors were fitted by DATA.md's recipe, seed 0, 16 threads, so that they differ only by
their features: 1314 for d0 and 1210 without the three groups. Held out, d0 2.7999 and d1 2.8449,
read by the engine at 2.8001 and 2.8451; top 1 21.73% and 21.06%. d0 benches 951898, since the
staged prior came from E52's two-thread fitter. E52 priced the groups one at a time at 0.006, 0.011
and 0.018; together they cost 0.045 and play the same, as E62 found for a prior's accuracy.

**The cut.** The user chose c and d, which went together: the history of what answered what, and
the prior's position features, square classes and lines, with `policy_context_features()`,
`class_of()`, `run_through()` and the position's rows in `policyfit`. The prior is d1's, and the
cleaned fitter writes it again to the byte. With the history kept, the cut benches d1's 1147531;
with both gone, 928599 from clang native and generic, gcc POPCNT, `make release` and `make
TUNE=on`. `make check` and `make datacheck` pass, and gcc's selftest passes 984,138,986 checks.

| Match | Result |
|---|---|
| Against the refactor, the engine that shipped, 2+0.02, [-20, 5], seed 3 | **PASSED**, 218 games, +75 -61 =82, **+22.3 +/- 30.5**, pairs [4, 23, 43, 33, 6] |

A test that stops this early reads high, as c's first did. What it says is that the cut, with d1's
fit in place of E52's, loses nothing. a and b stay: their gain tests leaned the other way.

**The first prior's biases.** `policy_context()` copied the prior's biases before the first call
installed them, so the first node each process scored had none, as E54 found. The context no
longer carries them, and `policy_logits()` reads them after installing. Before the fix a second
bench in the same process read 902225; now every bench reads it, the first included, so nothing
past that one node changed. Bench 902225 from clang native and generic, gcc POPCNT, `make release`
and `make TUNE=on`; `make check`, `make datacheck` and gcc's selftest pass. No match was played
for one node a process.

The variants are in `external/build/e65`, each tree with its own `src`, and `chain.sh` ran them;
the cut's binary is `external/build/e65cut/e65cut.exe`.

## E66: the first SPSA run, 38 tunables at once

E54's tuner, run for the first time, on E65's engine (bench 902225). `tools/tune.py --name
e66-test --pairs 10000 --tc 2+0.02 --book tests/books/6s_4ply.txt --concurrency 8 --r-end 0.004`,
the seven `Time` tunables excluded since a test at 2+0.02 reaches them only as one clock. 10,000
pairs in 3 h 32 m, seed 914742700, nothing else running, one game of 20,000 lost on time. Its own
tally, +7299 -7259 =5442, is each pair's plus side against its minus side, so it measures nothing.

Before the run, tune.py learned to pause: `--pause` lets the pairs in flight end and exits,
`--resume` carries on, a run plays its own copy of the engine, and its workers die with it, where
before a killed tuner left 8 workers and 16 engines playing. `tools/progress.py` shows an unfinished
run as paused. A 300-pair test paused, was killed twice and resumed to the end, with the state and
the per-pair history agreeing each time.

Where it ended, rounded, for the 29 that moved a whole step; the other nine rounded back to their
defaults (RfpDepth, the three null move numbers, LmpBase, LmrMoves, LmrCutNode, LmrPvNode,
AspirationDepth):

| Tunable | Was | Now | | Tunable | Was | Now |
|---|---|---|---|---|---|---|
| QuiesceDepth | 4 | 3 | | HistoryLimit | 16384 | 12966 |
| RfpMargin | 90 | 49 | | HistoryKeep | 8 | 10 |
| SingularDepth | 6 | 5 | | PriorWeight | 6 | 5 |
| SingularTtDepth | 3 | 2 | | PriorDepth | 2 | 3 |
| SingularMargin | 32 | 44 | | OrderFlatNear | 300 | 262 |
| LmpDepth | 6 | 8 | | OrderFlat | 100 | 114 |
| LmpSlope | 6 | 4 | | OrderCap | 150 | 197 |
| LmpImprovingBase | 24 | 22 | | OrderRoadOurs | 8192 | 7710 |
| LmpImprovingSlope | 12 | 7 | | OrderRoadTheirs | 4096 | 3916 |
| LmrBase | 50 | 39 | | OrderSwing | 256 | 367 |
| LmrDivisor | 160 | 170 | | AspirationWindow | 40 | 25 |
| LmrDepth | 3 | 2 | | AspirationGrowth | 200 | 176 |
| HistoryBonusMul | 16 | 19 | | AspirationLimit | 500 | 482 |
| HistoryBonusMax | 4000 | 3983 | | ThreatOne | 60 | 36 |
| | | | | ThreatMany | 220 | 217 |

The largest moves for their range: reverse futility's margin nearly halved, the price of one road
threat down by 40%, the aspiration window narrowed, move-count pruning out to depth 8 with a
gentler slope, one ply less of quiescence, and the prior used from depth 3 rather than 2.

| Match | Result |
|---|---|
| The candidate against the defaults, the same tune build both sides, 2+0.02, [0, 5] | **PASSED**, 982 games, +431 -288 =263, **+51.0 +/- 17.6**, pairs [32, 77, 182, 116, 84], no game on time |
| Against syntaks at full strength, 8+0.08 each, 2 komi, racetrack, stopped at 509 of 1000 | +49 -434 =26, **-343 [-387, -297]**; white +10 -228 =17, black +39 -206 =9 |

Bench 1485338 from `make`, `make release` and `make TUNE=on`, and from the tune build that played
the SPRT given the candidate's options, so what is staged is what was tested. `make check` passes.
Not yet done: a confirmation against the old defaults at another time control, which TESTING.md
asks for, and a run with the clock's tunables. The binary is `external/baselines/e66.exe`.

**Where the 434 losses come from**, read from the PTN by `tools/gamestats.py` and a scratch script
over the scores racetrack keeps. The user heard on the Tak Discord that syntaks is the strongest
engine by a wide margin, so this is the gap at the top rather than a fault.

- Not the clock. Each side used about 10 s a game, the median 10.1 for Yapper and 10.3 for syntaks;
  syntaks spends more in the opening, mean 0.44 s a move before ply 20 against 0.19, and less after.
  On that time it reports depth 15, 14 and 12 by phase against 13, 13 and 11, though two engines'
  depths need not mean the same.
- Mostly flats, by a little. 300 losses on flats with a median margin of 2 flats, 134 by road, 130
  of them finished by a spread. syntaks ended all 300 flat losses itself, placing its last stone;
  by then Yapper scored itself near -10. In 82 losses Yapper scored itself above +1 in the last
  third of the game.
- Decided in the middle game, at level flats. Taking the turn as the ply after which syntaks scores
  Yapper below -1 for good, 428 losses have one: 80 before ply 20, 188 from 20 to 49, 160 at 50 or
  later, median ply 40. At the turn the flat count with komi is level (median 0) and Yapper's own
  score just before it is -0.18; Yapper's score falls below -1 a median of 6 plies later (p90 21).
- syntaks foresees the result better at every stage. Where a score is past 0.3 either way, its sign
  matches the result for syntaks at 56, 70, 77, 83 and 89% at plies 20, 30, 40, 50 and 60, and for
  Yapper at 44, 52, 61, 70 and 76%. Yapper loses 85% of these games, so its numbers say it is too
  hopeful, not that it is worse than chance.
- Too hopeful as black. Of 41 losses turned between plies 20 and 50 with Yapper above +0.3 just
  before, 32 had Yapper as black, which with 2 komi is the side the score already favours.

So the gap is judgement more than speed: positions that count level and that syntaks already
reads as lost, about three of Yapper's moves before Yapper does. The scripts are
`external/analysis/analyse.py` and `analyse2.py`; the games are `external/games/tak/e66-stc.ptn`.

## E67: the scale a score is read through in the loss, by size

The user's direction on 2026-11-08: make the network better through its data, and since that
morning without another engine's play in it, DATA.md. Before generating anything, gen 11's own 6x6
games, 100,000 of them, were read for how well the stored 20,000-node scores foretell the results.
The fit turns a score into a chance of winning as `sigmoid(score / 310)`, and self-play's results are
far more decisive than that:

| Plies | Positions | Scale that fits best | Loss against results, at 310 and at best |
|---|---|---|---|
| 0 to 9 | 182,954 | 140 | 0.5252, 0.4675 |
| 10 to 19 | 850,360 | 140 | 0.4975, 0.4413 |
| 20 to 29 | 989,219 | 160 | 0.4697, 0.4195 |
| 30 to 39 | 986,648 | 160 | 0.4417, 0.3955 |
| 40 to 59 | 1,941,602 | 180 | 0.3922, 0.3561 |
| 60 to 79 | 1,771,250 | 180 | 0.3175, 0.2870 |
| 80 to 119 | 1,017,334 | 180 | 0.2882, 0.2611 |

By size, on all scored plies: 120 for 3x3 and 4x4 (gen 1), 140 for 5x5, 160 for 6x6's gen 11 and
170 for its gen 10, 240 for 7x7 and 280 for 8x8, where 310 costs nothing (0.4911 to 0.4903). E37
had read 240 on 6x6's earlier games; a stronger engine turns a small lead into a win more surely.
With lambda 0.7 the fit has been blending a search target that is too timid with results that
disagree with it, and the gradient on level positions, where E66 found the games against syntaks
are lost, is the weaker for it. Two smaller findings from the same games: a side's score moves by
37 rms from one of its moves to its next in level positions at plies 20 to 40, and 49 at 40 to 60;
and at 2 komi the first scored ply's mean score foretells white's result to the point (0.482
against 0.478), while at 0 and 3 komi it under-reacts (0.554 against 0.596, 0.440 against 0.408).

`nntrain --scale 6=170`, or `--scale 170` for every size, sets the scale the loss reads a score
through: the target is `lambda * sigmoid(score / s) + (1 - lambda) * result`, the prediction
`sigmoid(output * 310 / s)`, and the slope gains the factor `310 / s`. The engine's units stay
NN_SCALE's, since the search's score in the target holds the output to them, so no margin in the
search moves. The GPU kernels read the ratio by size from a constant array; at the default it is
exactly 1 and a fit keeps its bytes. **Checked: E49's recipe from E48's network, on the GPU, writes
E49's `net.h` byte for byte** with the option in, so this is E49 with one thing changed. (A first
build printed the array's ones as `1f`, which NVRTC refused, and both fits fell back to the CPU,
whose control then differed from E49 in the float rounding; the CPU fits are kept in
`external/build/e67/cpu`.)

One scale of 170 for every size, E49's recipe otherwise, fitted 6x6 better and 7x7 and 8x8 worse
than the control after its first epoch (0.5091 and 0.5733 against 0.4943 and 0.5463 against
results), so the scale is by size: 120, 120, 140, 170, 240 and 280 for 3x3 to 8x8. Held out,
against results, E49's recipe and the same with the scales by size:

| Group | E49, 310 | By size |
|---|---|---|
| 5x5 gen 11 | 0.387641 | 0.346770 |
| 6x6 gen 10 | 0.419904 | 0.396853 |
| 6x6 gen 11 | 0.416033 | 0.387644 |
| 7x7 gen 11 | 0.462961 | 0.460220 |
| 8x8 gen 11 | 0.504147 | 0.504338 |

Most of the 5x5 and 6x6 gain is the reading of a score as a result, not a better order of
positions, which only play can tell. The engine reads within 0.58 of the fit on average, 4 at
worst. Fits and logs in `external/build/e67`.

| Match | Result |
|---|---|
| Scales by size against E66 (E49's network), 2+0.02, [0, 10] | **FAILED**, 1532 games, +518 -563 =451, **-10.2 +/- 14.0**, pairs [98, 146, 304, 139, 79], one loss on time |

Calibrated to its own results, the network plays no better and leans worse. At 170 a position
three flats up is already read as 85% won and gets little gradient, so the scores past that, which
the search's margins read when it prunes, are learned less well; and the weight moves onto level
positions, which the network already ranks about as well as it can. The default stays 310, and
`--scale` stays as an option at no cost.

## E68: the search's score smoothed over the next plies

E67's second finding: a side's stored score moves by about half a flat from one of its moves to its
next in level middle games, so one search's score is a noisy target, and the searches after it see
further. `nntrain --smooth n` replaces a position's score in the target with the mean, as a chance
of winning, of the next n scored plies' scores, each read for the side to move at the position; a
deviation's unscored ply is skipped and a forced result counts as 0 or 1. The data is unchanged,
the trainer reads ahead in the game it already holds. E49's recipe from E48's network with
`--smooth 4`, on the GPU, in `external/build/e68` (to be staged only if it passes):

| Held out, against results | E49 | Smoothed over 4 |
|---|---|---|
| 5x5 gen 11 | 0.387641 | 0.383223 |
| 6x6 gen 10 | 0.419904 | 0.417741 |
| 6x6 gen 11 | 0.416033 | 0.413635 |
| 7x7 gen 11 | 0.462961 | 0.462664 |
| 8x8 gen 11 | 0.504147 | 0.504035 |

The scale is the same, so these compare, but a target drawn from later scores sits nearer the
result and so scores nearer it, as a higher lambda would: the SPRT decides. The engine reads within
0.77 of the fit on average, 6 at worst on 3x3.

| Match | Result |
|---|---|
| Smoothed over 4 against E66 (E49's network), 2+0.02, [0, 10] | INCONCLUSIVE at the cap, 2000 games, +741 -660 =599, **+14.1 +/- 12.7**, LLR +2.15, pairs [111, 193, 332, 232, 132], bench 1534984, none on time |
| The same at `--seed 2`, another order of the book | **PASSED**, 1708 games, +640 -547 =521, **+18.9 +/- 13.4**, pairs [84, 158, 305, 195, 112], none on time |
| Both, pooled | 3708 games, **+16.3 +/- 9.2**, pairs [195, 351, 637, 427, 244] |

A first build of it was E49 by mistake, its chain having deleted the network before copying it;
the refit gave the same bytes, as a GPU fit does. The later scores of a game are a better target
than one search's: about 16 Elo at no cost in data or play.

## E69: gen 13, from our own seeds and people's games

Gen 13 is 6x6 only, played by E66, with no other engine's play in it (DATA.md): 130,395 games, 12.7M
positions, `external/data/gen13`, takref replaying 652 of them without a fault. Three sets, each at
20k nodes and at 60k nodes for the same time, about 100 minutes a cell:

| Set | Seeded from | 20k | 60k |
|---|---|---|---|
| self | random plies, as before | 27,000 | 9,000 |
| variants | positions from Yapper's own match games, 46 plies in on average (`mkopenings.py`) | 38,400 | 12,795 |
| humans | positions from 19,381 PlayTak games between people (`playtak.py`) | 32,400 | 10,800 |

Nearly every position of every set, gen 11's included, is new, so variety was counted by kind:
stacks (height, top, the colours of the top six) and positions (empty squares, tallest stack,
walls, capstones, flat lead, reserves). Self-play is gen 11 again. The variants have the most
stacks of a kind gen 11 rarely shows (1.8% of them against 1.1%), tall stacks and walls of the late
middle game, and a quarter of their games drawn; the human-seeded games spread widest over kinds
of position (18.8% of a kind under one in 10,000 of gen 11's, against 14.2%). Road wins fell to
5-6% of the games in every set, self-play's too, from 8-9% in gen 10 and 11.

Seven fits from E49 by its recipe, gen 13's part listed four times (one file a cell: 90 files
four times passed Windows' command line). Held out, against results:

| Fit | 6x6 gen 10 | 6x6 gen 11 | 6x6 gen 13 20k | 6x6 gen 13 60k |
|---|---|---|---|---|
| E49, before any step | 0.419904 | 0.416033 | 0.448693 | 0.447475 |
| control, no gen 13 | 0.419801 | 0.415905 | | |
| all of gen 13 | 0.415989 | 0.411143 | 0.441786 | 0.439708 |
| its 20k half | 0.416674 | 0.411997 | 0.443109 | |
| its 60k half | 0.418276 | 0.413980 | | 0.444487 |
| self alone | 0.417755 | 0.413354 | | |
| variants alone | 0.418594 | 0.414465 | | |
| humans alone | 0.418131 | 0.413787 | | |

Gen 13 was far from what E49 knew, 0.033 worse than gen 11 on 6x6, and fitting on it improved gen
11's held-out games too, each set alone included. Then two fine-tunes of the fit on all of it, at
rate 0.0002 for two epochs: on every game of 20k nodes and up (3x3 and 4x4 keeping their 5k games,
which are all they have), and on gen 13's 60k games alone for 6x6, the other sizes at a twentieth.

| Match, 2+0.02, [0, 10] | Result |
|---|---|
| All of gen 13 against E66 (E49's network) | **PASSED**, 730 games, +282 -208 =240, **+35.3 +/- 19.7**, pairs [24, 69, 129, 95, 48], bench 1341109 |
| The 60k half against the 20k half | **FAILED**, 976 games, +313 -368 =295, **-19.6 +/- 17.8**, pairs [63, 117, 166, 96, 46] |
| 20k+ fine-tune against all | INCONCLUSIVE at the cap, 2000 games, +749 -683 =568, **+11.5 +/- 12.6**, LLR +1.56, pairs [105, 205, 345, 209, 136], bench 1350467 |
| 60k fine-tune against all | **FAILED**, 1446 games, +504 -555 =387, **-12.3 +/- 14.9**, pairs [97, 155, 256, 132, 83] |

None lost on time. For the same machine time, three times the games at 20k nodes beat a third of
them at 60k: more positions teach more than deeper scores do, at these budgets. Finishing on the
deeper games alone forgets; finishing on everything at 20k and up leans better, and gets a second
SPRT at `--seed 2`. A fine-tune's held-out losses do not compare with the fits above: it reads
other games.

## Mistakes worth not repeating

- **Bench is not a tree-size measure.** Its 13 positions are mostly openings and other sizes. E62's
  leaf-search order benched 8% fewer nodes and took 17% to 22% more on real 6x6 middlegames, then
  lost 25 to 32. Time to depth on positions from games (`external/build/e62/ttd.py`) first.

- **A racetrack run can hang with both engines idle.** E52's Topaz gauntlet stopped on its last
  game for four hours, neither engine thinking, and everything queued behind it waited. Run
  racetrack under `timeout`, and watch the queue in `tools/progress.py`, which marks a run
  stalled.

- **A prior that predicts better does not order better by as much.** E52's ranked the cutting
  move first a third more often than E51's, and the search's order gained 1.5 points, because
  history already knew most of it; it paid only once history was halved at each search. Measure a
  prior inside the tree, beside history, before paying for it.
- **Two bits taken from a score change its ties.** Masked to carry a flag, they moved bench by
  7.5%; shifting the score up to make room left it alone, E52.

- **An idea that times slower may be the compiler's fault.** E32 dropped a single pass over the
  accumulator as slower. With the slice held in vector registers it is 5% faster, E43. Check
  what the compiler made of an idea before dropping the idea.

- **A build with no network still plays.** One candidate's weights went in under a file name the
  engine no longer read, so it counted flats on every size and lost 274 games of 274. Its bench was
  new, which passed for a new network. A network that changes bench must also change it from a
  build with none.
- **An SPRT can fail on no evidence.** On 3x3 the side to move first won all 408 games, every pair
  went 1-1, and the test read FAILED in three seconds. Read the pairs before the verdict: a test
  where nothing can differ says nothing about either side.
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

- **A match against another engine needs a quiet machine.** A GPU fit on two threads lifted the
  same binary by 72 against syntaks at an eighth and 94 against Topaz, E42. Self-play shares any
  slowdown between its sides; another engine does not. Run nothing beside a gauntlet, and compare
  only gauntlets run alike.
- **A wait on a log line must wait for a line nothing else prints.** A queued gauntlet waited for
  `exit` in an output that began with an earlier job's `exit 0`, and started at once beside an
  SPRT, E42.
- **A compiler can merge two stages into one slower one.** Inlined, the second layer's pooling
  and its rows became one function that rebuilt the pools for every row: 210 ns a score where the
  parts apart take 158, E42. Time each part alone before deciding what a design costs.
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
