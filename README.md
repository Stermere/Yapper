# Yapper

A Tak engine in C, aiming to be the strongest there is on a plain CPU, on anything from a
desktop to an ARM board. It plays 3x3 through 8x8 under the
[USTAK rules](https://ustak.org/play-beautiful-game-tak/), with komi as an option.

The board and move generation are complete and verified. Every size is evaluated by a network of
its own, all of one shape and fitted to self-play, and the search has had two passes, which puts it
well ahead of Tiltak at 5+0.05: see [docs/STATUS.md](docs/STATUS.md).

## Build

Any C17 compiler. No dependencies.

```sh
make                 # for this machine
make ARCH=generic    # no CPU assumptions, for ARM and everything else
make check           # perft, selftest and bench
```

On Windows, run `make` from PowerShell with MSYS2's `ucrt64\bin` and `usr\bin` on PATH.

## Play it

```sh
python tools/play.py                   # 5x5, you are White, one second a move
python tools/play.py --size 6 --color black
python tools/play.py --depth 2         # an easier opponent
```

Type `help` for how to write a move. `moves`, `hint` and `undo` are there when you need them.

## Use

Yapper speaks [TEI](docs/TEI.md), so it works with the tools the Tak community already has:

- **PTN Ninja** ([ptn.ninja](https://ptn.ninja)) is the GUI. Open a game file, or connect for
  live analysis with `websocketd --port=7731 ./yapper`.
- **racetrack** runs engine matches.

It also has its own match runner, which needs nothing but Python:

```sh
python tools/match.py --base old.exe --sprt --book tests/books/6s_4ply.txt \
    --ptn external/games/run.ptn
python tools/ptnlink.py external/games/run.ptn 17     # a ptn.ninja link for game 17
```

From the command line:

```sh
yapper bench
yapper perft 6 5
yapper perft 6 3 tps "x6/x6/x6/x6/x6/x6 1 1"
```

## Docs

| | |
|---|---|
| [docs/STATUS.md](docs/STATUS.md) | what is done, what is not, what comes next |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | how the board works and why it is fast |
| [docs/TESTING.md](docs/TESTING.md) | how a change gets verified |
| [docs/EXPERIMENTS.md](docs/EXPERIMENTS.md) | every measurement |
| [docs/TEI.md](docs/TEI.md) | the protocol as real programs speak it |

## Thanks

[racetrack](https://github.com/MortenLohne/racetrack) for what TEI is in practice, and
[PTN Ninja](https://github.com/gruppler/PTN-Ninja) for making games worth looking at.
