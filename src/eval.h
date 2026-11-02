/* eval.h - the static evaluation. */
#ifndef EVAL_H
#define EVAL_H

#include "board.h"
#include "nn.h"

/* What a position is scored on. A term's count is from the side to move's point of view, so
 * a term both sides have is mine less theirs.
 *
 * LEAD is the flat count in half flats, komi included, by how many stones the game has left:
 * that is what it will be decided on, and how much that matters climbs too steeply near the
 * end for a blend of two values. KOMI gives back the part of komi that an opening does not owe,
 * since a road ignores it. INNER is a stone off the edge, and CORE one that is off the edge by
 * two. GROUP_n is a group n files or ranks short of spanning the board. */
#define EVAL_TERMS(X) \
    X(LEAD_1)         \
    X(LEAD_2)         \
    X(LEAD_3)         \
    X(LEAD_4)         \
    X(LEAD_6)         \
    X(LEAD_9)         \
    X(LEAD_14)        \
    X(LEAD_MORE)      \
    X(KOMI)           \
    X(RESERVE)        \
    X(WALL)           \
    X(CAP)            \
    X(FLAT_INNER)     \
    X(FLAT_CORE)      \
    X(FLAT_CORNER)    \
    X(WALL_INNER)     \
    X(WALL_CORE)      \
    X(CAP_INNER)      \
    X(CAP_CORE)       \
    X(FRIEND_FLAT)    \
    X(FRIEND_WALL)    \
    X(FRIEND_CAP)     \
    X(CAPTIVE_FLAT)   \
    X(CAPTIVE_WALL)   \
    X(CAPTIVE_CAP)    \
    X(HARD_CAP)       \
    X(GROUP_1)        \
    X(GROUP_2)        \
    X(GROUP_3)        \
    X(GROUP_4)        \
    X(GROUP_5)        \
    X(ADJACENT)       \
    X(DIAGONAL)       \
    X(LIBERTY)        \
    X(FLAT_CONTACT)   \
    X(WALL_CONTACT)   \
    X(CAP_CONTACT)    \
    X(THREAT_ONE)     \
    X(THREAT_MANY)    \
    X(TEMPO)

#define X(name) T_##name,
typedef enum { EVAL_TERMS(X) TERM_COUNT } EvalTerm;
#undef X

extern const char *const EvalTermNames[TERM_COUNT];

/* 0 at the start of a game, this at the end of it. */
#define PHASE_END 256

typedef struct EvalTrace {
    int counts[TERM_COUNT];
    int phase;
} EvalTrace;

/* In hundredths of a flat, from the side to move's point of view. Must stay well inside
 * SCORE_WIN_BOUND. `threats` is board_road_placements() for the side not to move: every
 * caller needs it for its own reasons, and it is the most expensive input there is. The
 * network plays a size that has one, and the hand-written evaluation any other. */
int evaluate(const Board *b, u64 threats);

/* The network's score with its frame already up to date, for a search that carries frames
 * from node to node. Only for a size nn_ready() has. */
int evaluate_frame(const Board *b, u64 threats, const NNFrame *frame);

/* What the network is scored on besides the stacks, read the same way in play and in a fit. */
void eval_net_inputs(const Board *b, NNInputs *in);

/* The hand-written evaluation, whatever plays this size. */
int evaluate_hce(const Board *b, u64 threats);

/* The hand-written score from the same body, which also fills in *trace. */
int evaluate_trace(const Board *b, u64 threats, EvalTrace *trace);

/* The score a trace adds up to. Selftest holds this equal to evaluate_hce(), which is what
 * makes a fit over traces a fit of the evaluation that plays. */
int eval_from_trace(const EvalTrace *trace);

/* The weights as built, for a fit to start from. */
void eval_weights(double *opening, double *ending);

#endif
