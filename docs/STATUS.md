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

E79 dropped 6x6 gens 7 to 9 from the fit, keeping gens 10 to 14: held-out loss fell 0.009 to 0.012 on
every 6x6 group, and it passed against E77, **+35.1 +/- 19.7** over 666 games at 8+0.08. It was the
network until E86.

E80 and E81 kept nothing and measured where the gap to syntaks is. Both to a fixed depth, Yapper
leads by 552 at depth 6 and 162 at 8, on roads it sees sooner, and trails by 24 at 10 and **93 at
12**, where syntaks searches a third of the nodes; at equal time syntaks is a ply to a ply and a
half deeper. Fifteen changes to what the search reduces or returns at, in either direction, each
read level or behind at 8+0.08: the reductions are where the tuner left them. The reason is in the
evaluation. On a game's own positions it is level with a deep search; on the positions the search
asks it about, drawn from the tree itself, the search reads **149 higher for the side to move with
a spread of 230**, at every depth into the tree. No shape learns that: the network fitted with
those positions, warm or from nothing, the feature map at 16 and 32 filters, both together and the
engine's shape from 16 to 128 templates all read them no better, and none beats the engine's shape
on games; 32 templates tie 64. Knowing every node's best move beforehand saves a sixth of a
position's nodes at the median and nothing in total, so the order has little left to give. The
target smoothed over 8 plies is level with E68's 4 and over 16 loses 28. A pilot of games started
a move off a game's line fitted 0.001 to 0.002 better on every 6x6 group and read +7.5 +/- 15.9
when it was stopped.

E82 took that swing up from inside the search. Within one search the evaluation's errors at two
nodes with the same side to move correlate at 0.20, and at 0.34 when the same move led to both, so
the search now keeps what its finished nodes found beyond the evaluation, by the move that led to
the node and the mover's own move before it, and the pruning reads the evaluation with that added:
the correction history of chess engines, keyed by what the measurement found. It passed against
E79 at 8+0.08, **+42.4 +/- 21.8** over 486 games, and is the engine now, bench 888446. A slot for
the side alone, read, runs away between the two sides and loses 190; E74 had counted the idea and
not built it. Against syntaks at full strength it reads **-220 [-243, -198]** over 1000 games,
where E77's build read -222: the gain is against Yapper's own evaluation. A table entry's score in
place of the evaluation, E83, read -4.3 +/- 18.6 and was stopped.

E85 asked whether more data still pays before a run of a billion positions. From nothing, each
doubling of the 6x6 games of gens 10 to 14 is worth 0.007 to 0.010 of held-out loss against
results, twice the epochs 0.0004, and 128 templates overfit 50M positions while gaining as much
from a doubling as 64: the plateau E61 and E81 found is the data's. In play, E79 against its own
recipe on half of gens 13 and 14 passed, **+27.0 +/- 16.8** over 902 games. E84, SPSA on E82 with
the correction's numbers and five refinements that start at nothing, read +12.0 +/- 16.2 over 842
games and was stopped: tuning waits until the gap to syntaks has closed.

Gen 15, a billion positions by E82, is being played on four rented machines. E86 fitted the first
220M of them into E79's recipe, on a trainer that now keeps the games rather than the positions, so
a fit is bounded by its games at 4 bytes a ply and not by memory; every 6x6 group held out fell by
0.007 to 0.012, as much as E79's step, and it played **+104.3 +/- 33.1** against E82 over 302
games at 8+0.08. The same games fitted from nothing, over 8 epochs where a warm fit takes 2, did
better again on every 6x6 group and played **+66.3 +/- 27.2** against E86 over 382 games, E87: with
this many games the start every fit since E49 kept had become a weight. E87 fitted 7x7 and 8x8
a little worse. Against syntaks at full strength, on E82's 1000 openings, E87 reads
**-136 [-156, -118]** where E82 read -220 [-243, -198]: 84 nearer. Gen 15 was stopped at 147
rounds (about 298M scored positions) and gen 16 was played in its place by E87 until 2026-11-17,
when the user stopped it at 321 rounds to work on the prior and the search: 10,025,888 games and
655.9M scored positions on four rented servers, which are deleted. E89 refitted from nothing on
gen 16's first 24 rounds and on gen 16 of the other sizes,
186,000 games played overnight on this machine. It played **+18.0 +/- 13.0** against E87 on 6x6,
and passed the first games on other sizes since E39: **+74.1 +/- 29.8** on 7x7 and
**+14.4 +/- 10.9** on 5x5. Twice the epochs leaned +10.3 and did not pass.

Eighteen hours on the training process then tried the rest of the recipe, E91 to E98, each fit
against a control on the same games at 100,000 nodes a move. Most of it is where it should be:
128 filters, a wider head, a longer mean of the weights, the older 6x6 generations back in, twice
the learning rate and 24 epochs each gained nothing or lost, the end of the game weighted up in
the loss lost 36, `lambda` by phase leaned +7 and did not pass, and twice the epochs is worth
about 7 on 6x6. Two fits of one recipe play 10 to 20 apart, and a recipe's lead of 20 over another
turned into a loss of 12 when both were fitted again, so one pair of fits says little under 25.
What the hours found is in the filters every size shares, which follow the games: as gen 16
grows, a plain refit takes them for 6x6, and on the newest list it lost 22 to E89 on 7x7. With
7x7 and 8x8 sitting out the first half of a fit of 16 epochs (`--join`), they gain instead: that
fit, E92, on every gen 16 game home at 05:27 on 2026-11-17, 711M positions, played
**+66.5 +/- 27.7** against E89 on 6x6 at 8+0.08 over 328 games, a test that stopped early and
reads high (+36.6 +/- 20.2 at 100,000 nodes a move), **+100.4 +/- 57.2** on 7x7, +7.2 +/- 21.0 on
5x5, and **+58.8 +/- 26.2** on 8x8 at 100,000 nodes a move. The one change to the recipe itself
that held up came last: decay on the filters, which fades the rows few positions reach, held
every size out better on two pairs of fits and played **+29.4 +/- 17.7** against E92 at 100,000
nodes a move, E97. E97 is the network now: E92's recipe and games with decay 0.01.
Against E89 it played **+79.5 +/- 30.6** on 6x6 at 8+0.08 over 298 games, which reads high as
E92's did. Until syntaks is beaten, networks are judged on 6x6 alone and the other sizes wait,
and no match against syntaks is played until the gains in self-play since E87, the last build
measured against it, pass 100 (the user, 2026-11-17): E89's 18, E92's 37 and E97's 29 make 84 by
the count of nodes, and E89's 18 with E97's 79.5 on the clock make 98.

E100 to E103 took up the prior and the search on 2026-11-17, and kept a speedup and a tune. The
prior is neither stale nor short of games: fitted on gens 10 and 11 it reads gen 16 at 2.8731 and a
refit there at 2.8584, whose loss on what it fitted is its held-out loss. It takes 18.5% of the
search, nearly all of it scoring the 86 moves of a list, fifteen evaluations' worth. A light table
fitted for depths 1 and 2, where move-count pruning searches 4 to 6 moves of 86, names the search's
move twice as often as the order by hand and played **-6.1 +/- 22.4** over 400 games at 8+0.08. The
prior from depth 1 read +47.2 +/- 30.4 at equal nodes and **-15.5 +/- 26.2** on the clock, and from
depth 5 **-28.7 +/- 25.6**: it is worth having at every depth, and what a move costs to score keeps
it at depth 3, E100. A second ply of singular extension read level, and reduced moves let down to
the leaf search lost 22.6 +/- 22.8, E101. E103 took a fifth off what a move costs the prior to
score, every logit unchanged: the same tree **4.8% faster** on real positions and 5.3% on bench,
measured beside a tune. E102, SPSA over all 55 tunables with the clock's, 10,000 pairs at 8+0.08
overnight, passed **+22.3 +/- 11.2** against the values it started from: the leaf search a ply
shorter for the third tune running, lighter reductions, reverse futility two plies deeper and more
time a move, bench 1009458. With it the gains in self-play since E87 come to some 120 on the clock,
past the 100 a match against syntaks waited for. Against syntaks at full strength on E87's 1000
openings it reads **-54 [-72, -36]** to E87's -136: **83 [61, 104] nearer**, paired opening by
opening, and the first build within 100 of syntaks. At 60+1 the same build reads **-223 +/- 27**
over 500 games, 173 [129, 223] worse on the same openings: the games turn by move 35 while the
clocks are level, and syntaks's depth grows faster with time, 5.2 plies for eight times the time to
Yapper's 4.2. The tune is not the cause: it beat its start values by +34.2 +/- 18.6 at 30+0.3, and
against syntaks at 60+1 the start values read -245 +/- 28, E102 22 [-14, 55] ahead, E107. E104
fitted a prior of 16 filters on the value network's tokens, weighed by its pools: 2.690 on gen 16's
held-out games to policy.c's 2.869, and 2.582 mixed with it. Five screens at equal nodes came to
+7.5 +/- 11.1 over 2,000 games and its lists cost a fifth more; not kept. Narrower shapes of it cost
no less than policy.c: a move's rows come from a table too big to stay in cache, and the cheapest
shape as accurate as policy.c costs 0.8 to 0.9 of it a move and more a node, E106. Screened beside
the tune at fixed nodes, a ply off a table move that is not singular read +22.6 +/- 18.5 at 200,000
nodes and +7.4 +/- 17.3 at 400,000; on the clock, on E102's values, it read +5.4 +/- 10.3 over 2,000
games, inconclusive, and is not kept, E105. E97's recipe on all of gen 16, a fifth more to fit, read
+6.9 +/- 10.6 over 2,000 games, inconclusive and about what E85's rate for data predicts; not kept,
E108. Counted to depth 20, a ply costs 1.78 times the last and no part of the tree comes apart, but
the subtrees under the third and later replies to a road, never reduced, climb from 24% of it at
depth 10 to 46% at 20, E108. Searching them a ply shallower from the third reply on saves 15% of
the nodes to depth 19 and read **+15.1 +/- 11.4** at 30+0.3, passing, E109, bench 1025277. ProbCut,
dead twice at 8+0.08, from depth 7 cuts 31% of the nodes to depth 15 and read **+34.4 +/- 19.1** at
30+0.3, passing, E112, bench 1156007. The prior's probability adds little to a move's place in
the list, and moves it rules out cost a node each, so cutting by it was not played, E113.
Searching again a ply short after a reduced move only just beat alpha saved 9% of the nodes and
read -7.6 +/- 12.8, failing, E114. ProbCut from depth 6 saved 15% more to depth 15 and read
-2.7 +/- 10.0, failing, E116.

## Done and verified

| Piece | State | Evidence |
|---|---|---|
| Rules, sizes 3-8 | Done | Published counts on 5x5 and 6x6, takref on all sizes, one selftest position per rule |
| Board, make/unmake | Done | 576M invariant checks; clean under a UB trap build |
| Move generation | Done, and leading | 9.9 to 12.6 times the fastest engine on bulk, and 6.0 to 7.5 times on full where only Topaz and Tiltak can be compared, on every position raced: EXPERIMENTS.md E8 and E13 |
| TEI | Done | 40 games through the real racetrack with no protocol faults |
| Match runner and SPRT | Done | Known-answer tests, EXPERIMENTS.md E1-E3 |
| Evaluation | One network for every size, a view for each side, a head of two layers | E92's recipe with decay 0.01 on the filters: **+79.5 +/- 30.6** over E89 at 8+0.08 on 6x6, and **+29.4 +/- 17.7** over E92 at 100,000 nodes, E97. Gen 16 to 05:27 on 2026-11-17 over 16 epochs, 7x7 and 8x8 joining for the last eight: **+66.5 +/- 27.7** over E89 at 8+0.08 on 6x6 (+36.6 +/- 20.2 at 100,000 nodes), **+100.4 +/- 57.2** on 7x7, +7.2 +/- 21.0 on 5x5, and **+58.8 +/- 26.2** on 8x8 at 100,000 nodes, E92. Refitted from nothing with gen 16's first 24 rounds and the other sizes' gen 16: **+18.0 +/- 13.0** over E87 at 8+0.08, **+74.1 +/- 29.8** on 7x7 and **+14.4 +/- 10.9** on 5x5, E89. The same games from nothing over 8 epochs: **+66.3 +/- 27.2** over E86 at 8+0.08, E87. Gen 15's first 220M positions added to E79's recipe: **+104.3 +/- 33.1** over E82 at 8+0.08, E86. 6x6 from gen 10 on, without gens 7 to 9: **+35.1 +/- 19.7** over E77, E79. Gen 14 listed twice in E70's recipe: **+76.4 +/- 30.1** over E70, E71. With the target's score smoothed over the next four searches: **+28.3 +/- 17.4** over E69, E70. E49's recipe with gen 13 added: **+35.3 +/- 19.7** at 2+0.02, E69. E48 fine-tuned on gen 11: **+23.4 +/- 15.4** at 2+0.02, +150 at k=8 and +103 against Topaz, E49. A head of 16 and 32 units on the pooled filters: **+54.0 +/- 24.7** at 2+0.02, +86.0 at equal nodes, E48. Two views of 64 filters, for one of 128 that the other side subtracted from: **+87.7 +/- 31.9** in self-play, +83 at k=8 and +17 against Topaz, E44, after E43 found one view could weigh only the difference between the sides. Before that, 128 filters, from 32 in E39: +79.4 +/- 21.8 for 64 at 100,000 nodes, and 128 led 64 by 45 against syntaks at k=8, E40. Filters on an 8x8 grid every board shares, and each size's own bias, output and skip. Against E38's networks of each size's own at 100,000 nodes: **8x8 +130, 7x7 +203, 5x5 +64**, passed SPRTs for its lambda 0.5 forerunner, then **+77 and +70 more on 8x8 and 7x7** from lambda 0.7; **4x4 +14.3 +/- 8.8**, passed, and +7.3 +/- 8.2 at 2+0.02; **6x6 +16.9 +/- 13.8 against v2, and +23.7 +/- 13.9 at 2+0.02**, both at the cap with their intervals above zero; 3x3 is decided by who moves first at every budget tried: E39. The hand-written evaluation went in E38. Refitted with gen 10, E42: +11.8 +/- 13.5 on E40 in self-play. A second layer fitted better and lost 37 on speed, playing only +14.5 at equal nodes, and the same as F1 against both other engines |
| Search | Second pass done | **+116 [+92, +140] against Tiltak over 800 games at 5+0.05**, from -38: EXPERIMENTS.md E25. It sees roads by spread, and most of the gain is speed in real middlegames. With the network, stronger reductions, move-count pruning to depth 6 and singular extensions: **+82.5 +/- 31.3 on v2's search at 2+0.02**, E36. History by the whole move rather than square and kind: **+121.7 +/- 26.5** at 100,000 nodes, E40. Move-count pruning at half: **+19.7 +/- 12.9** at the cap, and a quarter or deeper gained nothing more, E42. Only the moves that can stop a road answer it, and road-making placements, the other side's road squares and spreads by the tops they win go first: **+29.3 +/- 17.6**, E50. A prior over moves fitted to the search's own choices orders from depth 2: **+42.5 +/- 22.0** and then **+20.1 +/- 14.0**, E51. A network prior with history halved at each search: **+28.1 +/- 17.3** over E51, E52. Reductions `/ 1.6` for `/ 2.0`: **+21.7 +/- 10.8** over two runs, the second passing, E53. SPSA over every tunable but the clock's: **+51.0 +/- 17.6** at 2+0.02, E66, and again at 8+0.08 on E71's network, **+44.4 +/- 16.3** at 8+0.08, E72. A cache of evaluations, since a quarter are asked twice: the same nodes **7.6% to 9.6% faster**, E74. The head by pairs of inputs: 5.7% to 6.5% more, E77. The evaluation's error learned in the search by the move that led to a node, and read by the pruning: **+42.4 +/- 21.8** over E79 at 8+0.08, E82. The prior's moves a fifth cheaper to score: the same tree **4.8% faster** on real positions and 5.3% on bench, measured beside a tune, E103. SPSA over all 55 tunables, the clock's with them, at 8+0.08 on E97's network: **+22.3 +/- 11.2**, E102. Replies to a road from the third on a ply shallower: **+15.1 +/- 11.4** at 30+0.3, E109. ProbCut from depth 7: **+34.4 +/- 19.1** at 30+0.3, E112 |
| Time management | Done for now | Budgets on the moves left, from the shorter reserve. One time loss in 5242 games, and it does not use the whole clock yet: E25. More time in the opening, as syntaks spends it, lost 22; less lost 9, and a later stop lost 7: E42. An iteration started later while the best move is new and sooner once it has held: **+23.2 +/- 15.2** at 8+0.08 on no more clock, E76. Its twelve numbers tuned with the rest of the search, more time a move, inside E102's +22.3 |
| Training data | Every size | Gens 15 and 16, 6x6 at 20,000 nodes on four rented servers from Yapper's own games, people's games and the positions its search misjudged: 5,292,000 games and 298M scored positions by E82, then 10,025,888 games and 655.9M scored positions by E87, of which E97's list holds all of gen 15's and 465.7M of gen 16's. Gen 16 of 3x3 to 8x8, 186,000 games at 20,000 nodes by E87, 14.5M scored positions, E89. Gen 14, 6x6 from E70 at 20,000 nodes over a day, seeded by people's games, by Yapper's own and by the positions its own search misjudged: 684,720 games, 68.3M positions, E71. Gen 13, 6x6 from E66 at 20,000 and 60,000 nodes, seeded by random plies, by Yapper's own match games and by people's PlayTak games: 130,395 games, 12.7M positions, E69. Gen 11, two rounds over 5x5 to 8x8 at 20,000 nodes from E48: 202,000 games, 21.5M positions, E49. 6x6: gens 1-10, 228.8M positions, gen 10 being 120,000 games at 20,000 nodes from E40, E42; every one recoverable from about four bytes a ply, repeatable, checked by takref: docs/DATA.md, E34, E38. 150,000 to 400,000 games of each other size from Gen-1Big. Openings of up to 16 random plies and margin-checked deviations from E37 on. At equal time, 20,000 nodes a move taught a 6x6 network far more than 5000, E38. The network is fitted on 6x6's gens 7-8 and gen 9's 20,000-node games: gens 1-6 can go and gen 7 cannot, E39 |
| Fitting a network | On the GPU only | An RTX 3070 fits about 18M positions a second to the CPU's 1.05M, repeatable to the byte; nothing is needed to build it: E37. The CPU fit is gone, and the trainer keeps the games rather than the positions, making positions a block at a time, so a fit is bounded by its games at 4 bytes a ply, not by memory at 80 bytes a position; `--from net.h` carries on any network from any build: E86. Every size in one fit of minutes, the weights averaged over its last tenth. Two fits of the same games that differ only in seed play 15 to 35 apart, so a fit is chosen from several: E39. On 610M positions two seeds played 10 apart, and one recipe's lead of 20 over another became a loss of 12 when both were fitted again; a size with few games joins a long fit late, `--join`: E92. Decay on the filters, `--decay`, is the one change to the recipe that held on two pairs of fits: E97 |
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
   E82 scores 0.220 over 1000 games, -220 [-243, -198]: its 42 in self-play do not show there.
   E87 scores 0.314 on the same 1000 openings, -136 [-156, -118]: the networks of E86 and E87,
   +170 in self-play, show there at half their size.
   E102 scores 0.424 on the same 1000 openings, -54 [-72, -36]: the 120 in self-play since E87
   show there at about two thirds of their size, +83 [+61, +104] paired game by game. At 60+1 it
   scores 0.217, -223 +/- 27, on 250 of the openings: lost on the flat count, by move 35. The
   values E102's tune started from do no better there, 0.196, -245 +/- 28, E107.
   The depths above are E34's and E42's. E80 measured E79: syntaks is 1.5 plies deeper at equal
   time early in a game and 1 later, its ply costs 1.7 times the last to Yapper's 1.85, and at
   depth 12 each it leads by 93 on a third of the nodes. About 90 of the gap is judgement at equal
   depth and the rest the plies. Fifteen ways of reducing or returning sooner did not find them,
   E80, and neither a new shape for the network nor a better order holds them, E81.
   E42 still finds syntaks two to three plies deeper at less time, its tree growing 1.5 times a ply
   to Yapper's 1.8 to 2.4. syntaks also spends three times Yapper's time on each opening move. Its
   10 ms `MoveOverhead` takes more than the clock says at an eighth. Both sticks move with the
   machine's load, by 72 and 94 for one binary, so a gauntlet has to run alone. From E82 on it is
   one build over 1000 games, two seeds pooled, with no rerun of the old build beside it: the
   games go to the number that matters.
2. More generations, at 20,000 nodes, from the one network, on every size. Gen 11's two rounds,
   weighted four times in a fine-tune, gave +23.4 on 6x6, where the fit moved least; 7x7, 8x8
   and 5x5 moved more and have not been played, E49. Roads end fewer of its games each
   generation, 8% on 6x6 and 11% on 8x8 where gen 1 had 30%, so keep the older games in a fit.
   6x6's first generation from a network gave +355, E33. On 6x6, 64,000 games at 20,000 nodes taught
   more than 300,000 at 5000 in the same time, E38. At 128 filters a 20,000-node game takes 1.6
   times what it did at 32: E42.
   Gen 15 is a billion positions by E82 on rented machines, since E85 found every doubling of the
   games still pays, 27 in play for the second half of gens 13 and 14, and a wider network short
   of data rather than of use. Its first 220M lowered every 6x6 group held out by 0.007 to 0.012,
   E86; the trainer now holds a fit's games, not its positions, so all of it can be fitted at once.
   Gen 16 is a billion more by E87. Its first 24 rounds, with 186,000 games of the other sizes, gave
   E89: +18.0 on 6x6, +74.1 on 7x7 and +14.4 on 5x5.
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
   at the leaves. It counted a correction of the evaluation kept by a key of the position as worth
   too little to build, which was wrong: built with the move as its key, it gained 42, E82.
   E75 found the search's own shallow result no substitute for the null move either at 8+0.08,
   which the clock got wrong: from depth 7 at 30+0.3 it gained 34, E112. It found nothing
   to gain from history or parity in the reductions by counting: 84% of reduced moves have no
   history or have it against them, and a reduced move beats alpha in full once in a hundred times
   at best.
   E101 found a second ply of singular extension level and reduced moves let down to the leaf
   search worth -23. E105 screened the rest at fixed nodes: a ply off a table move that is not
   singular gives less the deeper the search, +22.6 at 200,000 nodes, +7.4 at 400,000 and +5.4
   +/- 10.3 at 8+0.08 over 2,000 games, inconclusive; a ply off a node expected to cut with no
   table move, or the first at cut nodes only, read level.
7. The order, E61 and E62. The prior names the 20,000-node search's move 21.8% of the time, the
   value network one ply down 25.4%, a depth 1 search 34.0%, and a prior fitted on the GPU with a
   trunk of its own 28.3%, at seven times today's cost. None of that is the order inside the
   tree: ordering by a depth 1 search from depth 6 cost a sixth more nodes to a depth on real
   positions and lost 25 to 32 in three forms, E62, as E52 found a better-predicting prior ordered
   little better. History already holds what refutes in the tree; a prior is worth pursuing for
   its cost, not its top 1. The feature map as an evaluation does not pay either, E61. No other
   engine's play goes into the data, so E60's gen 12 stays out of every fit: DATA.md.
   E100 found the prior no better for gen 16, a fitted order for depths 1 and 2 level with the
   order by hand, and the prior from depth 1 worth +47 at equal nodes and -15 on the clock: its
   cost is the lever. E103 took a fifth off a move; spreads are three moves in four and no one
   piece of theirs stands out. Next there: the prior from depth 2 on the cheaper move.
   E104's prior of filters on the value network's tokens named the move 0.18 nats better than
   policy.c, 0.29 mixed with it, and played +7.5 +/- 11.1 at equal nodes: four priors from 14% to
   27% top 1 have now played alike. Made narrow it costs no less than policy.c: a move's 1.5
   rows from a table too big for the cache, and working out what it leaves, are the floor, E106.
5. Run on real ARM and measure it, since that is a stated target.
6. Make and unmake have plateaued at about 3 ns a node on the start position and 8.5 ns on the
   endgame: E7 lists what no longer helps. Any further lead has to come from an idea that is not
   on that list. A pure speedup must leave the bench node count unchanged.
