# ============================================================================
#  Yapper build system.
#
#  Usage:
#    make                 optimised build for THIS machine (-march=native)
#    make ARCH=generic    no CPU assumptions, runs anywhere the compiler targets
#    make ARCH=popcnt     x86-64 with POPCNT, which is what CI uses
#    make debug           unoptimised + assertions (+ sanitizers on POSIX)
#    make bench           build, then run the deterministic node-count benchmark
#    make perft           build, then run the sealed perft suites
#    make selftest        build, then run the board and rule invariants
#    make check           perft + selftest + bench, the gate for any board change
#    make perftdiff       fuzz the engine against tools/takref.py, the slow oracle
#    make speed           race move generation against external/engines
#    make format          apply .clang-format
#    make clean
#
#  Do not break: `make EXE=<name>` must produce exactly <name> (or <name>.exe)
#  beside this Makefile, and `CC=` must be honoured.
#
#  Sources are compiled in a single invocation with LTO. A full rebuild is
#  about a second, which keeps this file free of dependency tracking.
# ============================================================================

EXE  ?= yapper
ARCH ?= native
CC   ?= gcc

# `make` predefines CC as `cc`, which `?=` leaves alone. MSYS2 has no such
# binary on PATH from PowerShell, and gcc is what every platform here has.
ifeq ($(origin CC),default)
CC := gcc
endif

SRC := $(wildcard src/*.c) $(wildcard src/test/*.c)
HDR := $(wildcard src/*.h) $(wildcard src/test/*.h)

CFLAGS_WARN := -std=c17 -Wall -Wextra -Wshadow -Wcast-qual -Wstrict-prototypes \
               -Wmissing-prototypes -Wpointer-arith -Wwrite-strings
CFLAGS_OPT  := -O3 -flto -funroll-loops -fno-math-errno -fomit-frame-pointer -DNDEBUG
LDLIBS      := -lm

ifeq ($(OS),Windows_NT)
    BIN_EXT := .exe
    LDFLAGS += -static -Wl,--stack,8388608
else
    BIN_EXT :=
    LDLIBS  += -pthread
endif

ifeq ($(ARCH),native)
    CFLAGS_ARCH := -march=native
else ifeq ($(ARCH),popcnt)
    CFLAGS_ARCH := -mpopcnt
else ifeq ($(ARCH),generic)
    CFLAGS_ARCH :=
else
    $(error Unknown ARCH '$(ARCH)': use native, popcnt or generic)
endif

BIN := $(EXE)$(BIN_EXT)

ifneq ($(findstring debug,$(MAKECMDGOALS)),)
    CFLAGS := $(CFLAGS_WARN) -O1 -g
    ifneq ($(OS),Windows_NT)
        CFLAGS += -fsanitize=address,undefined
    endif
else
    CFLAGS := $(CFLAGS_WARN) $(CFLAGS_OPT) $(CFLAGS_ARCH)
endif

# A stamp of the flags, so switching ARCH rebuilds instead of handing back the
# binary built with the other ones.
FLAGS_NOW := $(CC) $(CFLAGS) $(LDFLAGS)
FLAGS_OLD := $(shell cat .buildflags 2>/dev/null)
ifneq ($(FLAGS_NOW),$(FLAGS_OLD))
    $(shell echo '$(FLAGS_NOW)' > .buildflags)
endif

.PHONY: all debug bench perft selftest check perftdiff speed format clean

all: $(BIN)

debug: $(BIN)

$(BIN): $(SRC) $(HDR) .buildflags
	$(CC) $(CFLAGS) -Isrc $(SRC) -o $@ $(LDFLAGS) $(LDLIBS)

bench: $(BIN)
	./$(BIN) bench

perft: $(BIN)
	./$(BIN) perfttest tests/perft/published.perft
	./$(BIN) perfttest tests/perft/sealed.perft

selftest: $(BIN)
	./$(BIN) selftest

check: perft selftest bench

perftdiff: $(BIN)
	python tools/perftdiff.py --engine ./$(BIN)

speed: $(BIN)
	python tools/speedcmp.py --engine ./$(BIN)

format:
	clang-format -i src/*.c src/*.h src/test/*.c src/test/*.h

clean:
	rm -f $(EXE) $(EXE).exe .buildflags
