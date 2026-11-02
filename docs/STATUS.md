# Status

**The current goal is to beat Tiltak.** Move generation leads every baseline, so strength is
where the gain is now. The order is a stronger evaluation base, then the search, then long
overnight runs on the evaluation, with networks replacing the hand-written one later.

## Done and verified

| Piece | State | Evidence |
|---|---|---|
| Rules, sizes 3-8 | Done | Published counts on 5x5 and 6x6, takref on all sizes, one selftest position per rule |
| Board, make/unmake | Done | 576M invariant checks; clean under a UB trap build |
| Move generation | Done, and leading | 9.9 to 12.6 times the fastest engine on bulk, and 6.0 to 7.5 times on full where only Topaz and Tiltak can be compared, on every position raced: EXPERIMENTS.md E8 and E13 |
| TEI | Done | 40 games through the real racetrack with no protocol faults |
| Match runner and SPRT | Done | Known-answer tests, EXPERIMENTS.md E1-E3 |
| Evaluation | Base done, fitted to self-play | +414 -0 on the old one at 10+0.1: EXPERIMENTS.md E18. Five generations have converged, so more needs new terms or new data |
| Search | First pass done | **+116 [+92, +140] against Tiltak over 800 games at 5+0.05**, from -38: EXPERIMENTS.md E25. It sees roads by spread, and most of the gain is speed in real middlegames |
| Time management | Done for now | Budgets on the moves left, from the shorter reserve. One time loss in 5242 games, and it does not use the whole clock yet: E25 |
| Training data | Done, ready for networks | Every position recoverable from about four bytes a ply, repeatable, checked by takref: docs/DATA.md |
| Playing it yourself | Done | `python tools/play.py` |
| Portability | Partly | The hot files compile for AArch64 with NEON; passes every gate with no CPU assumptions on x86-64, at about 80% of native speed, with clang and gcc giving the same bench. **Not yet run on ARM**: CI has a real ARM runner, and it runs on the first push |

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

**Two newer engines joined the race on 2026-11-01:** `Ciekce/syntaks` (Rust, TEI, under active
development) and `87flowers/pentakle` (Rust, perft only in this build). Their perft does not make
the last ply, which is proved for syntaks and inferred from timing for pentakle, so they race on
bulk alone: E13. syntaks is far stronger than anything else here: it beat the build of E25 +99 -1
at 5+0.05, E26, which that build leads Tiltak by 116 Elo at.

Rebuilding them on Windows takes three things. Use a short folder for the clones, `RUSTUP_HOME`
and `CARGO_HOME`, because a long one breaks both cargo's git checkouts and the linker. syntaks
needs `WindowsProgramming::INFINITE` changed to `Threading::INFINITE` in its `atomic-wait`
dependency, a third-party library and not an engine. pentakle needs `cargo +nightly`. Build with
`--message-format=short`, which leaves source lines out of the errors.

## Next

1. The rest of the search, about 46 Elo short of the +200 it was asked for. First stronger
   reductions, which read +27.0 +/- 13.9 and were left out unfinished. Then the aspiration window
   and reverse futility's margin, neither of which has been tested, and the clock's floor: E25.
2. Why Yapper loses the race to run out of stones, which is 196 of its 228 losses to Tiltak: E25.
3. Long overnight runs on the evaluation, then networks. All the data so far came from engines that
   could not see a road by spread at the leaf, so a new generation is due.
4. Run on real ARM and measure it, since that is a stated target.
5. Make and unmake have plateaued at about 3 ns a node on the start position and 8.5 ns on the
   endgame: E7 lists what no longer helps. Any further lead has to come from an idea that is not
   on that list. A pure speedup must leave the bench node count unchanged.
