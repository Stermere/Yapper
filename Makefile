# ============================================================================
#  Yapper build system.
#
#  Usage:
#    make                 optimised build for THIS machine (-march=native)
#    make ARCH=generic    no CPU assumptions, runs anywhere the compiler targets
#    make ARCH=popcnt     x86-64 with POPCNT, which is what CI uses
#    make release         the engine alone, with no datagen, fitting or selftest
#    make TUNE=on         every tunable in src/tune.h as a TEI option, for tools/tune.py
#    make debug          unoptimised + assertions (+ sanitizers on POSIX)
#    make bench           build, then run the deterministic node-count benchmark
#    make perft           build, then run the sealed perft suites
#    make selftest        build, then run the board and rule invariants
#    make check           perft + selftest + bench, the gate for any board change
#    make perftdiff       fuzz the engine against tools/takref.py, the slow oracle
#    make speed           race move generation against external/engines
#    make datacheck       training data: read by takref, and the same twice
#    make format          apply .clang-format
#    make clean
#
#  Do not break: `make EXE=<name>` must produce exactly <name> (or <name>.exe)
#  beside this Makefile, and `CC=` must be honoured.
#
#  Sources are compiled as one translation unit, because every hot call crosses
#  files, and LTO would need a linker plugin that not every clang install has.
#  A full rebuild is seconds, which keeps this file free of dependency tracking.
# ============================================================================

EXE  ?= yapper
ARCH ?= native

# `make` predefines CC as `cc`, which `?=` leaves alone, and MSYS2 has no such
# binary on PATH from PowerShell. clang keeps a node's state in registers across
# its moves where gcc reloads it, so it is used if it is there; gcc is what
# every platform here has. Either gives the same bench.
ifeq ($(origin CC),default)
CC := $(shell command -v clang >/dev/null 2>&1 && echo clang || echo gcc)
endif

SRC := $(wildcard src/*.c) $(wildcard src/test/*.c)
HDR := $(wildcard src/*.h) $(wildcard src/test/*.h)

CFLAGS_WARN := -std=c17 -Wall -Wextra -Wshadow -Wcast-qual -Wstrict-prototypes \
               -Wmissing-prototypes -Wpointer-arith -Wwrite-strings
CFLAGS_OPT  := -O3 -fno-math-errno -fomit-frame-pointer -DNDEBUG
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

# Two gcc passes that cost the spread loop more than they gain, and that clang
# does not know, so they are asked about rather than assumed.
GCC_ONLY := -fno-version-loops-for-strides -fno-tree-slp-vectorize
CFLAGS_OPT += $(shell $(CC) -Werror $(GCC_ONLY) -E -x c /dev/null -o /dev/null 2>/dev/null \
                     && echo $(GCC_ONLY))

BIN := $(EXE)$(BIN_EXT)

ifneq ($(findstring debug,$(MAKECMDGOALS)),)
    CFLAGS := $(CFLAGS_WARN) -O1 -g
    ifneq ($(OS),Windows_NT)
        CFLAGS += -fsanitize=address,undefined
    endif
else
    CFLAGS := $(CFLAGS_WARN) $(CFLAGS_OPT) $(CFLAGS_ARCH)
endif

# What ships is the engine alone. It must bench the same as the full build, since that is
# what ties it to the build the gates passed on.
TOOL_SRC := src/datagen.c src/gpu.c src/nntrain.c src/policyfit.c $(wildcard src/test/*.c)
ifneq ($(findstring release,$(MAKECMDGOALS)),)
    SRC     := $(filter-out $(TOOL_SRC),$(SRC))
    CFLAGS  += -DYAPPER_RELEASE -ffunction-sections -fdata-sections
    LDFLAGS += -Wl,--gc-sections
endif

# Off, each tunable is a constant the compiler folds, so the option costs a normal build nothing.
# On, it must bench the same at its defaults, or a tuning run tunes another engine.
TUNE ?= off
ifeq ($(TUNE),on)
    CFLAGS += -DYAPPER_TUNE
else ifneq ($(TUNE),off)
    $(error Unknown TUNE '$(TUNE)': use on or off)
endif

# A stamp of the flags, so switching ARCH rebuilds instead of handing back the
# binary built with the other ones.
FLAGS_NOW := $(CC) $(CFLAGS) $(LDFLAGS)
FLAGS_OLD := $(shell cat .buildflags 2>/dev/null)
ifneq ($(FLAGS_NOW),$(FLAGS_OLD))
    $(shell echo '$(FLAGS_NOW)' > .buildflags)
endif

.PHONY: all debug release bench perft selftest check perftdiff speed datacheck format clean

all: $(BIN)

debug: $(BIN)

release: $(BIN)

$(BIN): $(SRC) $(HDR) .buildflags
	printf '#include "%s"\n' $(SRC:src/%=%) | \
		$(CC) $(CFLAGS) -Isrc -x c - -o $@ $(LDFLAGS) $(LDLIBS)

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

datacheck: $(BIN)
	rm -f external/data/check-*.ydg
	./$(BIN) datagen external/data/check-a.ydg 100 7 2000
	./$(BIN) datagen external/data/check-b.ydg 100 7 2000
	python tools/datacheck.py external/data/check-a.ydg --check-every 1 \
		--same-as external/data/check-b.ydg
	rm -f external/data/check-*.ydg

format:
	clang-format -i src/*.c src/*.h src/test/*.c src/test/*.h

clean:
	rm -f $(EXE) $(EXE).exe .buildflags
