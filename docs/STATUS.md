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
| Evaluation | Base done, fitted to self-play | +414 -0 on the old one at 10+0.1, and -38 [-85, +9] against Tiltak from about -636: EXPERIMENTS.md E18. Five generations have converged, so more needs new terms |
| Training data | Done, ready for networks | Every position recoverable from about four bytes a ply, repeatable, checked by takref: docs/DATA.md |
| Playing it yourself | Done | `python tools/play.py` |
| Portability | Partly | The hot files compile for AArch64 with NEON; passes every gate with no CPU assumptions on x86-64, at about 80% of native speed, with clang and gcc giving the same bench. **Not yet run on ARM**: CI has a real ARM runner, and it runs on the first push |

## Skeleton only

| Piece | State |
|---|---|
| Search | Textbook PVS on one thread. No margin has been measured, and none was set for this evaluation |
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

**Two newer engines joined the race on 2026-11-01:** `Ciekce/syntaks` (Rust, TEI, under active
development) and `87flowers/pentakle` (Rust, perft only in this build). Their perft does not make
the last ply, which is proved for syntaks and inferred from timing for pentakle, so they race on
bulk alone: E13. syntaks is the likely candidate for the strongest
engine now, though nothing found so far ranks it and no match has been played.

Rebuilding them on Windows takes three things. Use a short folder for the clones, `RUSTUP_HOME`
and `CARGO_HOME`, because a long one breaks both cargo's git checkouts and the linker. syntaks
needs `WindowsProgramming::INFINITE` changed to `Threading::INFINITE` in its `atomic-wait`
dependency, a third-party library and not an engine. pentakle needs `cargo +nightly`. Build with
`--message-format=short`, which leaves source lines out of the errors.

## Next

1. The search, starting with road wins by spread at the leaf. `board_road_placements()` only sees
   placements, so neither the threat term nor `quiesce` sees the way most roads are lost: E14, E18.
   Then its margins, which were never set for this evaluation: bench fell to under a quarter of
   its nodes at the same depth, and what that pruning costs is unmeasured.
2. Long overnight runs on the evaluation, then networks.
3. Run on real ARM and measure it, since that is a stated target.
4. Make and unmake have plateaued at about 3 ns a node on the start position and 8.5 ns on the
   endgame: E7 lists what no longer helps. Any further lead has to come from an idea that is not
   on that list. A pure speedup must leave the bench node count unchanged.
