# Testing methodology

| Test | Question | When |
|---|---|---|
| **perft** | Is move generation legal and complete? | Any board change |
| **selftest** | Is the state behind the moves exactly right? | Any board change |
| **perftdiff** | Does an implementation sharing nothing with this one agree? | Any board change |
| **bench** | Did the search change, and how fast is it? | Every build |
| **speed** | Are we still ahead of the baselines? | Any change to the hot path |
| **datacheck** | Is training data right, and repeatable? | Any change to datagen |
| **SPRT** | Does this actually gain Elo? | Every behavioural change |
| **SPSA** | Where might the tunables be better? | Before an SPRT, never instead of one |

## Three independent opinions on the rules

A test written by the author of the code can only catch mistakes, not misreadings. So the
rules are checked against three sources that do not share a reading:

1. **Published counts** (`tests/perft/published.perft`), from two other engine projects.
   Only the transcription was done here. The deepest is a 48,986,506,534-node endgame.
2. **`tools/takref.py`**, a naive implementation written from the USTAK page. It reproduces
   the published counts by itself, so it earns its place as an oracle, and
   `tests/perft/sealed.perft` is sealed from it, covering sizes 3-8 and every game phase.
3. **The rule page itself**: `rules()` in `src/test/selftest.c` pins one position to each
   rule, so a failure names the rule that broke.

## perft

```sh
make perft
```

Two conventions exist and both have published numbers, so a suite line names which it uses.
`none`: a game that ends early contributes no leaves. `leaf`: it counts as one. They differ on any position with a win inside the horizon, which is why the
same "suicide" position reads 956,736 in one and 957,000 in the other.

To chase a mismatch, compare `divide N` from the engine's TEI loop with `perft` on the same
position in `takref`, then recurse into the first move that differs.

## selftest

```sh
make selftest
```

Plays thousands of random games on every size, biased towards spreads so tall stacks form,
and at every position checks: all derived state against the stacks; the incremental hash
against a full recompute; unmake against a byte compare of the whole board; the fast road
check against the slow one; PTN and TPS round trips; and legality of 400 random 16-bit words
against the generated list. The random words are what caught a stale-table bug in the first
build, and they are not optional.

From the third ply on it also checks the evaluation the build plays: that the score survives
every rotation and reflection of the board and, with no komi, swapping the colours. Nothing else
would catch an evaluation that treats a square or a colour differently, since games still play.
Deliberate bugs were put in to confirm each of these fails.

The test for a road by spread is held to a brute force over every generated move, at every
position. It rules moves out without playing them, so an error there means a win the search can
no longer see, and games would go on looking normal. Five deliberate bugs confirmed this fails too.

Then it installs a network of random weights on every size, whatever is compiled in, and plays
more games through it. At every position a frame carried over gaps from an earlier one, as table
hits leave them in a search, must equal one built from nothing, bit for bit, and score the same;
and the score must survive the eight symmetries and a colour swap. That last holds for any
weights, by construction, so random ones are a fair test. Three deliberate bugs, one per check,
confirmed each fails: one orientation left untied, a carry that forgot to take black tokens away,
and colours not folded under a black top. A fourth, from when every board sat on one grid, the
grid's transpose dropped for every board smaller than it, failed it 155,380 times.

## perftdiff

```sh
make perftdiff
python tools/perftdiff.py --games 400 --depth 3     # for a board rewrite
```

Random games from takref, compared with the engine on the exact move list, the game result
under four komi values, and perft. It takes every core for about 48 minutes, so nothing can be
timed alongside it. After a deliberate rule change only:

```sh
python tools/perftdiff.py --seal tests/perft/sealed.perft
```

Resealing to make a suite pass is how the numbers stop meaning anything.

## bench

```sh
make bench
```

Fixed depth on 13 frozen positions, single-threaded, table pinned at 64 MB. The last line is
`<nodes> nodes <nps> nps`. An unchanged node count proves a patch changed nothing about the
search; a changed one means it is behavioural and needs an SPRT.

`make release` leaves selftest, datagen and the fitting tools out of the binary, so its gates
cannot run on it. Its bench must equal the full build's, and that is what says it is the engine
the gates passed on. `make TUNE=on` must bench the same too, at its defaults: otherwise a tuning
run tunes another engine.

## speed

```sh
make speed
```

Races perft against the binaries in `external/engines`, best of three, and prints the lead on
each position. Takes about fifteen minutes, and needs the machine to itself. syntaks and pentakle
have no full count, so they only appear on bulk: E13 is how that was established.

A rebuild alone moves these numbers by about 3%, because code layout changes, so do not read
anything into a difference smaller than that. Bulk runs under 100 ms are noise: measure a
change at one depth deeper.

The full count must time real work. `board_touched()` in `perft.c` forces the position into
memory between make and unmake at every leaf. Without it, a compiler that inlines both may delete
nearly all of them, and report an empty loop as a record.

## SPRT

```sh
cp yapper.exe external/baselines/base.exe     # before making the change
make
python tools/match.py --base external/baselines/base.exe --sprt --book tests/books/6s_4ply.txt
```

The default is 10+0.1 on 6x6 with 2 komi, where the strongest engines are compared. Each
opening is played twice with colours reversed, and the pair is the unit of evidence: the
pentanomial model reads pairs, which cancels the book's bias and needs far fewer games than
counting wins. The test is a GSPRT with bounds `[elo0, elo1]` and error rates `alpha`,
`beta`, default `[0, 5]` and 0.05 each.

Do not stop early because the number looks good, and do not read the Elo of a passed test as
accurate: an SPRT measures which side of the bounds a change is on, and stopping at a bound
biases the estimate upward.

A change that is meant to be neutral, such as a new hash, is a non-regression test: say what it
must not lose, with `--elo0 -20 --elo1 5`. At 2+0.02 that takes minutes, where tighter bounds
take hours, because pairs in Tak are polarised. A failed test stands. Do not rerun it to get a
pass; change the design, or drop it.

Run nothing else during a match, and keep `--concurrency` below the core count: a starved
engine flags, and the result is noise that looks like data. Stay at or above 2+0.02 on 8x8.

The harness was tested on cases with known answers. Results are in EXPERIMENTS.md, E1 to E3.

### Using racetrack instead

`racetrack` has the same GSPRT. Only plain matches have been run through it so far:

```sh
external/engines/racetrack.exe --engine path=external/baselines/base.exe --engine path=yapper.exe \
    --all-engines tc=10+0.1 --komi 2 -s 6 -b tests/books/6s_4ply.txt \
    --format sprt --sprt elo0=0 elo1=5 alpha=0.05 beta=0.05
```

It draws on threefold repetition, as does `tools/match.py`. The USTAK rules have no
repetition rule, so that is a tournament convention rather than a rule of the game.

## SPSA

```sh
make TUNE=on EXE=yapper-tune
python tools/tune.py --list
python tools/tune.py --name lmr --params LmrBase,LmrDivisor --pairs 20000 --tc 2+0.02 \
    --book tests/books/6s_4ply.txt
python tools/tune.py --name lmr --report
```

Every number the search plays by is in `src/tune.h`, one line each with its default and range.
A normal build folds them as constants; `make TUNE=on` makes each a TEI spin option, so one
binary can play any set of values. `tools/tune.py` reads the list from the engine, plays pairs
between the values either side of where it stands, and moves every parameter at once along the
result: fishtest's schedule, one pair an iteration, on every core. It saves after every pair to
`external/tune/<name>.json`, `--resume` carries on, and `tools/progress.py` shows the run, as
paused while it waits for one. A run plays its own copy of the engine, `external/tune/<name>.exe`,
so it can span days and rebuilds. `--pause`, from another terminal, lets the pairs being played
finish and exits; Ctrl+C, a closed window or a reboot lose only those pairs. Keep the machine from
sleeping: a game asleep mid-move loses on time.

SPSA does not measure. It walks wherever its noise leads and reports where it ended, so what it
prints is a candidate. `--report` gives the SPRT that decides it: the tune build against itself,
with the candidate's values on one side. Pass that, confirm it at another time control, and only
then change the defaults in `src/tune.h`. Travel goes as `--r-end` times `--pairs`, so a short
run at the default 0.002 barely moves and looks like a confirmation of the defaults.

A run at `--nodes` never sees the `Time` tunables, and one at a clock tunes for that clock: keep
them out of a run whose conditions cannot reach them, with `--params` or `--exclude`.

## Viewing games

```sh
python tools/match.py ... --ptn external/games/run.ptn
python tools/ptnlink.py external/games/run.ptn 17
```

Open the file in https://ptn.ninja to browse a whole match, or open the printed link for one
game. For live analysis of a position, PTN Ninja connects to any TEI engine over a WebSocket:
run `websocketd --port=7731 ./yapper`, then connect from its engine panel with SSL off.
