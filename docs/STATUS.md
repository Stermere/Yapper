# Status

**Tiltak is beaten, and syntaks is the bar.** A network on 6x6 led Tiltak by +428 over 800 games,
E33. The aim set for 2026-11-02 was to come within 300 of syntaks that day; the best network, v2,
read -576 [-665, -481] over 400 games, E34. A more selective search gained +82.5 on v2's own, and
read -665 [-784, -541] against syntaks, which at this gap cannot tell the two apart: E36. One
network now plays every size, at least as well as the networks of their own that replaced the
hand-written evaluation, and far better on 5x5, 7x7 and 8x8: E38, E39.

## Done and verified

| Piece | State | Evidence |
|---|---|---|
| Rules, sizes 3-8 | Done | Published counts on 5x5 and 6x6, takref on all sizes, one selftest position per rule |
| Board, make/unmake | Done | 576M invariant checks; clean under a UB trap build |
| Move generation | Done, and leading | 9.9 to 12.6 times the fastest engine on bulk, and 6.0 to 7.5 times on full where only Topaz and Tiltak can be compared, on every position raced: EXPERIMENTS.md E8 and E13 |
| TEI | Done | 40 games through the real racetrack with no protocol faults |
| Match runner and SPRT | Done | Known-answer tests, EXPERIMENTS.md E1-E3 |
| Evaluation | One network for every size | Filters on an 8x8 grid every board shares, and each size's own bias, output and skip. Against E38's networks of each size's own at 100,000 nodes: **8x8 +130, 7x7 +203, 5x5 +64**, passed SPRTs for its lambda 0.5 forerunner, then **+77 and +70 more on 8x8 and 7x7** from lambda 0.7; **4x4 +14.3 +/- 8.8**, passed, and +7.3 +/- 8.2 at 2+0.02; **6x6 +16.9 +/- 13.8 against v2, and +23.7 +/- 13.9 at 2+0.02**, both at the cap with their intervals above zero; 3x3 is decided by who moves first at every budget tried: E39. The hand-written evaluation went in E38 |
| Search | Second pass done | **+116 [+92, +140] against Tiltak over 800 games at 5+0.05**, from -38: EXPERIMENTS.md E25. It sees roads by spread, and most of the gain is speed in real middlegames. With the network, stronger reductions, move-count pruning to depth 6 and singular extensions: **+82.5 +/- 31.3 on v2's search at 2+0.02**, E36 |
| Time management | Done for now | Budgets on the moves left, from the shorter reserve. One time loss in 5242 games, and it does not use the whole clock yet: E25 |
| Training data | Every size | 6x6: gens 1-9, 217.8M positions, every one recoverable from about four bytes a ply, repeatable, checked by takref: docs/DATA.md, E34, E38. 150,000 to 400,000 games of each other size from Gen-1Big. Openings of up to 16 random plies and margin-checked deviations from E37 on. At equal time, 20,000 nodes a move taught a 6x6 network far more than 5000, E38. The network is fitted on 6x6's gens 7-8 and gen 9's 20,000-node games: gens 1-6 can go and gen 7 cannot, E39 |
| Fitting a network | On the GPU | An RTX 3070 fits about 18M positions a second to the CPU's 1.05M, the same network to within float rounding and repeatable to the byte; nothing is needed to build it: E37. Every size in one fit of minutes, the weights averaged over its last tenth. Two fits of the same games that differ only in seed play 15 to 35 apart, so a fit is chosen from several: E39 |
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
   where Yapper reached 13, and most losses to it are on flats by a median of 2: E34. Giving the
   network each side's stones left did not help, E35. E36's search gained on its own but not
   measurably on syntaks, and lost more often to roads by spread. A handicapped syntaks, scoring
   near half, is needed to measure progress against it.
2. The next generation, at 20,000 nodes, from the one network, on every size. The other sizes have
   only games from the hand-written evaluation, and gained most from sharing 6x6's; 6x6's first
   generation from a network gave +355, E33. On 6x6, 64,000 games at 20,000 nodes taught more than
   300,000 at 5000 in the same time, E38, and gens 7-9 hold little more for this network, E39.
3. Training. Two fits of the same games differ by 15 to 35 in play, as much as most changes, so
   fewer of them would make every comparison cheaper: E39's lower learning rate fitted marginally
   better and has not played. The scale that turns a score into a result: the games say 240 where
   the fit uses 310, E37. A wider network, which the speed lead over syntaks leaves room for, and
   which the shared filters, fitted on every size at once, may need more than one size's did.
4. The search items left: history in the reductions, a two-ply continuation history, null move
   from deeper with a reduction that grows with the margin, the aspiration window and the clock's
   floor. E36 found that sparing moves that threaten a road, and the improving flag outside
   move-count pruning, both lose.
5. Run on real ARM and measure it, since that is a stated target.
6. Make and unmake have plateaued at about 3 ns a node on the start position and 8.5 ns on the
   endgame: E7 lists what no longer helps. Any further lead has to come from an idea that is not
   on that list. A pure speedup must leave the bench node count unchanged.
