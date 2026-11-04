# Training data

Self-play games are the one training input Yapper has. **A file holds games, never positions or
features.** Replaying a game gives back every position in it, so nothing has to be regenerated
when a network's inputs or its shape change, and the games that taught the hand-written
evaluation went on to teach every network after it.

## Making data

```sh
python tools/datagen.py --out external/data/gen1 --games 60000 --nodes 5000 --generation 1
python tools/datacheck.py external/data/gen1-*.ydg
```

`yapper datagen` plays the games inside the engine, one process a job, so there is no protocol,
referee or pipe in the loop, and a run uses every logical core. Each job writes its own file and
shares nothing. The budget is in nodes, so a busy machine changes the speed and not the games:
the same seed and build give the same bytes. `external/` is gitignored, so data is never
committed.

Each game starts from 4 to 16 random plies, chosen by kind so that capstones turn up at every
point of the opening. The longer openings leave structures on the board that the engine's own
play would never build. A game is thrown away if its first search already calls it decided, which
costs no search of its own: that search's move is the game's first.

On about one ply in ten, a random move is searched as well, and played in place of the search's
choice if it comes within 0.4 of a flat of it. That is about one such deviation in 110 plies, for
8% more search. It takes games off the lines one network keeps to, and the margin keeps it from
being a blunder that the game's result would then pin on the positions before it. Komi is
mostly 2, with enough of 0, 1, 1.5, 2.5 and 3 for a network to learn what it does. Games
are played to the end under the rules, with a threefold repetition or 512 plies as a draw, the
same convention matches use. There is no adjudication: a game costs little once it is decided,
and a stored result is then always the true one.

`datacheck` reads the files with no engine and replays a sample through `tools/takref.py`, so a
game's moves and result are confirmed by code that shares nothing with `src/`. Run it on anything
new. It is also the reference reader for this format.

## The format

Files are little-endian and are plain lists of games, so joining files joins datasets. A game is
a 16-byte header, then 4 bytes for each ply:

| Offset | Type | Field |
|---|---|---|
| 0 | u32 | magic, `0x31474459`, the bytes `YDG1` |
| 4 | u8 | board size |
| 5 | i8 | komi in half flats, in black's favour |
| 6 | u8 | outcome: 1 white road, 2 black road, 3 white flats, 4 black flats, 5 draw |
| 7 | u8 | ending: 0 by the rules, 1 threefold repetition, 2 ply limit |
| 8 | u16 | plies |
| 10 | u8 | random plies at the start, which carry no score |
| 11 | u8 | generation, the round of training that made this game |
| 12 | u32 | search budget per move, in nodes |

Then for each ply, starting from the empty board:

| Offset | Type | Field |
|---|---|---|
| 0 | u16 | the move, encoded as in `src/types.h` |
| 2 | i16 | the search's score, for the side to move, or `0x7FFF` for a move the search did not choose: a random ply or a deviation |

A score at or beyond 29872 in size is a forced win or loss in `30000 - |score|` plies. Any change
to any of this needs a new magic. A file that ends part way through a game came from a killed
run, and readers stop at the last whole game.

## Reading it

In C, `data_replay()` calls a function with every position of a file, its game and its ply. That
function is the seam: `nntrain.c` is one such reader. Moves off a disk go through
`movegen_is_legal()` like any other unproven move.

What a position gives a trainer, with nothing regenerated:

| Target or input | Where it comes from |
|---|---|
| The whole position, stacks, reserves and side to move | Replay |
| Game result, and whether it was a road or the flats | The header's outcome |
| A search score, as a second value target | The ply |
| The best move, as a policy target | The ply: every scored ply's move is the search's choice |
| Komi and board size, as inputs | The header |
| A split by game, so positions of one game never land on both sides | The file's structure |
| The eight symmetries, for augmentation | Worked out at training time, so not stored |

## Training a network

One network plays every size, so one fit reads every size's games at once:

```sh
D=external/data/gen1big
yapper nntrain src/net.h 1 0.7 15 15 --share 7=0.33 --share 8=0.2 --lambda 3=0.5 \
    --lambda 4=0.5 external/data/gen[78]-*.ydg $D/6s/gen9-20k-*.ydg $D/3s/*.ydg \
    $D/4s/gen1-*.ydg $D/5s/gen1-*.ydg $D/7s/gen1-*.ydg $D/8s/gen1-*.ydg
make format && make
```

That is the fit of E39's network. The arguments are the output, the one-in-N positions to keep,
`lambda`, the epochs and the threads, then the options, then the games. A size is in the network
when the games have some of it, and a size they do not have counts flats. The engine picks up
`net.h` the next time it is built. After swapping one in by hand, check that bench changed: a
copied file can keep an older date, and `make` will then quietly do nothing.

`--seed n`, before the game files, picks other first weights and another order of the
positions, which is how to see how far two fits of the same games play apart. `--share 8=0.2`
puts a fifth of 8x8's positions into each epoch, drawn afresh each time, for a size with too few
games for every epoch to see them all. Weighting its loss instead would not do: the optimiser
rescales the steps of the weights only that size reaches, so they would move as far as ever.
`--lambda 4=0.5` gives one size a `lambda` of its own, since the best one differs by board: E39.

Only positions the evaluation is really asked about are used. A side able to win on the spot, a
road being answered, a forced result and the random plies all go to the search rather than to a
stand pat, so they are left out. That is why what a road threat is worth is still set by hand.

The target is `lambda` times the search's score, through the scale of 310, plus the rest times
the game's result. The scale is held fixed, which keeps a flat near 100. It sets what a hundredth
of a flat means, and the search's margins are written in those units: letting it float would
retune them by accident.

A sixteenth of the games are held out, chosen by a game's place in its own file, so a game is
held out whatever else a fit reads. The report gives the held-out loss against the target and
against results alone, after every epoch, and at the end for each group of games: one size,
generation and budget. Results alone is the one to compare across fits, and a group's line
compares between any two fits that read its files. It cannot choose `lambda`, since a target
nearer the results always scores nearer them, nor which games to fit on: a fit on only the last
two generations scored best on them by far and lost 80 to one that kept the one before, E39.

Each size's held-out loss is printed every epoch too, since one size can run out of games while
the rest still learn. A held-out loss that rises while the fit's falls means it has: 150,000 games
of 7x7 or 8x8 did that after two epochs, E38 and E39, where 6x6's millions never have. That is
what `--share` is for.

The network written is the mean of the weights over the last tenth of the steps, not where the
last step left them. Even that late, a held-out loss moves by up to 0.003 from one epoch to the
next, so where a fit happened to stop would otherwise decide much of how good it is: E39.

**Fit on more than the last generation.** Self-play only teaches what self-play does. One engine
won so well by racing stones down that only 3% of its games against itself ended in a road, and a
fit on those games alone forgot what roads are worth: it fell far behind against an outside
opponent while its loss looked excellent. Check the outcomes `datacheck` prints before fitting,
and judge a fit against an engine that does not play like it.

A network is fitted on the same rounded weights the engine runs, so the last line, the engine
against the fit on held-out positions, should read within a hundredth or so. Anything more means
the engine and the trainer read a position differently.

An NVIDIA GPU does the fitting if the machine has one. Nothing is needed to build for it: the
tooling build finds the CUDA driver and NVRTC, the toolkit's runtime compiler, when it runs, on
the path or under `CUDA_PATH`, and compiles its kernels for the card it finds. `make release`
leaves it out. `--cpu` before the game files fits on the CPU instead, and the first line of the
report says which was used and why. The threads then only lay out batches and score the held-out
positions.

The two fit the same network to within the rounding of floats, not bit for bit, and each gives
the same bytes on every run: the GPU adds gradients in fixed point, where order does not matter.
The engine is still checked against the fit on the CPU, so a kernel that read a position
differently would show in the last line.

A fit takes every core it is given, so nothing can be timed or matched alongside it. A lower loss
is not a claim of strength: only an SPRT makes one.
