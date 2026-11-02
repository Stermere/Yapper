# Training data

Self-play games are the one training input Yapper has, for the hand-written evaluation now and
for a network later. The format is built around that second use: **a file holds games, never
positions or features.** Replaying a game gives back every position in it, so nothing has to be
regenerated when the evaluation, its terms or a network's inputs change.

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

Each game starts from 4 to 9 random plies, chosen by kind so that capstones turn up at every
point of the opening, and is thrown away if a short search already calls it decided. Komi is
mostly 2, with enough of 0, 1, 1.5, 2.5 and 3 for a fit or a network to learn what it does. Games
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
| 2 | i16 | the search's score, for the side to move, or `0x7FFF` for a random ply |

A score at or beyond 29872 in size is a forced win or loss in `30000 - |score|` plies. Any change
to any of this needs a new magic. A file that ends part way through a game came from a killed
run, and readers stop at the last whole game.

## Reading it

In C, `data_replay()` calls a function with every position of a file, its game and its ply. That
function is the seam: `tune.c` is one such reader, and whatever writes a network's inputs will be
another. Moves off a disk go through `movegen_is_legal()` like any other unproven move.

What a position gives a trainer, with nothing regenerated:

| Target or input | Where it comes from |
|---|---|
| The whole position, stacks, reserves and side to move | Replay |
| Game result, and whether it was a road or the flats | The header's outcome |
| A search score, as a second value target | The ply |
| The best move, as a policy target | The ply: every move after the random ones is the search's choice |
| Komi and board size, as inputs | The header |
| A split by game, so positions of one game never land on both sides | The file's structure |
| The eight symmetries, for augmentation | Worked out at training time, so not stored |

The search does not know what its evaluation is, so datagen will drive a network the moment one
is behind `evaluate()`.

## Fitting the weights

```sh
yapper tune src/weights.h 1 0 external/data/gen1-*.ydg     # report the scale of the build
yapper tune src/weights.h 1 310 external/data/gen*-*.ydg   # fit, and rewrite the file
make format && make
```

The arguments are the output, the one-in-N positions to keep, and the logistic scale. The fit is
a logistic regression of each position's trace against its game's result, and since a score is
linear in the weights it has one answer. It lives in the binary because it reads millions of
positions through the board and the evaluation, which would take Python hours.

Only positions the evaluation is really asked about are used. A side able to win on the spot, a
road being answered, a forced result and the random plies all go to the search rather than to a
stand pat, so they are left out, and terms no kept position has hold their value. That is why the
two `THREAT` weights are still hand-set.

The scale is held fixed at 310 from one fit to the next, which keeps a flat near 100. It sets what
a hundredth of a flat means, and the search's margins are written in those units: letting it
float would retune them by accident.

A fit starts from the weights in the binary and overwrites the file, so a build must follow it
before anything else reads the new weights. After swapping `weights.h` by hand, check that bench
changed: a copied file can keep an older date, and `make` will then quietly do nothing.

A sixteenth of the games are held out and reported on, and a held-out loss that stops falling
means the data has run out. A lower loss is still not a claim of strength. Only an SPRT makes one.

**Fit on more than the last generation.** Self-play only teaches what self-play does. One engine
won so well by racing stones down that only 3% of its games against itself ended in a road, and a
fit on those games alone forgot what roads are worth: it fell far behind against an outside
opponent while its loss looked excellent. Check the outcomes `datacheck` prints before fitting,
and judge a fit against an engine that does not play like it.

## Training a network

```sh
yapper nntrain 6 src/net6.h 1 0.5 20 15 external/data/gen6-*.ydg external/data/gen7-*.ydg
make format && make
```

The arguments are the board size, the output, the one-in-N positions to keep, `lambda`, the
epochs and the threads. Each size has its own network and is trained on its own games, since a
square means something different on each board: `datagen` takes the size. The engine picks up a
`netN.h` the next time it is built, and a size with none plays the hand-written evaluation.

Positions are kept as `tune` keeps them, and a sixteenth of the games are held out the same way.
The target is `lambda` times the search's score, through the scale of 310, plus the rest times
the game's result. The report gives the held-out loss against that target and against results
alone, which is the one to compare with the hand-written evaluation's on the same games. That
comparison is only fair on games neither was fitted on.

A network is fitted on the same rounded weights the engine runs, so the last line, the engine
against the fit on held-out positions, should read within a hundredth or so. Anything more means
the engine and the trainer read a position differently.

A fit takes every core it is given, so nothing can be timed or matched alongside it. A lower loss
is not a claim of strength: only an SPRT makes one.
