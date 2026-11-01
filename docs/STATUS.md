# Status

**The current goal is move generation speed.** Beating the state of the art means winning on
that first. Evaluation and strength come later, once the basics are settled.

## Done and verified

| Piece | State | Evidence |
|---|---|---|
| Rules, sizes 3-8 | Done | Published counts on 5x5 and 6x6, takref on all sizes, one selftest position per rule |
| Board, make/unmake | Done | 549M invariant checks; clean under a UB trap build |
| Move generation | Done, and leading | Faster than every baseline on every position raced: EXPERIMENTS.md E5 |
| TEI | Done | 40 games through the real racetrack with no protocol faults |
| Match runner and SPRT | Done | Known-answer tests, EXPERIMENTS.md E1-E3 |
| Playing it yourself | Done | `python tools/play.py` |
| Portability | Partly | Compiles for AArch64 with NEON; passes every gate with no CPU assumptions on x86-64. **Not yet run on ARM**: CI has a real ARM runner, and it runs on the first push |

## Skeleton only

| Piece | State |
|---|---|
| Search | Textbook PVS on one thread. No margin has been measured |
| Evaluation | Hand-set placeholder, and it shows: E6 |
| Time management | Simple, and only checked for not flagging |

## Not tested yet

- **Running on ARM.** See above.
- **PTN Ninja.** Tags and the link format were checked against its documented formats, but no
  game has been opened in it yet.

## Baselines

`external/engines/` holds prebuilt binaries for speed races and matches, with their commits
in `VERSIONS.txt`. They are gitignored, so a fresh clone does not have them.

**They are black boxes.** Do not read rival engines' source, not to explain a result and not
to see how something is done. Yapper's ideas stay its own. To rebuild one, build it in a
throwaway folder, keep the binary, and delete the source and the toolchain with it.

## Next

1. Widen the speed lead. `make speed` is the scoreboard, and a pure speedup must leave the
   bench node count unchanged.
2. Run on real ARM and measure it, since that is a stated target.
