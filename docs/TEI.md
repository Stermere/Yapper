# TEI

TEI is UCI with `tei` in place of `uci`, PTN moves, and TPS positions. No written
specification exists, so this file records what the two programs that matter actually send:
read from racetrack's `src/game.rs` and `src/engine.rs`, and PTN Ninja's `src/bots/tei.js`.

## What Yapper accepts

| Command | Notes |
|---|---|
| `tei` | Answers `id name`, `id author`, `option` lines, `teiok` |
| `isready` | Answered at once, even during a search |
| `setoption name <n> value <v>` | `HalfKomi`, `Hash`, and the tunables in a `TUNE=on` build |
| `teinewgame <size>` | racetrack's form |
| `teinewgame size <s> halfkomi <k>` | PTN Ninja's TEI 1 form |
| `position startpos [moves ...]` | |
| `position tps <tps> [moves ...]` | |
| `go wtime <ms> btime <ms> winc <ms> binc <ms>` | racetrack sends all four |
| `go movetime <ms>`, `go depth <n>`, `go nodes <n>` | |
| `go infinite` | `bestmove` waits for `stop`, as in UCI |
| `stop`, `quit` | Answered at once |

Not TEI, for tools and people: `print`, `moves`, `perft <n>`, `divide <n>`, `bench`.

## Options

- `HalfKomi`: spin, -10 to 10, default 0. The komi in half points, in black's favour, so 4
  is 2 komi. The USTAK rules have no komi, which is why the default is 0.
  racetrack's `--komi 2` sends `setoption name HalfKomi value 4`.
- `Hash`: spin, in megabytes, default 64.
- In a `make TUNE=on` build only, every tunable in `src/tune.h`, as a spin with its name, default
  and range. A value outside the range is clamped to it.

## Output

```
info depth 9 seldepth 13 score cp -119 nodes 8638856 nps 3999470 hashfull 41 time 2160 pv e3 d5
bestmove e3
```

Scores are in hundredths of a flat, from the side to move's point of view.

A forced result is `score mate <plies>`, negative when losing. PTN Ninja reads it as plies and
shows it as a win in that many. racetrack's parser understands only `cp`, so it skips those
lines with a warning, which costs nothing but the comment on that move in its PTN.

`bestmove (none)` means the game in the given position is already over.

## Compatibility notes

- racetrack sends `position startpos moves ` with a trailing space.
- racetrack drops an engine without `HalfKomi` to 0 komi only, so the option must stay.
- Options at their default are never sent, so the default must be what the engine really does.
- Declaring a `size` line during `tei` flips PTN Ninja to TEI 1. Yapper does not declare it,
  and accepts the TEI 1 `teinewgame` anyway.
- Spreads may be spelled `1a1>1` or `a1>`, and a flat `Fa1` or `a1`. A trailing `*` on a
  flattening move and any annotation marks are ignored on input and never written.
