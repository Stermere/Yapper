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
untied and wider it gained half as much, E58.

E61 measured where the search's time goes and where the evaluation and the prior fall short. The
two networks take 57% of the search, the value network 39% and the prior 17%, and a list scored by
the prior costs as much as 11 evaluations. On every 6x6 game, 150.7M positions fitted on the GPU,
E58's lead was gone: the 8-filter map trails the engine's network by 0.0011 against results, a
16-filter map ties it, and the two side by side gain nothing, so no shape within reach makes the
evaluation much better on these games. The prior is the weak part: it names the 20,000-node
search's move 21.8% of the time, where the value network's best child one ply down names 25.4% and
a depth 1 search 34.0%, and twice the games move it 0.1. A prior read off a small feature map's
trunk named 23.4% to its 21.1%. E62 fitted a prior on the GPU to 28.3%, short of a depth 1 search's
32.4% on quiet positions and seven times today's cost, and ordering near the root by a depth 1
search lost 25 to 32 in three forms: it cost a sixth more nodes on real positions and ordered no
better than history and the prior. See Next.

E65 reviewed the code for what it could do without. The network has nothing to spare: the straight
sum of the pools carries twice the head's part of the score. Of four smaller pieces, none was
harmful when taken out, and two went: the history of what answered what, and the prior's position
features, square classes and lines, with a prior refitted without them. Together they passed a
non-regression test against the engine before them, **+22.3 +/- 30.5** over 218 games. The road
threat's price and the hand order's flat and capstone terms stay; their gain tests leaned the
other way.

E67 to E69 worked on the data, with no other engine's play in it. Fitting the loss to self-play's
own results, a scale for each size, lost 10, E67. Reading the target's score as the mean of the
next four searches' passed, **+16.3 +/- 9.2** over two runs, E68. Gen 13, 130,395 games of 6x6 by
E66 from random openings, from positions of Yapper's own match games and from positions of PlayTak
games between people, was far from what the network knew, and E49's recipe with it added passed
**+35.3 +/- 19.7**, E69. For the same time, three times the games at 20,000 nodes beat a third as
many at 60,000, and finishing on the deeper games alone lost. The smoothed target on gen 13's fit
passed again, **+28.3 +/- 17.4** over E69: the two gains add, E70. Against the other engines, beside
E66 run straight after, E70 reads **+55 [+28, +81]** against syntaks at a quarter, E66's +56,
**+172 [+121, +221]** against Topaz, E66's +151, and **-290 [-345, -233]** against syntaks at full
strength at 8+0.08, E66's -339: the day's 60 in self-play shows as up to 50 there, inside the noise.
Gen 14, a day of E70's games, 684,720 of 6x6 from people's games, Yapper's own and the positions
its own search misjudged, listed twice in that recipe passed **+76.4 +/- 30.1** over E70, E71.
SPSA over the search's 38 tunables at 8+0.08 on that network, where E66's first run had tuned at
2+0.02, moved 29 of them and passed **+44.4 +/- 16.3** at 8+0.08, E72. Against syntaks at full
strength at 8+0.08, the three back to back on the same 500 openings read E70 -308, E71 -277 and
**E72 -260 [-294, -225]**, about two fifths of each step's gain in self-play. The clock's seven
tunables tuned alone, E73, read +8.8 +/- 19.8 after 592 games and were stopped; they wait for one
tune of every parameter nearer syntaks's strength.

E74 measured where a search's nodes go before anything was built. Four nodes in five are evaluated
and return at once, and a quarter of those evaluations are of a position already scored that game,
since reverse futility and the leaves write nothing to the table. A cache of them searches the same
tree, node for node, **9.6% faster** alone and **7.6% faster** with seven engines at once. The same
searches said what not to build: a leaf's score cannot be guessed from its parent's, a table probe
at the leaves finds an entry 2.4% of the time, the evaluation's error does not follow any key of
the position by much, and a pass costs 450 on the evaluation's scale, which is why the null move
holds at a tenth of the nodes it is tried at.

E75 took up the null move's job. The node itself, searched four plies shallower against a bound
100 past beta, is right 98% to 99% of the time from depth 5 and reaches depth 12 on real positions
in 30% less time, yet it read **-0.6 +/- 15.1** over 1218 games at 8+0.08 and was stopped: the
singular search already cuts half of the same nodes, and the depth it saves was doing the work.
The same counting found that a position below alpha recovers too often for any margin or shallow
search to prune it, that a cache of the prior's lists would be hit one time in eight, and that
reductions by an odd number of plies are lopsided, since scores swing 40 to 80 between odd and
even depths, though moving their bound found nothing more.

E76 gave the clock a signal from the search. Another iteration names another move a third of the
time when the move has just changed and one time in seventeen once it has held for seven, so an
iteration is now started up to 2.3 times as late just after a change and 0.57 times once the move
has held for four. The shares were chosen by replaying rules over 5,886 logged searches of 1.8M
nodes, where this one ended on the long search's move 2.4 points more often for the same nodes,
what half as much time again buys. It passed at 8+0.08, **+23.2 +/- 15.2** over 1066 games, on
slightly less clock than before.

E77 laid the head's two small layers out by pairs of inputs, so each is a run of multiply-adds with
no sum across a register: the same scores, **5.7% faster** alone and 6.5% with seven engines at
once. The cache, the clock and the head together passed against E72 at 8+0.08, **+94.0 +/- 31.8**
over 284 games, a test that stopped early and reads high. Against syntaks at full strength at
8+0.08 that build reads **-222 [-256, -191]**, where E72 run straight after on the same 500
openings reads -250 [-287, -217]: +28 +/- 35 game by game.

Gen 14 listed four times in E71's recipe fitted better on every 6x6 group and played **+3.0 +/-
11.1** against E77 over 2000 games, E78, so the network stays E71's. The shallow-search cut of E75
from depth 7 only read +5.7 +/- 11.2 over 2000.

## Done and verified

| Piece | State | Evidence |
|---|---|---|
| Rules, sizes 3-8 | Done | Published counts on 5x5 and 6x6, takref on all sizes, one selftest position per rule |
| Board, make/unmake | Done | 576M invariant checks; clean under a UB trap build |
| Move generation | Done, and leading | 9.9 to 12.6 times the fastest engine on bulk, and 6.0 to 7.5 times on full where only Topaz and Tiltak can be compared, on every position raced: EXPERIMENTS.md E8 and E13 |
| TEI | Done | 40 games through the real racetrack with no protocol faults |
| Match runner and SPRT | Done | Known-answer tests, EXPERIMENTS.md E1-E3 |
| Evaluation | One network for every size, a view for each side, a head of two layers | Gen 14 listed twice in E70's recipe: **+76.4 +/- 30.1** over E70, E71. With the target's score smoothed over the next four searches: **+28.3 +/- 17.4** over E69, E70. E49's recipe with gen 13 added: **+35.3 +/- 19.7** at 2+0.02, E69. E48 fine-tuned on gen 11: **+23.4 +/- 15.4** at 2+0.02, +150 at k=8 and +103 against Topaz, E49. A head of 16 and 32 units on the pooled filters: **+54.0 +/- 24.7** at 2+0.02, +86.0 at equal nodes, E48. Two views of 64 filters, for one of 128 that the other side subtracted from: **+87.7 +/- 31.9** in self-play, +83 at k=8 and +17 against Topaz, E44, after E43 found one view could weigh only the difference between the sides. Before that, 128 filters, from 32 in E39: +79.4 +/- 21.8 for 64 at 100,000 nodes, and 128 led 64 by 45 against syntaks at k=8, E40. Filters on an 8x8 grid every board shares, and each size's own bias, output and skip. Against E38's networks of each size's own at 100,000 nodes: **8x8 +130, 7x7 +203, 5x5 +64**, passed SPRTs for its lambda 0.5 forerunner, then **+77 and +70 more on 8x8 and 7x7** from lambda 0.7; **4x4 +14.3 +/- 8.8**, passed, and +7.3 +/- 8.2 at 2+0.02; **6x6 +16.9 +/- 13.8 against v2, and +23.7 +/- 13.9 at 2+0.02**, both at the cap with their intervals above zero; 3x3 is decided by who moves first at every budget tried: E39. The hand-written evaluation went in E38. Refitted with gen 10, E42: +11.8 +/- 13.5 on E40 in self-play. A second layer fitted better and lost 37 on speed, playing only +14.5 at equal nodes, and the same as F1 against both other engines |
| Search | Second pass done | **+116 [+92, +140] against Tiltak over 800 games at 5+0.05**, from -38: EXPERIMENTS.md E25. It sees roads by spread, and most of the gain is speed in real middlegames. With the network, stronger reductions, move-count pruning to depth 6 and singular extensions: **+82.5 +/- 31.3 on v2's search at 2+0.02**, E36. History by the whole move rather than square and kind: **+121.7 +/- 26.5** at 100,000 nodes, E40. Move-count pruning at half: **+19.7 +/- 12.9** at the cap, and a quarter or deeper gained nothing more, E42. Only the moves that can stop a road answer it, and road-making placements, the other side's road squares and spreads by the tops they win go first: **+29.3 +/- 17.6**, E50. A prior over moves fitted to the search's own choices orders from depth 2: **+42.5 +/- 22.0** and then **+20.1 +/- 14.0**, E51. A network prior with history halved at each search: **+28.1 +/- 17.3** over E51, E52. Reductions `/ 1.6` for `/ 2.0`: **+21.7 +/- 10.8** over two runs, the second passing, E53. SPSA over every tunable but the clock's: **+51.0 +/- 17.6** at 2+0.02, E66, and again at 8+0.08 on E71's network, **+44.4 +/- 16.3** at 8+0.08, E72. A cache of evaluations, since a quarter are asked twice: the same nodes **7.6% to 9.6% faster**, E74. The head by pairs of inputs: 5.7% to 6.5% more, E77 |
| Time management | Done for now | Budgets on the moves left, from the shorter reserve. One time loss in 5242 games, and it does not use the whole clock yet: E25. More time in the opening, as syntaks spends it, lost 22; less lost 9, and a later stop lost 7: E42. An iteration started later while the best move is new and sooner once it has held: **+23.2 +/- 15.2** at 8+0.08 on no more clock, E76 |
| Training data | Every size | Gen 14, 6x6 from E70 at 20,000 nodes over a day, seeded by people's games, by Yapper's own and by the positions its own search misjudged: 684,720 games, 68.3M positions, E71. Gen 13, 6x6 from E66 at 20,000 and 60,000 nodes, seeded by random plies, by Yapper's own match games and by people's PlayTak games: 130,395 games, 12.7M positions, E69. Gen 11, two rounds over 5x5 to 8x8 at 20,000 nodes from E48: 202,000 games, 21.5M positions, E49. 6x6: gens 1-10, 228.8M positions, gen 10 being 120,000 games at 20,000 nodes from E40, E42; every one recoverable from about four bytes a ply, repeatable, checked by takref: docs/DATA.md, E34, E38. 150,000 to 400,000 games of each other size from Gen-1Big. Openings of up to 16 random plies and margin-checked deviations from E37 on. At equal time, 20,000 nodes a move taught a 6x6 network far more than 5000, E38. The network is fitted on 6x6's gens 7-8 and gen 9's 20,000-node games: gens 1-6 can go and gen 7 cannot, E39 |
| Fitting a network | On the GPU | An RTX 3070 fits about 18M positions a second to the CPU's 1.05M, the same network to within float rounding and repeatable to the byte; nothing is needed to build it: E37. Every size in one fit of minutes, the weights averaged over its last tenth. Two fits of the same games that differ only in seed play 15 to 35 apart, so a fit is chosen from several: E39 |
| Playing it yourself | Done | `python tools/play.py` |
| Playing on PlayTak | Done | `python tools/playtakbot.py --user <account>`. One guest-against-guest test game on the live server, 2026-11-08; not yet from a bot account |
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
   E70 scores 0.579 there, and 0.158 against syntaks at full strength at 8+0.08. E72 scores 0.183
   at full strength, where E70 run beside it scored 0.145: self-play gains show there at about two
   fifths of their size, E72.
   E77 scores 0.218 at full strength, where E72 run beside it scored 0.192.
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
   better and has not played. The scale that turns a score into a result: the games say 120 to 280
   by size where the fit uses 310, yet a fit to that lost 10, E67. A fit's gain plays at about 10 Elo at equal nodes per 0.001 of held-out
   loss, so a network that costs speed must buy more than that: E42's second layer did not, nor did
   lambda 0.85 or more epochs, nor 256 filters, at two thirds of the speed.
4. The search items left: a two-ply continuation history, the aspiration window and the clock's
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
   the open question there. E74 measured why the null move does nothing here and a deeper one
   would not either: a pass costs 450, so it holds at 10% of its tries while 88% of the rest fail
   high anyway. It also ruled out, by counting, a stand pat guessed from the parent, a table probe
   at the leaves, and a correction of the evaluation kept by a key of the position.
   E75 found the search's own shallow result no substitute for the null move either, and nothing
   to gain from history or parity in the reductions by counting: 84% of reduced moves have no
   history or have it against them, and a reduced move beats alpha in full once in a hundred times
   at best.
7. The order, E61 and E62. The prior names the 20,000-node search's move 21.8% of the time, the
   value network one ply down 25.4%, a depth 1 search 34.0%, and a prior fitted on the GPU with a
   trunk of its own 28.3%, at seven times today's cost. None of that is the order inside the
   tree: ordering by a depth 1 search from depth 6 cost a sixth more nodes to a depth on real
   positions and lost 25 to 32 in three forms, E62, as E52 found a better-predicting prior ordered
   little better. History already holds what refutes in the tree; a prior is worth pursuing for
   its cost, not its top 1. The feature map as an evaluation does not pay either, E61. No other
   engine's play goes into the data, so E60's gen 12 stays out of every fit: DATA.md.
5. Run on real ARM and measure it, since that is a stated target.
6. Make and unmake have plateaued at about 3 ns a node on the start position and 8.5 ns on the
   endgame: E7 lists what no longer helps. Any further lead has to come from an idea that is not
   on that list. A pure speedup must leave the bench node count unchanged.
