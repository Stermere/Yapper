# Status

**Tiltak is beaten, and syntaks is the bar.** A network on 6x6 led Tiltak by +428 over 800 games,
E33. The aim set for 2026-11-02 was to come within 300 of syntaks that day; the best network, v2,
read -576 [-665, -481] over 400 games, E34. A more selective search gained +82.5 on v2's own, and
read -665 [-784, -541] against syntaks, which at this gap cannot tell the two apart: E36. One
network now plays every size, at least as well as the networks of their own that replaced the
hand-written evaluation, and far better on 5x5, 7x7 and 8x8: E38, E39. syntaks on an eighth of
its clock is the measure now: E39 read -54 [-82, -26] there, and E40, four times the network's
width and history kept by the whole move, **+60 [+32, +87]**, 600 games each. At full strength
E40 reads **-442 [-532, -348]** against syntaks and **-99 [-146, -52]** against a newer Topaz,
200 games each: E41. E42's refit on 120,000 new games at 20,000 nodes reads, on a quiet machine,
**+27 [+0, +55]** at an eighth, **-132 [-181, -82]** against Topaz and **-427 [-488, -362]**
against syntaks at full strength over 400 games. A GPU fit beside the same binary lifted it by 72
and 94 against the first two. E40, run alone, reads +39 [+11, +67] and -92 [-140, -43], so
neither the refit nor E42's tighter pruning is measurably better against another engine yet.
Together they passed against E40 in self-play, **+36.8 +/- 14.8**. E44 gave each side a view of
its own in the network, at the same width and cost: **+87.7 +/- 31.9** in self-play, **+83
[+55, +110]** against syntaks at an eighth and **+17 [-27, +61]** against Topaz, the first score
above half against it. E48 put a head of two small layers after the pooled filters:
**+54.0 +/- 24.7** in self-play at a time control despite a fifth less speed, +86.0 at equal
nodes, and **+104 [+76, +131]** against syntaks at an eighth and **+48 [+3, +93]** against
Topaz. Against syntaks at full strength it reads **-410 [-467, -349]**, E42's -427: what is left
there is depth. E49 fine-tuned E48 on gen 11, 202,000 games of 5x5 to 8x8 from E48 itself, and
passed against it, **+23.4 +/- 15.4**, in a fit of 4 minutes. It reads **+150 [+120, +179]**
against syntaks at an eighth, **-17 [-44, +9]** at a quarter, which is the measure from here on,
**+103 [+55, +150]** against Topaz and **-391 [-446, -334]** against syntaks at full strength. A fit of the same games from nothing lost to it by 28.7.
E50 took the search's ordering and its answers to a road from Tak's own structure: a road to
answer plays only the moves that can stop it, and placements that leave a road to finish or take
the other side's, with spreads by the tops they win, go first. **+29.3 +/- 17.6** in self-play.
Against E49 run beside it, **-33 [-60, -6]** to -40 against syntaks at a quarter and **+79
[+34, +123]** to +74 against Topaz: no ground lost, and E49 itself read 23 and 29 lower than in
its own session. E51 fitted a prior over moves to the moves the search chose in gens 10 and 11, a
weight for each of 262 Tak features of a move, and ordered by it from depth 2: **+42.5 +/- 22.0**
over E50 from depth 1, and **+20.1 +/- 14.0** more from depth 2. Against E50 run beside it,
**-36 [-62, -9]** to -52 against syntaks at a quarter and **+119 [+68, +168]** to +57 against
Topaz. E52 made the prior a small network over richer Tak features, read against the last two
moves: it fits far better, top-1 21.9% where E51 had 14.4%, and transfers to the other sizes, yet
alone it only paid for its cost, +0.3 and -0.7 at the cap, since history already holds much of
what it knows. Halving history at the start of each search, the user's idea, made it pay:
**+26.0 +/- 16.4** over the network prior alone, and **+28.1 +/- 17.3** over E51 at 1536 a nat.
E53 came from a review of the code by a subagent kept from this history. Of its nine suggestions,
stronger late move reductions passed: **+42.8 +/- 22.0** on a fresh run after +14.4 at the cap,
**+21.7 +/- 10.8** the two together. Against E52 run beside it, **+23 [-3, +49]** to +36 against
syntaks at a quarter and **+134 [+85, +182]** to +155 against Topaz: inside the noise, and leaning
the other way.

The night of 2026-11-06 tried five ideas from Tak's own structure side by side, E55 to E60. In
play, none passed: quiet spreads pruned by their own count lost everywhere and were neutral behind
a road-danger gate, E55; history by the prior's features lost 65 as first written and 24 with the
malus shared, E56; a prior fitted on the search's own cutoffs lost 8, E59; a generation of 50,000
games from the openings of the games against syntaks and Topaz, refitted warm, read **+12.0 +/-
12.5** at the cap, the shape of the data steps before it, E60. Two fits told more. Roads and
reach as inputs to the head were worth 0.0025 of held-out loss on 14.7M positions and nothing on
all 254M, so the filters learn connectivity by themselves given the data, E57. A local
feature-map network, each square's hidden vector from a 5x5 kernel tied under the board's eight
symmetries, with 8 base filters and 123,000 parameters, beat the engine's own architecture fitted
the same way on the same games by **0.0073 against results**, twice what two views gave in E44;
untied and wider it gained half as much, E58. That is the direction: see Next.

## Done and verified

| Piece | State | Evidence |
|---|---|---|
| Rules, sizes 3-8 | Done | Published counts on 5x5 and 6x6, takref on all sizes, one selftest position per rule |
| Board, make/unmake | Done | 576M invariant checks; clean under a UB trap build |
| Move generation | Done, and leading | 9.9 to 12.6 times the fastest engine on bulk, and 6.0 to 7.5 times on full where only Topaz and Tiltak can be compared, on every position raced: EXPERIMENTS.md E8 and E13 |
| TEI | Done | 40 games through the real racetrack with no protocol faults |
| Match runner and SPRT | Done | Known-answer tests, EXPERIMENTS.md E1-E3 |
| Evaluation | One network for every size, a view for each side, a head of two layers | E48 fine-tuned on gen 11: **+23.4 +/- 15.4** at 2+0.02, +150 at k=8 and +103 against Topaz, E49. A head of 16 and 32 units on the pooled filters: **+54.0 +/- 24.7** at 2+0.02, +86.0 at equal nodes, E48. Two views of 64 filters, for one of 128 that the other side subtracted from: **+87.7 +/- 31.9** in self-play, +83 at k=8 and +17 against Topaz, E44, after E43 found one view could weigh only the difference between the sides. Before that, 128 filters, from 32 in E39: +79.4 +/- 21.8 for 64 at 100,000 nodes, and 128 led 64 by 45 against syntaks at k=8, E40. Filters on an 8x8 grid every board shares, and each size's own bias, output and skip. Against E38's networks of each size's own at 100,000 nodes: **8x8 +130, 7x7 +203, 5x5 +64**, passed SPRTs for its lambda 0.5 forerunner, then **+77 and +70 more on 8x8 and 7x7** from lambda 0.7; **4x4 +14.3 +/- 8.8**, passed, and +7.3 +/- 8.2 at 2+0.02; **6x6 +16.9 +/- 13.8 against v2, and +23.7 +/- 13.9 at 2+0.02**, both at the cap with their intervals above zero; 3x3 is decided by who moves first at every budget tried: E39. The hand-written evaluation went in E38. Refitted with gen 10, E42: +11.8 +/- 13.5 on E40 in self-play. A second layer fitted better and lost 37 on speed, playing only +14.5 at equal nodes, and the same as F1 against both other engines |
| Search | Second pass done | **+116 [+92, +140] against Tiltak over 800 games at 5+0.05**, from -38: EXPERIMENTS.md E25. It sees roads by spread, and most of the gain is speed in real middlegames. With the network, stronger reductions, move-count pruning to depth 6 and singular extensions: **+82.5 +/- 31.3 on v2's search at 2+0.02**, E36. History by the whole move rather than square and kind: **+121.7 +/- 26.5** at 100,000 nodes, E40. Move-count pruning at half: **+19.7 +/- 12.9** at the cap, and a quarter or deeper gained nothing more, E42. Only the moves that can stop a road answer it, and road-making placements, the other side's road squares and spreads by the tops they win go first: **+29.3 +/- 17.6**, E50. A prior over moves fitted to the search's own choices orders from depth 2: **+42.5 +/- 22.0** and then **+20.1 +/- 14.0**, E51. A network prior with history halved at each search: **+28.1 +/- 17.3** over E51, E52. Reductions `/ 1.6` for `/ 2.0`: **+21.7 +/- 10.8** over two runs, the second passing, E53 |
| Time management | Done for now | Budgets on the moves left, from the shorter reserve. One time loss in 5242 games, and it does not use the whole clock yet: E25. More time in the opening, as syntaks spends it, lost 22; less lost 9, and a later stop lost 7: E42 |
| Training data | Every size | Gen 11, two rounds over 5x5 to 8x8 at 20,000 nodes from E48: 202,000 games, 21.5M positions, E49. 6x6: gens 1-10, 228.8M positions, gen 10 being 120,000 games at 20,000 nodes from E40, E42; every one recoverable from about four bytes a ply, repeatable, checked by takref: docs/DATA.md, E34, E38. 150,000 to 400,000 games of each other size from Gen-1Big. Openings of up to 16 random plies and margin-checked deviations from E37 on. At equal time, 20,000 nodes a move taught a 6x6 network far more than 5000, E38. The network is fitted on 6x6's gens 7-8 and gen 9's 20,000-node games: gens 1-6 can go and gen 7 cannot, E39 |
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
`--message-format=short`, which leaves source lines out of the errors. Topaz's engine needs its
network as `src/quantised.bin`, from the Google Drive folder its README links, and the network
has to be the one of its commit's day: `topaz-tei.exe` is dcb8cd2 on branch `back` with the
10_30_2026 network, since none of those published fits main's 2989527.

## Next

1. Close on syntaks. It searches about a quarter of Yapper's nodes a second yet reaches depth 18
   where Yapper reached 13, and most losses to it are on flats by a median of 2: E34. Giving the
   network each side's stones left did not help, E35. E36's search gained on its own but not
   measurably on syntaks, and lost more often to roads by spread. syntaks at 1/8 of 5+0.05 is the
   stick, and about 100 to 250 Elo a halving of its clock: E40 scores 0.586 there. Most road losses
   are still by spread, 56 of 59, but looking for the other side's spreads where the search trusts
   a quiet position lost, in reverse futility and more so in the leaf search, as did a follow-up
   history and IIR: E41. Topaz at full strength, which E40 scores 0.36 against, is a second stick.
   E49 scores 0.704 at an eighth and 0.475 at a quarter, 1.25+0.0125, which is the stick now.
   E42 still finds syntaks two to three plies deeper at less time, its tree growing 1.5 times a ply
   to Yapper's 1.8 to 2.4. syntaks also spends three times Yapper's time on each opening move. Its
   10 ms `MoveOverhead` takes more than the clock says at an eighth. Both sticks move with the
   machine's load, by 72 and 94 for one binary, so a gauntlet has to run alone and against
   another run alike.
2. More generations, at 20,000 nodes, from the one network, on every size. Gen 11's two rounds,
   weighted four times in a fine-tune, gave +23.4 on 6x6, where the fit moved least; 7x7, 8x8
   and 5x5 moved more and have not been played, E49. Roads end fewer of its games each
   generation, 8% on 6x6 and 11% on 8x8 where gen 1 had 30%, so keep the older games in a fit.
   6x6's first generation from a network gave +355, E33. On 6x6, 64,000 games at 20,000 nodes taught
   more than 300,000 at 5000 in the same time, E38. At 128 filters a 20,000-node game takes 1.6
   times what it did at 32: E42.
3. Training. Two fits of the same games differ by 15 to 35 in play, as much as most changes, so
   fewer of them would make every comparison cheaper: E39's lower learning rate fitted marginally
   better and has not played. The scale that turns a score into a result: the games say 240 where
   the fit uses 310, E37. A fit's gain plays at about 10 Elo at equal nodes per 0.001 of held-out
   loss, so a network that costs speed must buy more than that: E42's second layer did not, nor did
   lambda 0.85 or more epochs, nor 256 filters, at two thirds of the speed.
4. The search items left: history in the reductions, a two-ply continuation history, null move
   from deeper with a reduction that grows with the margin, the aspiration window and the clock's
   floor. E36 found that sparing moves that threaten a road, and the improving flag outside
   move-count pruning, both lose. E50 found that hand-set priors each gain too little to measure,
   and that a tinue prover, pruning spreads by the tops they leave, and leaves that answer roads by
   spread all lose. E51's fitted prior gained through the order, and asking it again where
   move-count pruning stops, to spare a move or to skip one, did not. E52's network prior was
   neutral alone and paid with history halved at each search; reductions by it lost, and so did
   keeping it from depth 3. Its cost is a list of 3.9 us where E51's took 1.9, so the order is a
   quarter of the search: making it cheaper is worth about the speed it takes. A fit on every
   size helps 7x7 and 8x8 offline and has not played. E55 found that quiet spreads, those that
   win no tops and lead to no road, are not where the width is: pruning them lost, and a prior
   fitted on cutoffs ordered worse than one fitted on root choices, E59. A per-feature history
   lost as first written because shared features drown in maluses, E56; the shared-malus form is
   the open question there.
7. The feature-map network, E58: the first architecture to beat the global templates on the
   same games, and the tied, smaller form beat them most, by 0.0073 against results with 16
   pools a view. What it needs next: the same comparison on all 254M positions with the full
   recipe, since E57's gain on the subset vanished there, and the right F; then an engine design
   that keeps a frame of 36 squares by 64 units by two views, updates the 25 squares a change
   touches, pools 4608 units, and reads the policy off the same accumulator so the prior costs
   nothing and can run at depth 1. The cost in play is about 1.6 times today's update and 4.5
   times the pooling, against a gain the fit puts near 70 at equal nodes. E60's openings from
   the opponents' games belong in the next generation's mix.
5. Run on real ARM and measure it, since that is a stated target.
6. Make and unmake have plateaued at about 3 ns a node on the start position and 8.5 ns on the
   endgame: E7 lists what no longer helps. Any further lead has to come from an idea that is not
   on that list. A pure speedup must leave the bench node count unchanged.
