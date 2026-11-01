# Testing methodology

| Test | Question | When |
|---|---|---|
| **perft** | Is move generation legal and complete? | Any board change |
| **selftest** | Is the state behind the moves exactly right? | Any board change |
| **perftdiff** | Does an implementation sharing nothing with this one agree? | Any board change |
| **bench** | Did the search change, and how fast is it? | Every build |
| **speed** | Are we still ahead of the baselines? | Any change to the hot path |
| **SPRT** | Does this actually gain Elo? | Every behavioural change |

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

## perftdiff

```sh
make perftdiff
python tools/perftdiff.py --games 400 --depth 3     # for a board rewrite
```

Random games from takref, compared with the engine on the exact move list, the game result
under four komi values, and perft. After a deliberate rule change only:

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

## speed

```sh
make speed
```

Races perft against the binaries in `external/engines`, best of three, and prints the lead on
each position. Takes about ten minutes, and needs the machine to itself.

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

## Viewing games

```sh
python tools/match.py ... --ptn external/games/run.ptn
python tools/ptnlink.py external/games/run.ptn 17
```

Open the file in https://ptn.ninja to browse a whole match, or open the printed link for one
game. For live analysis of a position, PTN Ninja connects to any TEI engine over a WebSocket:
run `websocketd --port=7731 ./yapper`, then connect from its engine panel with SSL off.
