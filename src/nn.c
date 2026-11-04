/* nn.c - the network. Weights arrive once per filter and are spread over the eight symmetries
 * here, so the table a search reads has a row per square and token, and a node costs only the
 * rows its moves changed. Everything is integer, so every build scores alike. */
#include "nn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bitboard.h"

/* A size has a network when net.h was fitted on its games. `yapper nntrain` writes it. */
#if __has_include("net.h")
#include "net.h"
#endif

typedef struct Net {
    i16 *table; /* [square][token][unit], a unit being orientation * NN_FILTERS + filter */
    void *block;
    _Alignas(64) i16 bias[NN_HIDDEN];
    _Alignas(64) i16 output[NN_BUCKETS][NN_HIDDEN];
    i32 skip[NN_BUCKETS][4];
} Net;

/* Static, not allocated, so that the alignment above holds. */
static Net Nets[MAX_SIZE + 1];
static bool Ready[MAX_SIZE + 1];
static const NNWeights *Builtin[MAX_SIZE + 1];

/* Line `x` of a board of `n` on the grid: the near half counted from one edge and the rest from
 * the other. */
static int on_grid(int x, int n) { return x < (n + 1) / 2 ? x : NN_GRID - n + x; }

/* Selftest's eight transforms, though any eight that make up the group would serve. */
int nn_cell(int size, int g, int sq) {
    int rank = sq / size, file = sq % size;
    rank = g & 1 ? size - 1 - rank : rank;
    file = g & 2 ? size - 1 - file : file;
    if (g & 4) {
        const int t = rank;
        rank        = file;
        file        = t;
    }
    return on_grid(rank, size) * NN_GRID + on_grid(file, size);
}

bool nn_install(int size, const NNWeights *weights) {
    Net *net    = &Nets[size];
    Ready[size] = false;
    free(net->block);
    net->block = NULL;
    if (!weights) {
        return true;
    }
    const int squares  = size * size;
    const size_t cells = (size_t)squares * NN_TOKENS * NN_HIDDEN;
    net->block         = malloc(cells * sizeof(i16) + 64);
    if (!net->block) {
        return false;
    }
    net->table = (i16 *)(((uintptr_t)net->block + 63) & ~(uintptr_t)63);

    /* Unit (filter, g) sees square s through filter's weights at the cell of g(s). Turning the
     * position by any symmetry then only permutes the units of each filter among themselves. */
    for (int sq = 0; sq < squares; sq++) {
        for (int g = 0; g < NN_ORIENTATIONS; g++) {
            const int cell = nn_cell(size, g, sq);
            for (int token = 0; token < NN_TOKENS; token++) {
                memcpy(net->table + ((size_t)sq * NN_TOKENS + token) * NN_HIDDEN + g * NN_FILTERS,
                       weights->filters + ((size_t)cell * NN_TOKENS + token) * NN_FILTERS,
                       NN_FILTERS * sizeof(i16));
            }
        }
    }
    for (int g = 0; g < NN_ORIENTATIONS; g++) {
        for (int f = 0; f < NN_FILTERS; f++) {
            net->bias[g * NN_FILTERS + f] = weights->bias[f];
            for (int bucket = 0; bucket < NN_BUCKETS; bucket++) {
                net->output[bucket][g * NN_FILTERS + f] = weights->output[bucket * NN_FILTERS + f];
            }
        }
    }
    memcpy(net->skip, weights->skip, sizeof(net->skip));
    Ready[size] = true;
    return true;
}

void nn_init(void) {
#ifdef NET_H
    static NNWeights sizes[MAX_SIZE + 1];
    for (int size = MIN_SIZE; size <= MAX_SIZE; size++) {
        if (NetSizes >> size & 1) {
            sizes[size]   = (NNWeights){NetFilters, NetBias + size * NN_FILTERS,
                                        NetOutput + size * NN_BUCKETS * NN_FILTERS,
                                        NetSkip + size * NN_BUCKETS * 4};
            Builtin[size] = &sizes[size];
        }
    }
#endif
    for (int size = MIN_SIZE; size <= MAX_SIZE; size++) {
        if (Builtin[size] && !nn_install(size, Builtin[size])) {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
    }
}

const NNWeights *nn_builtin(int size) { return Builtin[size]; }

bool nn_ready(int size) { return Ready[size]; }

int nn_feature(const Board *b, int sq) {
    const u64 word = b->stones[sq];
    if (word == 1) {
        return 0;
    }
    const int height = 63 - __builtin_clzll(word);
    const u64 black  = word & 1;
    int stack;
    if (height <= NN_WINDOW) {
        /* The word itself, its colours turned over under a black top. The 1 above the bottom
         * stone stays put, so every height has a range of its own. */
        stack = (int)((word ^ (-black & ((1ULL << height) - 1))) >> 1);
    } else {
        stack = 64 + (int)(((word ^ -black) & 63) >> 1);
    }
    const int token = (int)board_top(b, sq) * 96 + stack;
    return black ? -(token + 1) : token + 1;
}

static inline const i16 *row_of(const Net *net, int size, int sq, int feature) {
    const int token  = (feature < 0 ? -feature : feature) - 1;
    const int square = (sq >> 3) * size + (sq & 7);
    return net->table + ((size_t)square * NN_TOKENS + token) * NN_HIDDEN;
}

static inline void row_add(i16 *restrict acc, const i16 *restrict row) {
    for (int i = 0; i < NN_HIDDEN; i++) {
        acc[i] = (i16)(acc[i] + row[i]);
    }
}

static inline void row_sub(i16 *restrict acc, const i16 *restrict row) {
    for (int i = 0; i < NN_HIDDEN; i++) {
        acc[i] = (i16)(acc[i] - row[i]);
    }
}

/* A white top adds its row and a black one subtracts it; taking a token away does the reverse. */
static inline void put(const Net *net, int size, i16 *acc, int sq, int feature, bool remove) {
    const i16 *row = row_of(net, size, sq, feature);
    if ((feature > 0) != remove) {
        row_add(acc, row);
    } else {
        row_sub(acc, row);
    }
}

void nn_refresh(const Board *b, NNFrame *frame) {
    const Net *net = &Nets[b->size];
    memset(frame->acc, 0, sizeof(frame->acc));
    memset(frame->feature, 0, sizeof(frame->feature));
    u64 occupied = b->owned[WHITE] | b->owned[BLACK];
    while (occupied) {
        const int sq       = pop_lsb(&occupied);
        const int feature  = nn_feature(b, sq);
        frame->feature[sq] = (i16)feature;
        put(net, b->size, frame->acc, sq, feature, false);
    }
    frame->computed = true;
}

void nn_derive(const Board *b, NNFrame *frame, const NNFrame *from, u64 touched) {
    const Net *net = &Nets[b->size];
    memcpy(frame->acc, from->acc, sizeof(frame->acc));
    memcpy(frame->feature, from->feature, sizeof(frame->feature));
    while (touched) {
        const int sq  = pop_lsb(&touched);
        const int now = nn_feature(b, sq);
        const int was = frame->feature[sq];
        if (now != was) {
            if (was) {
                put(net, b->size, frame->acc, sq, was, true);
            }
            if (now) {
                put(net, b->size, frame->acc, sq, now, false);
            }
            frame->feature[sq] = (i16)now;
        }
    }
    frame->computed = true;
}

/* clang sizes this loop's vectors by the 32-bit sum, which halves them, unless told otherwise.
 * gcc gets the full width by itself, and a pragma only clang knows would warn there. */
#if defined(__clang__)
#define FULL_WIDTH _Pragma("clang loop vectorize_width(16) interleave_count(1)")
#else
#define FULL_WIDTH
#endif

/* Clipped and squared. An activation times its weight fits 16 bits, and 256 of those times the
 * activation again fit 32, which is what NN_QA and NN_QB were chosen for. */
i64 nn_score(int size, const NNFrame *frame, const NNInputs *in) {
    const Net *net             = &Nets[size];
    const i16 *restrict acc    = frame->acc;
    const i16 *restrict bias   = net->bias;
    const i16 *restrict weight = net->output[in->bucket];
    i32 sum                    = 0;
    if (in->stm == WHITE) {
        FULL_WIDTH
        for (int i = 0; i < NN_HIDDEN; i++) {
            const i16 z = (i16)(acc[i] + bias[i]);
            const i16 x = z < 0 ? 0 : z > NN_QA ? NN_QA : z;
            sum += (i32)(i16)(x * weight[i]) * x;
        }
    } else {
        FULL_WIDTH
        for (int i = 0; i < NN_HIDDEN; i++) {
            const i16 z = (i16)(bias[i] - acc[i]);
            const i16 x = z < 0 ? 0 : z > NN_QA ? NN_QA : z;
            sum += (i32)(i16)(x * weight[i]) * x;
        }
    }
    const i32 *skip = net->skip[in->bucket];
    return (i64)sum * NN_SCALE * 64 / ((i64)NN_QA * NN_QA * NN_QB) + (i64)skip[0] * in->lead +
           (i64)skip[1] * in->reserve + (i64)skip[2] * in->komi + skip[3];
}
