# CLAUDE.md

Guidance for Claude Code when working in this repository.

## What this is

A Tak engine in **C** (C17), built to be the strongest there is, on a CPU, on anything
from a desktop to an ARM board. Rules are the USTAK ones:
https://ustak.org/play-beautiful-game-tak/. It speaks **TEI**, the protocol the Tak engine
world uses, so racetrack and PTN Ninja work with it unchanged.

**The current goal is to beat Tiltak.** The board, move generation and harness are finished and
verified, and Yapper leads the baselines on speed. Strength work goes in order: a stronger
evaluation base, then the search, then long overnight runs on the evaluation. The hand-written
evaluation is a stepping stone: networks will replace it, so training data is kept in a form
that does not depend on it. See [docs/STATUS.md](docs/STATUS.md).

**Never read the source of a rival Tak engine**, not to explain a result and not to see how
something is done. Yapper's ideas stay its own. `external/engines/` holds black-box binaries.
racetrack and PTN Ninja are tools rather than rivals, so working with them is fine.

## Build and test

```sh
make                # build for this machine
make ARCH=generic   # no CPU assumptions
make check          # perft + selftest + bench: the gate for any board change
make perftdiff      # fuzz against tools/takref.py, the independent oracle
make bench          # deterministic node count
make speed          # race move generation against external/engines
make datacheck      # training data: readable without the engine, right by takref, repeatable
make debug          # assertions, plus sanitizers on POSIX
make release        # the engine alone: no datagen, fitting, GPU or selftest
make format         # apply .clang-format
```

On Windows, `make` is MSYS2's and must run from PowerShell, with `C:\msys64\ucrt64\bin` and
`C:\msys64\usr\bin` on PATH. From Git Bash it exits 127 without a word: the two MSYS
runtimes collide.

## Non-negotiable invariants

Breaking any of these silently destroys the ability to measure progress, which is worse
than a bug: it makes every later result untrustworthy.

1. **Bench must stay deterministic.** Identical node counts across runs and machines. Keys
   come from a fixed seed, never from `time()` or any entropy source, and bench pins its
   own hash size.

2. **Bench must keep printing `<nodes> nodes <nps> nps`** as its final line.

3. **`make EXE=<name>`** must produce exactly `<name>` (or `<name>.exe`) beside the
   Makefile, and `CC=` must be honoured.

4. **The TEI loop must never block on the search.** `isready`, `stop` and `quit` are
   answered while thinking. The search has its own thread; keep it that way.

5. **Moves that did not come from the generator go through `movegen_is_legal()`** before
   being made. Table entries can be stale or collided, and any 16-bit word must be safely
   rejected: selftest throws random words at it for exactly this reason.

6. **`board_make()` must keep the hash equal to `board_compute_hash()`**, and
   `board_unmake()` must restore the board bit for bit. Selftest compares the whole struct
   with memcmp, so a new field that make touches must be restored by unmake.

7. **`board_outcome_after()` must always agree with `board_outcome()`.** The first only
   floods from squares the move changed, which is the whole reason a node is cheap. It
   relies on the position before the move not having been over.

8. **`search_clear()` must reset everything that carries between searches.** Otherwise
   results depend on what was searched before.

9. **`tools/takref.py` stays naive.** It is the second opinion, and its worth is that it
   shares nothing with `src/`. Never optimise it, and never fix it to match the engine
   without reading the rule first. The same goes for `tests/perft/published.perft`: those
   numbers belong to other engines.

10. **Only tags PTN Ninja knows go in a PTN file.** It rejects the whole game on anything
    else, racetrack's `Termination` included. Its list is `formats` in
    `src/Game/PTN/Tag.js`.

11. **The full perft count must make and unmake every leaf for real.** `board_touched()` keeps
    a compiler from deleting them as dead code. Without it a build once reported 517 Mnps for
    an empty loop.

12. **What is fitted must be what plays.** A hand-written trace must add up to
    `evaluate_hce()`, from the same body, and a network is fitted on the fixed point the engine
    runs, so `nntrain` must report the engine within a hundredth or so of its fit. If the two
    differ, a fit tunes something that does not play, and nothing in a match would say so.
    Selftest holds every evaluation unchanged under the board's symmetries and a colour swap.

13. **Training data is games, never features, and its format is a contract.** It has to outlive
    every evaluation trained on it, networks included. Any change to `docs/DATA.md`'s format
    needs a new magic. Datagen must stay deterministic for a seed and build, and take nothing
    from the clock.

14. **A network frame carried from an ancestor must equal a refresh, bit for bit.** The search
    only updates the squares its moves touched, so a square missed there leaves a wrong
    accumulator that still plays. Selftest holds it on random weights for every size, whether or
    not a network is compiled in.

15. **`make release` must bench the same as `make`.** It leaves the tools out, and the matching
    node count is what ties the binary that ships to the one the gates passed on.

## Testing discipline

Read [docs/TESTING.md](docs/TESTING.md). Record every result in
[docs/EXPERIMENTS.md](docs/EXPERIMENTS.md). The short version:

- **Correctness changes** (board, movegen): `make check` and `make perftdiff` must pass
  exactly. Not close: exactly.
- **Pure speedups**: bench node count must be *unchanged*. If it changed, the change is
  behavioural.
- **Behavioural changes** (search, eval, time): need a passing SPRT from `tools/match.py`.
  Do not commit an Elo claim without one, and do not argue a patch is good because it seems
  obviously good. Roughly half of obviously better patches in chess measure neutral or
  worse.
- **One change per test.** Two at once tells you nothing about either.

Do not weaken a failing test to make it pass, and never reseal a perft suite to make it
pass.

## Code style

- C17, 4-space indent, 100-column limit, enforced by `.clang-format`. Run `make format`.
- Comments explain **why**, not what, and are short. Keep the constraint a reader must not
  break. Measurements belong in docs/EXPERIMENTS.md.
- Mark unfinished work `TODO(engine):` with enough context to act on.
- Keep `src/` flat. `src/test/` is the one exception: gates compiled into the binary but
  never reached in play, so a gate cannot pass on a different build than the one that plays.
  `make release` leaves them out, and invariant 15 is what keeps it the same build.
- No dependencies. The engine links libc, libm and the platform threads, and that is what
  lets it build for anything. No x86 intrinsics in the core: use compiler builtins and let
  them lower to POPCNT or NEON, or keep any faster path behind a portable fallback.
- The tools may use an NVIDIA GPU, through `src/gpu.c`, which finds the driver and NVRTC when
  it runs. No build may need CUDA, a machine without a GPU must fall back to the CPU, and
  `make release` leaves all of it out with the other tools.
- `tools/` is Python, stdlib only.

## Where things live

| Path | Contents |
|---|---|
| `src/` | the engine, flat |
| `src/netN.h` | the network for NxN, written by `yapper nntrain`; a size without one plays the hand-written evaluation |
| `src/test/` | selftest, in the binary on purpose |
| `tests/perft/` | `published.perft` from other engines, `sealed.perft` from takref |
| `tests/books/` | opening books, built by `tools/mkbook.py` |
| `tools/takref.py` | the rules, written to be obviously right rather than fast |
| `tools/perftdiff.py` | fuzz the engine against takref, seal suites |
| `tools/match.py` | TEI match runner, referee and pentanomial SPRT |
| `tools/ptnlink.py` | a ptn.ninja link for a game |
| `tools/play.py` | play the engine in a terminal |
| `tools/speedcmp.py` | race move generation against the baselines |
| `tools/gamestats.py` | how a match went, side by side, from its PTN |
| `tools/datagen.py` | self-play for training, on every core |
| `tools/datacheck.py` | read training data without the engine, and check it against takref |
| `docs/` | status, architecture, testing, TEI, data, experiments |
| `external/` | **gitignored**: baseline binaries, match output, training data |

Never commit anything under `external/`, and never commit binaries.
