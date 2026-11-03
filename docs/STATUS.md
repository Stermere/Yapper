# Status

**Tiltak is beaten, and syntaks is the bar.** A network on 6x6 led Tiltak by +428 over 800 games,
E33. The aim set for 2026-11-02 was to come within 300 of syntaks that day; the best network, v2,
read -576 [-665, -481] over 400 games, E34. Every size but 6x6 still plays the hand-written
evaluation.

## Done and verified

| Piece | State | Evidence |
|---|---|---|
| Rules, sizes 3-8 | Done | Published counts on 5x5 and 6x6, takref on all sizes, one selftest position per rule |
| Board, make/unmake | Done | 576M invariant checks; clean under a UB trap build |
| Move generation | Done, and leading | 9.9 to 12.6 times the fastest engine on bulk, and 6.0 to 7.5 times on full where only Topaz and Tiltak can be compared, on every position raced: EXPERIMENTS.md E8 and E13 |
| TEI | Done | 40 games through the real racetrack with no protocol faults |
| Match runner and SPRT | Done | Known-answer tests, EXPERIMENTS.md E1-E3 |
| Evaluation | A network on 6x6, hand-written elsewhere | v2, fitted on gens 1-8: **+137 +/- 28 on v1 at 2+0.02**, which led Tiltak by +428: E33, E34. Each new generation from the best network has paid. Other sizes play the hand-written evaluation fitted on 6x6 self-play: E27 |
| Search | First pass done | **+116 [+92, +140] against Tiltak over 800 games at 5+0.05**, from -38: EXPERIMENTS.md E25. It sees roads by spread, and most of the gain is speed in real middlegames |
| Time management | Done for now | Budgets on the moves left, from the shorter reserve. One time loss in 5242 games, and it does not use the whole clock yet: E25 |
| Training data | 6x6 only so far | 157.7M positions in gens 1-8, every one recoverable from about four bytes a ply, repeatable, checked by takref: docs/DATA.md, E34. None yet for other sizes |
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
bulk alone: E13. syntaks is far stronger than anything else here: v2 scored +9 -381 =10 against
it at 5+0.05, E34.

Rebuilding them on Windows takes three things. Use a short folder for the clones, `RUSTUP_HOME`
and `CARGO_HOME`, because a long one breaks both cargo's git checkouts and the linker. syntaks
needs `WindowsProgramming::INFINITE` changed to `Threading::INFINITE` in its `atomic-wait`
dependency, a third-party library and not an engine. pentakle needs `cargo +nightly`. Build with
`--message-format=short`, which leaves source lines out of the errors.

## Next

1. Close on syntaks. It searches about a quarter of Yapper's nodes a second yet reaches depth 18
   where Yapper reaches 13, so its search is far more selective, and most losses to it are on
   flats by a median of 2: E34. Giving the network each side's stones left did not help: E35.
2. Networks for the other sizes. A first generation for 3x3 to 8x8 is sized, about 4.5 hours, and
   not started; of the books it needs, only 3x3's is built. One network shared by every size is on
   the `nnue` branch and has not played.
3. The search items left from E25: stronger reductions, which read +27.0 +/- 13.9 with the
   hand-written evaluation, the aspiration window, reverse futility's margin and the clock's
   floor. Each needs testing again with the network.
4. Run on real ARM and measure it, since that is a stated target.
5. Make and unmake have plateaued at about 3 ns a node on the start position and 8.5 ns on the
   endgame: E7 lists what no longer helps. Any further lead has to come from an idea that is not
   on that list. A pure speedup must leave the bench node count unchanged.
