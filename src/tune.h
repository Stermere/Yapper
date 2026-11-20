/* tune.h - every number the search plays by, in one list, so a tuner can reach them all.
 *
 * In a normal build each is an enum constant, which the compiler folds as it did the literal it
 * replaced. `make TUNE=on` makes each an int and a TEI spin option, so tools/tune.py can set them
 * game by game instead of building a binary per candidate. A name, value and range are written
 * here and nowhere else, so the option, the variable and the tuner cannot disagree.
 *
 * The tuner perturbs each by a twentieth of its range, at least 1, so a range says how far a
 * candidate may stray as well as where it must stay. Units are chosen so the defaults give the
 * integer arithmetic they replaced exactly: `make TUNE=on` must bench the same as `make`. */
#ifndef TUNE_H
#define TUNE_H

#include <stddef.h>

#include "types.h"

/* X(name, default, min, max). Scores are in hundredths of a flat. */
/* clang-format off */
#define TUNABLES(X)                                                                         \
    /* The leaf search: how many plies of forced replies it follows. */                    \
    X(QuiesceDepth,        1,     1,    8)                                                  \
    /* Reverse futility: below this depth, by this margin a ply. */                        \
    X(RfpDepth,            7,     1,    12)                                                 \
    X(RfpMargin,           53,    20,   300)                                                \
    /* Null move: from this depth, reduced by base + depth / divisor. */                   \
    X(NmpDepth,            4,     1,    8)                                                  \
    X(NmpBase,             5,     1,    8)                                                  \
    X(NmpDivisor,          4,     1,    12)                                                 \
    /* Singular extension: from this depth, on a table entry at most this much shallower,  \
     * by a margin in sixteenths a ply. */                                                  \
    X(SingularDepth,       5,     3,    12)                                                 \
    X(SingularTtDepth,     3,     1,    6)                                                  \
    X(SingularMargin,      41,    0,    160)                                                \
    /* Move-count pruning: below this depth, after (base + slope * depth^2) / 4 moves, and \
     * its own pair when improving. Quarters, so a slope of 1.5 is exact. */               \
    X(LmpDepth,            9,     1,    12)                                                 \
    X(LmpBase,             13,    0,    48)                                                 \
    X(LmpSlope,            3,     1,    24)                                                 \
    X(LmpImprovingBase,    26,    0,    96)                                                 \
    X(LmpImprovingSlope,   4,     1,    48)                                                 \
    /* Late move reductions: base + ln(depth) ln(moves) / divisor, both in hundredths,     \
     * from this depth and move, more at a cut node and less at a PV node. */              \
    X(LmrBase,             19,    0,    200)                                                \
    X(LmrDivisor,          194,   100,  400)                                                \
    X(LmrDepth,            3,     1,    8)                                                  \
    X(LmrMoves,            4,     1,    12)                                                 \
    X(LmrCutNode,          1,     0,    3)                                                  \
    X(LmrPvNode,           1,     0,    3)                                                  \
    /* Replies to a road, from this one on, are this many plies shallower; 0 reduces none. \
     * None at the root, which plays every move. */                                       \
    X(AnswerFrom,          3,     2,    12)                                                 \
    X(AnswerReduce,        1,     0,    3)                                                  \
    /* History: a cutoff earns min(depth^2 * mul, max), entries level off at the limit,    \
     * and a search starts with this many sixteenths of what the last one left. */          \
    X(HistoryBonusMul,     20,    1,    64)                                                 \
    X(HistoryBonusMax,     3698,  500,  16000)                                              \
    X(HistoryLimit,        6300,  4096, 65536)                                              \
    X(HistoryKeep,         12,    0,    16)                                                 \
    /* The move prior, from this depth: history units for each 256th of a nat. */          \
    X(PriorWeight,         5,     0,    24)                                                 \
    X(PriorDepth,          4,     1,    6)                                                  \
    /* Below it, the order by hand, in history units: a flat beside our road stones or     \
     * not, a capstone, a placement that leaves our road to finish or takes a square       \
     * that would leave theirs, and a spread for each top it wins. */                       \
    X(OrderFlatNear,       339,   0,    2000)                                               \
    X(OrderFlat,           91,    0,    1000)                                               \
    X(OrderCap,            222,   0,    1500)                                               \
    X(OrderRoadOurs,       6080,  0,    32768)                                              \
    X(OrderRoadTheirs,     3918,  0,    16384)                                              \
    X(OrderSwing,          570,   0,    2048)                                               \
    /* Aspiration: from this depth, a window that grows by this percent at each fail,      \
     * until it is past the limit and the search opens fully. */                            \
    X(AspirationDepth,     4,     1,    10)                                                 \
    X(AspirationWindow,    21,    5,    200)                                                \
    X(AspirationGrowth,    154,   120,  400)                                                \
    X(AspirationLimit,     478,   100,  2000)                                               \
    /* The evaluation's error, learned in the search: percent of what is held for the move  \
     * that led to a node and for the mover's own move before it, 4096ths of each error     \
     * taken in for each ply of depth, up to a most, and the most the two may add. */       \
    X(CorrLast,            105,   0,    200)                                                \
    X(CorrOwn,             98,    0,    200)                                                \
    X(CorrRate,            5,     0,    128)                                                \
    X(CorrRateMax,         86,    1,    1024)                                               \
    X(CorrMax,             395,   0,    1000)                                               \
    /* What the evaluation takes off for one road threat by the other side, and for more. */ \
    X(ThreatOne,           42,    0,    400)                                                \
    X(ThreatMany,          222,   0,    1000)                                               \
    /* The clock. A move gets the time left over moves + stones * per-stone / 100, at      \
     * least min, and this percent of the increment. The hard limit is a percent of that,  \
     * capped at a percent of the time left, and no iteration starts past a percent of     \
     * the share. Only games on a clock reach these. */                                     \
    X(TimeMoves,           3,     0,    30)                                                 \
    X(TimeMovesPerStone,   150,   50,   300)                                                \
    X(TimeMovesMin,        9,     2,    40)                                                 \
    X(TimeIncrement,       78,    0,    100)                                                \
    X(TimeHard,            303,   100,  800)                                                \
    X(TimeHardCap,         49,    10,   80)                                                 \
    X(TimeIteration,       57,    10,   100)                                                \
    /* That percent of the share is itself a percent of this: the most while the best move \
     * has just changed, a step less for each iteration it has held, down to the least; and \
     * this percent more for every 100 the score fell at the last iteration, counted up to  \
     * the cap, and a quarter of it the other way. */                                       \
    X(TimeStableMax,       240,   100,  400)                                                \
    X(TimeStableStep,      40,    0,    120)                                                \
    X(TimeStableMin,       60,    20,   100)                                                \
    X(TimeFall,            31,    0,    100)                                                \
    X(TimeFallMax,         118,   0,    400)
/* clang-format on */

#define X(name, value, min, max) \
    _Static_assert((min) <= (value) && (value) <= (max), #name " starts outside its range");
TUNABLES(X)
#undef X

#ifdef YAPPER_TUNE

#define X(name, value, min, max) extern int name;
TUNABLES(X)
#undef X

int tune_count(void);
void tune_info(int i, const char **name, int *value, int *min, int *max);

/* Clamps to the range rather than refusing, so a value is never silently left as it was. False
 * when no tunable has that name. */
bool tune_set(const char *name, size_t length, int value);

#else

#define X(name, value, min, max) name = (value),
enum { TUNABLES(X) };
#undef X

#endif

#endif
