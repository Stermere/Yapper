/* nn.h - the network: a position's stacks summed into hidden units, and those units scored.
 * docs/ARCHITECTURE.md says why it has this shape. Every size uses the same code, each with
 * weights of its own, and a size with none is played by the hand-written evaluation. */
#ifndef NN_H
#define NN_H

#include "board.h"

/* A hidden unit is one of the filters seen through one of the board's eight symmetries, and
 * the output weighs all eight alike, so turning the board cannot change the score. */
#define NN_FILTERS      32
#define NN_ORIENTATIONS 8
#define NN_HIDDEN       (NN_FILTERS * NN_ORIENTATIONS)

/* An occupied square is one token: its top stone's type and the colours of its top six stones,
 * which are the low bits of the stack's word. The colours are folded so the top stone is white,
 * and a black top subtracts its row instead of adding it, so one accumulator serves both sides.
 * 96 per type: 63 stacks of six stones or fewer, 32 taller ones, and one never used. */
#define NN_WINDOW  6
#define NN_TOKENS  (3 * 96)
#define NN_BUCKETS 8

/* Fixed point. An activation of 1 is NN_QA and an output weight of 1 is NN_QB, which keeps
 * an activation times a weight inside 16 bits. An output of 1 is NN_SCALE hundredths of a
 * flat, the scale the hand-written evaluation is fitted at, so the search's margins hold. */
#define NN_QA    255
#define NN_QB    64
#define NN_SCALE 310

/* Weights as a trainer writes them: each filter once, before the symmetries are applied. */
typedef struct NNWeights {
    const i16 *filters; /* [square][token][filter], squares rank by rank on the real board */
    const i16 *bias;    /* [filter] */
    const i16 *output;  /* [bucket][filter] */
    const i32 *skip;    /* [bucket][4]: lead, reserve, komi and a constant, in 64ths */
} NNWeights;

/* The accumulator of one node, from white's side, and the tokens it was summed from. */
typedef struct NNFrame {
    _Alignas(64) i16 acc[NN_HIDDEN];
    i16 feature[SQUARE_COUNT]; /* 0 for empty, else token + 1, negated for a black top */
    bool computed;
} NNFrame;

/* What the network is scored on besides the stacks, all from the side to move's view. */
typedef struct NNInputs {
    int stm;
    int bucket;  /* by stones left, as the hand-written LEAD terms are */
    int lead;    /* flats ahead in half flats, komi included */
    int reserve; /* stones in hand ahead */
    int komi;    /* in half flats */
} NNInputs;

/* Builds the networks compiled into the binary. Must run once, after board_init(). */
void nn_init(void);

/* Replaces the network for a size, or removes it with NULL. False if out of memory. */
bool nn_install(int size, const NNWeights *weights);

/* The weights compiled in for a size, or NULL. */
const NNWeights *nn_builtin(int size);

bool nn_ready(int size);

/* A square's token as stored in a frame. */
int nn_feature(const Board *b, int sq);

void nn_refresh(const Board *b, NNFrame *frame);

/* `frame` for `b`, from `from`, the frame of an ancestor, where every square a move has
 * changed since is in `touched`. Must equal nn_refresh() bit for bit: selftest holds it. */
void nn_derive(const Board *b, NNFrame *frame, const NNFrame *from, u64 touched);

/* The score in 64ths of a hundredth of a flat, for the side to move. */
i64 nn_score(int size, const NNFrame *frame, const NNInputs *in);

/* The squares a move changes. */
static inline u64 nn_move_squares(Move m) {
    if (m == MOVE_NONE) {
        return 0;
    }
    int sq   = move_square(m);
    u64 bits = 1ULL << sq;
    if (!move_is_place(m)) {
        const int step = DirectionStep[move_direction(m)];
        for (int hops = __builtin_popcount((unsigned)move_pattern(m)); hops > 0; hops--) {
            sq += step;
            bits |= 1ULL << sq;
        }
    }
    return bits;
}

#endif
