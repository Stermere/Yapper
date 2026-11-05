/* nn.h - the network: a position's stacks summed into hidden units, and those units scored.
 * docs/ARCHITECTURE.md says why it has this shape. One network plays every size: the filters are
 * shared, and each size has a bias, output and skip of its own. */
#ifndef NN_H
#define NN_H

#include "board.h"

/* A hidden unit is one of the filters seen through one of the board's eight symmetries, from one
 * side's view, and the output weighs all eight alike, so turning the board cannot change the
 * score. Each view has weights for a stack of its own and for one of the other side's, so a unit
 * can weigh what both sides have, where one view whose rows the other side subtracts could only
 * weigh the difference: E43. White's view and black's are the same weights, so swapping the
 * colours and the side to move swaps the views and cannot change the score either. */
#define NN_FILTERS      64
#define NN_ORIENTATIONS 8
#define NN_SIDES        2
#define NN_VIEW         (NN_FILTERS * NN_ORIENTATIONS)
#define NN_HIDDEN       (NN_SIDES * NN_VIEW)

/* An occupied square is one token: its top stone's type and the colours of its top six stones,
 * which are the low bits of the stack's word. The colours are folded so the top stone is its
 * owner's, and a view reads the token as its own or the other side's.
 * 96 per type: 63 stacks of six stones or fewer, 32 taller ones, and one never used. */
#define NN_WINDOW 6
#define NN_TOKENS (3 * 96)
#define NN_INPUTS                                                                  \
    (NN_SIDES * NN_TOKENS) /* a cell's rows: the viewer's tokens, then the other's \
                            */
#define NN_BUCKETS 8

/* Fixed point. An activation of 1 is NN_QA and an output weight of 1 is NN_QB, which keeps
 * an activation times a weight inside 16 bits. An output of 1 is NN_SCALE hundredths of a
 * flat, which keeps a flat near 100: the search's margins are written in those units. */
#define NN_QA    255
#define NN_QB    64
#define NN_SCALE 310

/* A head reads each filter's units pooled over the eight orientations, in both views, with the
 * skip inputs, through NN_L2 and then NN_L3 clipped units, and the output adds them to the
 * straight sum of the pools. Pooling first keeps the head blind to how the board is turned. */
#define NN_POOLS (NN_SIDES * NN_FILTERS)
#define NN_L2    16
#define NN_L3    32

/* The head's fixed point. A filter's squares pooled over the orientations are cut by
 * NN_POOL_SHIFT to fit 16 bits, after which a pooled 1 is NN_QU and stands for QA * QA / 2^16 of
 * the squares, and the fit reads them so. A weight of 1 is NN_Q2, NN_Q3 and NN_Q4 by layer, and a
 * unit of the head is cut back to 16 bits, where 1 is NN_QY, before the next layer reads it. */
#define NN_POOL_SHIFT 5
#define NN_QU         2048
#define NN_Q2         256
#define NN_Q3         256
#define NN_Q4         256
#define NN_QY         16384

/* The largest weight a trainer may write in each layer, which keeps every sum in 32 bits. */
#define NN_L2_LIMIT 384
#define NN_L3_LIMIT 512
#define NN_L4_LIMIT 1024

/* The skip inputs in the units both the fit and the head read them in. */
#define NN_LEAD_UNIT    16
#define NN_RESERVE_UNIT 16
#define NN_KOMI_UNIT    4

/* A filter is laid on a grid of NN_GRID by NN_GRID. A board sits on it with its edges on the
 * grid's edges, so a corner, an edge and the line beside it are the same weights on every size,
 * and only the middle differs. */
#define NN_GRID MAX_SIZE

/* Weights as a trainer writes them for one size: each filter once, before the symmetries are
 * applied, and that size's own bias, output, skip and head. A pool is [view][filter], the side to
 * move's view first. */
typedef struct NNWeights {
    const i16 *filters; /* [cell][input][filter], cells rank by rank on the grid */
    const i16 *bias;    /* [filter], the same in both views */
    const i16 *output;  /* [bucket][pool] */
    const i32 *skip;    /* [bucket][4]: lead, reserve, komi and a constant, in 64ths */
    const i16 *l2;      /* [bucket][unit][pool] */
    const i16 *l2_skip; /* [bucket][unit][3]: lead, reserve and komi */
    const i32 *l2_bias; /* [bucket][unit], in NN_QU * NN_Q2 */
    const i16 *l3;      /* [bucket][unit][unit of the layer before] */
    const i32 *l3_bias; /* [bucket][unit], in NN_QY * NN_Q3 */
    const i16 *l4;      /* [bucket][unit of the layer before] */
} NNWeights;

/* The accumulator of one node, white's view and then black's, and the tokens it was summed from. */
typedef struct NNFrame {
    _Alignas(64) i16 acc[NN_HIDDEN];
    i16 feature[SQUARE_COUNT]; /* 0 for empty, else token + 1, negated for a black top */
    bool computed;
} NNFrame;

/* What the network is scored on besides the stacks, all from the side to move's view. */
typedef struct NNInputs {
    int stm;
    int bucket;  /* by stones left */
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

/* The cell of the grid whose weights square `sq` of a board of `size` reads under symmetry `g`,
 * the square counted rank by rank on that board. The trainer uses it too. */
int nn_cell(int size, int g, int sq);

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
