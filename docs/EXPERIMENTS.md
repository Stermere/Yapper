# Experiments

Every measurement goes here, with the conditions that produced it. Machine unless noted:
Ryzen 7 5800X, Windows 11, gcc 12.2 `-O3 -flto -march=native`, one thread.

Bench signature at the end of the skeleton: **49104542 nodes**, identical from `make` and
`make ARCH=generic`.

## Speed

Nodes per second, single thread, with the hash and road check running as they do in search.

| Position | Depth | Nodes | make/unmake every leaf | Bulk count at the leaves |
|---|---|---|---|---|
| 6x6 start | 5 | 1,253,506,520 | 112 M/s | 3.1 G/s |
| 6x6 endgame | 4 | 395,359,484 | 48 M/s | 1.5 G/s |
| 8x8 start | 5 | 26,642,455,192 | 118 M/s | 6.0 G/s |

The endgame is the honest number: the start positions are nearly all placements. The
full published suite, depth 5 of that endgame included (48,986,506,534 nodes), takes 36
seconds. Search runs at about 3.6 M/s with the placeholder evaluation.

The race against other engines is E5.

## Correctness

| Check | Result |
|---|---|
| `published.perft`, 9 positions, full depth | 0 failures |
| `sealed.perft`, 144 positions, sizes 3-8 | 0 failures |
| selftest | 549,279,701 checks, 0 failures |
| perftdiff, 30 games per size, depth 3 | 63,624 positions, 0 differences |
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

## Mistakes worth not repeating

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
