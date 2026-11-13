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
/* C fills a short initialiser with zeros, so weights of another width would build and play. */
#if NET_FILTERS != NN_FILTERS
#error "net.h was fitted for another NN_FILTERS"
#endif
#if !defined(NET_SIDES) || NET_SIDES != NN_SIDES
#error "net.h was fitted for another number of views"
#endif
#if !defined(NET_L2) || NET_L2 != NN_L2 || NET_L3 != NN_L3
#error "net.h was fitted for another head"
#endif
#endif

typedef struct Net {
    /* [square][token][unit]: the token as its owner's view reads it, then as the other view
     * does, a unit within each being orientation * NN_FILTERS + filter. */
    i16 *table;
    void *block;
    _Alignas(64) i16 bias[NN_VIEW];
    _Alignas(64) i16 output[NN_BUCKETS][NN_POOLS];
    i32 skip[NN_BUCKETS][4];
    /* The head's layers by pairs of inputs, [pair][unit][which of the two]: see pair_products(). */
    _Alignas(64) i16 l2_pairs[NN_BUCKETS][NN_POOLS / 2][NN_L2 * 2];
    i16 l2_skip[NN_BUCKETS][NN_L2][3];
    i32 l2_bias[NN_BUCKETS][NN_L2];
    _Alignas(64) i16 l3_pairs[NN_BUCKETS][NN_L2 / 2][NN_L3 * 2];
    i32 l3_bias[NN_BUCKETS][NN_L3];
    _Alignas(64) i16 l4[NN_BUCKETS][NN_L3];
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
            for (int whose = 0; whose < NN_SIDES; whose++) {
                for (int token = 0; token < NN_TOKENS; token++) {
                    memcpy(net->table + ((size_t)sq * NN_TOKENS + token) * NN_HIDDEN +
                               whose * NN_VIEW + g * NN_FILTERS,
                           weights->filters +
                               ((size_t)cell * NN_INPUTS + whose * NN_TOKENS + token) * NN_FILTERS,
                           NN_FILTERS * sizeof(i16));
                }
            }
        }
    }
    for (int g = 0; g < NN_ORIENTATIONS; g++) {
        for (int f = 0; f < NN_FILTERS; f++) {
            net->bias[g * NN_FILTERS + f] = weights->bias[f];
        }
    }
    memcpy(net->output, weights->output, sizeof(net->output));
    memcpy(net->skip, weights->skip, sizeof(net->skip));
    for (int bucket = 0; bucket < NN_BUCKETS; bucket++) {
        for (int unit = 0; unit < NN_L2; unit++) {
            for (int k = 0; k < NN_POOLS; k++) {
                net->l2_pairs[bucket][k / 2][unit * 2 + k % 2] =
                    weights->l2[((size_t)bucket * NN_L2 + unit) * NN_POOLS + k];
            }
        }
        for (int unit = 0; unit < NN_L3; unit++) {
            for (int j = 0; j < NN_L2; j++) {
                net->l3_pairs[bucket][j / 2][unit * 2 + j % 2] =
                    weights->l3[((size_t)bucket * NN_L3 + unit) * NN_L2 + j];
            }
        }
    }
    memcpy(net->l2_skip, weights->l2_skip, sizeof(net->l2_skip));
    memcpy(net->l2_bias, weights->l2_bias, sizeof(net->l2_bias));
    memcpy(net->l3_bias, weights->l3_bias, sizeof(net->l3_bias));
    memcpy(net->l4, weights->l4, sizeof(net->l4));
    Ready[size] = true;
    return true;
}

void nn_init(void) {
#ifdef NET_H
    static NNWeights sizes[MAX_SIZE + 1];
    for (int size = MIN_SIZE; size <= MAX_SIZE; size++) {
        if (NetSizes >> size & 1) {
            sizes[size]   = (NNWeights){NetFilters,
                                        NetBias + size * NN_FILTERS,
                                        NetOutput + size * NN_BUCKETS * NN_POOLS,
                                        NetSkip + size * NN_BUCKETS * 4,
                                        NetL2 + size * NN_BUCKETS * NN_L2 * NN_POOLS,
                                        NetL2Skip + size * NN_BUCKETS * NN_L2 * 3,
                                        NetL2Bias + size * NN_BUCKETS * NN_L2,
                                        NetL3 + size * NN_BUCKETS * NN_L3 * NN_L2,
                                        NetL3Bias + size * NN_BUCKETS * NN_L3,
                                        NetL4 + size * NN_BUCKETS * NN_L3};
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

/* The part of a token's row that view `view` reads: its owner's view reads the first half. */
static inline const i16 *part_of(const i16 *row, int view, int feature) {
    return row + (view ^ (feature < 0)) * NN_VIEW;
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
        const i16 *row     = row_of(net, b->size, sq, feature);
        for (int view = 0; view < NN_SIDES; view++) {
            const i16 *restrict part = part_of(row, view, feature);
            i16 *restrict acc        = frame->acc + view * NN_VIEW;
            for (int i = 0; i < NN_VIEW; i++) {
                acc[i] = (i16)(acc[i] + part[i]);
            }
        }
    }
    frame->computed = true;
}

/* GCC's own vectors, so one source lowers to AVX2, SSE or NEON. A slice must fit in the target's
 * registers with room for a row, and eight 32-byte vectors on SSE take all sixteen and spill. */
#if defined(__AVX2__)
#define NN_VEC_BYTES 32
#else
#define NN_VEC_BYTES 16
#endif
typedef i16 NNVec __attribute__((vector_size(NN_VEC_BYTES), may_alias));

#define NN_SLICE_VECS 8
#define NN_SLICE      (NN_SLICE_VECS * (int)(sizeof(NNVec) / sizeof(i16)))
_Static_assert(NN_VIEW % NN_SLICE == 0, "a view is carried a slice at a time");

/* The rows first, then one pass over the accumulator, a slice at a time held in registers, from
 * the ancestor's straight into this frame. A pass a row reads and writes the whole accumulator
 * once per row, and it is most of a node's evaluation: E43. Adds wrap, so their order cannot
 * change the result. */
void nn_derive(const Board *b, NNFrame *frame, const NNFrame *from, u64 touched) {
    const Net *net = &Nets[b->size];
    memcpy(frame->feature, from->feature, sizeof(frame->feature));
    /* A square takes away at most one token and puts back at most one. */
    const i16 *adds[SQUARE_COUNT], *subs[SQUARE_COUNT];
    int add_feature[SQUARE_COUNT], sub_feature[SQUARE_COUNT];
    int add_count = 0, sub_count = 0;
    while (touched) {
        const int sq  = pop_lsb(&touched);
        const int now = nn_feature(b, sq);
        const int was = frame->feature[sq];
        if (now != was) {
            if (was) {
                sub_feature[sub_count] = was;
                subs[sub_count++]      = row_of(net, b->size, sq, was);
            }
            if (now) {
                add_feature[add_count] = now;
                adds[add_count++]      = row_of(net, b->size, sq, now);
            }
            frame->feature[sq] = (i16)now;
        }
    }
    for (int view = 0; view < NN_SIDES; view++) {
        const i16 *add_parts[SQUARE_COUNT], *sub_parts[SQUARE_COUNT];
        for (int k = 0; k < add_count; k++) {
            add_parts[k] = part_of(adds[k], view, add_feature[k]);
        }
        for (int k = 0; k < sub_count; k++) {
            sub_parts[k] = part_of(subs[k], view, sub_feature[k]);
        }
        for (int at = 0; at < NN_VIEW; at += NN_SLICE) {
            NNVec v[NN_SLICE_VECS];
            const NNVec *in = (const NNVec *)(from->acc + view * NN_VIEW + at);
            for (int j = 0; j < NN_SLICE_VECS; j++) {
                v[j] = in[j];
            }
            for (int k = 0; k < add_count; k++) {
                const NNVec *row = (const NNVec *)(add_parts[k] + at);
                for (int j = 0; j < NN_SLICE_VECS; j++) {
                    v[j] += row[j];
                }
            }
            for (int k = 0; k < sub_count; k++) {
                const NNVec *row = (const NNVec *)(sub_parts[k] + at);
                for (int j = 0; j < NN_SLICE_VECS; j++) {
                    v[j] -= row[j];
                }
            }
            NNVec *out = (NNVec *)(frame->acc + view * NN_VIEW + at);
            for (int j = 0; j < NN_SLICE_VECS; j++) {
                out[j] = v[j];
            }
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

/* Clipped and squared, each square cut by NN_POOL_SHIFT to 16 bits, then pooled over the
 * orientations, so every layer above is 16 by 16 bits into 32. */
#define NN_POOLED_MAX \
    (NN_ORIENTATIONS * ((NN_QA * NN_QA + (1 << (NN_POOL_SHIFT - 1))) >> NN_POOL_SHIFT))
#define NN_Y_SHIFT 5 /* a second-layer unit, NN_QU * NN_Q2 to 1, down to NN_QY */
#define NN_Z_SHIFT 8 /* a third-layer unit, NN_QY * NN_Q3 to 1, down to NN_QY */
_Static_assert(NN_QU << NN_POOL_SHIFT == 1 << 16, "a pooled 1 stands for 2^16 of the squares");
_Static_assert(NN_POOLED_MAX <= 32767, "a pooled filter fits 16 bits");
_Static_assert(((i64)NN_POOLS) * NN_POOLED_MAX * 127 < 1LL << 31, "the straight sum fits 32 bits");
_Static_assert(((i64)NN_POOLS) * NN_POOLED_MAX * NN_L2_LIMIT < (1LL << 31) - (1LL << 28),
               "a second-layer sum fits 32 bits, with room for its skip inputs and bias");
_Static_assert(((i64)NN_L2) * NN_QY * NN_L3_LIMIT < (1LL << 31) - (1LL << 28),
               "a third-layer sum fits 32 bits, with room for its bias");
_Static_assert((NN_QU * NN_Q2) >> NN_Y_SHIFT == NN_QY && (NN_QY * NN_Q3) >> NN_Z_SHIFT == NN_QY,
               "each unit is cut to NN_QY");
_Static_assert(NN_QY *NN_Q4 % (NN_QB * NN_QU) == 0, "the straight sum scales to the head's units");
_Static_assert(((i64)NN_L3) * NN_QY * NN_L4_LIMIT < 1LL << 31, "the head's output fits 32 bits");

typedef u16 NNUVec __attribute__((vector_size(NN_VEC_BYTES), may_alias));
#define NN_LANES ((int)(sizeof(NNVec) / sizeof(i16)))
_Static_assert(NN_FILTERS % NN_LANES == 0, "a view's filters are whole vectors");

/* One view's filters. Each square is cut before it is added, so the sums never leave 16 bits, and
 * in GCC's vectors, since clang widens the plain loop to 32: half the time, E48. Out of line:
 * inlined, clang derives the pools again for every row, E42. */
static __attribute__((noinline)) void pool(const i16 *restrict acc, const i16 *restrict bias,
                                           i16 *restrict pooled) {
    NNUVec sums[NN_FILTERS / NN_LANES];
    memset(sums, 0, sizeof(sums));
    const NNVec zero = {0}, top = zero + NN_QA;
    for (int g = 0; g < NN_ORIENTATIONS; g++) {
        const NNVec *a = (const NNVec *)(acc + g * NN_FILTERS);
        const NNVec *b = (const NNVec *)(bias + g * NN_FILTERS);
        for (int j = 0; j < NN_FILTERS / NN_LANES; j++) {
            NNVec z          = a[j] + b[j];
            z                = z & (NNVec)(z > zero);
            const NNVec over = (NNVec)(z > top);
            z                = (z & ~over) | (top & over);
            const NNUVec x   = (NNUVec)z;
            sums[j] += (x * x + (1 << (NN_POOL_SHIFT - 1))) >> NN_POOL_SHIFT;
        }
    }
    memcpy(pooled, sums, sizeof(sums));
}

static __attribute__((noinline)) i32 dot(const i16 *restrict a, const i16 *restrict b) {
    i32 sum = 0;
    FULL_WIDTH
    for (int k = 0; k < NN_POOLS; k++) {
        sum += (i32)a[k] * b[k];
    }
    return sum;
}

/* A layer of the head is small and dense. A dot product for each unit ends in a sum across a
 * register, which costs more than the products did, and thirty-two of them were most of a score.
 * So a layer's weights are kept by pairs of inputs, and the two activations of a pair, set in every
 * pair of lanes, are multiplied into a register of units at once: adjacent products added, which
 * is one instruction on x86 and two widening multiplies on NEON, written out as what it computes so
 * that a compiler finds whichever it has. The sums are the same integers in another order. */
typedef i32 NNWide __attribute__((vector_size(NN_VEC_BYTES), may_alias));
#define NN_WIDE_LANES ((int)(sizeof(NNWide) / sizeof(i32)))
_Static_assert(NN_L2 % NN_WIDE_LANES == 0 && NN_L3 % NN_WIDE_LANES == 0,
               "a layer's units are whole registers");
_Static_assert(NN_POOLS % 4 == 0 && NN_L2 % 2 == 0, "inputs come in pairs, and pairs in twos");

static inline NNWide pair_products(NNVec weights, NNVec pair) {
#if defined(__AVX2__)
    typedef short Lanes __attribute__((vector_size(32)));
    return (NNWide)__builtin_ia32_pmaddwd256((Lanes)weights, (Lanes)pair);
#elif defined(__SSE2__)
    typedef short Lanes __attribute__((vector_size(16)));
    return (NNWide)__builtin_ia32_pmaddwd128((Lanes)weights, (Lanes)pair);
#elif defined(__clang__) || __GNUC__ >= 12
    /* Even lanes times even, odd times odd, each widened: NEON has that multiply. */
    typedef i16 Half __attribute__((vector_size(NN_VEC_BYTES / 2)));
#if NN_VEC_BYTES == 32
#define NN_EVEN 0, 2, 4, 6, 8, 10, 12, 14
#define NN_ODD  1, 3, 5, 7, 9, 11, 13, 15
#else
#define NN_EVEN 0, 2, 4, 6
#define NN_ODD  1, 3, 5, 7
#endif
    const Half we = __builtin_shufflevector(weights, weights, NN_EVEN);
    const Half wo = __builtin_shufflevector(weights, weights, NN_ODD);
    const Half pe = __builtin_shufflevector(pair, pair, NN_EVEN);
    const Half po = __builtin_shufflevector(pair, pair, NN_ODD);
    return __builtin_convertvector(we, NNWide) * __builtin_convertvector(pe, NNWide) +
           __builtin_convertvector(wo, NNWide) * __builtin_convertvector(po, NNWide);
#else
    NNWide out;
    for (int i = 0; i < NN_WIDE_LANES; i++) {
        out[i] = (i32)weights[2 * i] * pair[2 * i] + (i32)weights[2 * i + 1] * pair[2 * i + 1];
    }
    return out;
#endif
}

/* Two adjacent activations in every pair of lanes. Read through memory, so the lanes come back in
 * the order they lay in whatever the byte order. */
static inline NNVec pair_of(const i16 *at) {
    i32 both;
    memcpy(&both, at, sizeof(both));
    return (NNVec)((NNWide){0} + both);
}

i64 nn_score(int size, const NNFrame *frame, const NNInputs *in) {
    const Net *net   = &Nets[size];
    const int bucket = in->bucket;
    _Alignas(64) i16 pooled[NN_POOLS];
    /* The side to move's view is the half of its own colour, and its pools come first. */
    pool(frame->acc + in->stm * NN_VIEW, net->bias, pooled);
    pool(frame->acc + (in->stm ^ 1) * NN_VIEW, net->bias, pooled + NN_FILTERS);

    const i32 linear    = dot(net->output[bucket], pooled);
    const i32 inputs[3] = {in->lead * (NN_QU / NN_LEAD_UNIT),
                           in->reserve * (NN_QU / NN_RESERVE_UNIT),
                           in->komi * (NN_QU / NN_KOMI_UNIT)};
    /* The second layer, a pair of pools at a time into all its units. Two sets of sums, turn
     * about, so that one add does not wait for the last. */
    NNWide even[NN_L2 / NN_WIDE_LANES] = {0}, odd[NN_L2 / NN_WIDE_LANES] = {0};
    for (int p = 0; p < NN_POOLS / 2; p += 2) {
        const NNVec a = pair_of(pooled + 2 * p), c = pair_of(pooled + 2 * p + 2);
        const NNVec *rows  = (const NNVec *)net->l2_pairs[bucket][p];
        const NNVec *rows2 = (const NNVec *)net->l2_pairs[bucket][p + 1];
        for (int j = 0; j < NN_L2 / NN_WIDE_LANES; j++) {
            even[j] += pair_products(rows[j], a);
            odd[j] += pair_products(rows2[j], c);
        }
    }
    i32 sums[NN_L2];
    for (int j = 0; j < NN_L2 / NN_WIDE_LANES; j++) {
        even[j] += odd[j];
    }
    memcpy(sums, even, sizeof(sums));
    _Alignas(32) i16 y[NN_L2];
    for (int j = 0; j < NN_L2; j++) {
        const i16 *ws = net->l2_skip[bucket][j];
        i32 v         = net->l2_bias[bucket][j] + ws[0] * inputs[0] + ws[1] * inputs[1] +
                ws[2] * inputs[2] + sums[j];
        v    = v < 0 ? 0 : v > NN_QU * NN_Q2 ? NN_QU * NN_Q2 : v;
        y[j] = (i16)((v + (1 << (NN_Y_SHIFT - 1))) >> NN_Y_SHIFT);
    }
    /* The third, the same way from the second's units. */
    NNWide units[NN_L3 / NN_WIDE_LANES];
    memcpy(units, net->l3_bias[bucket], sizeof(units));
    for (int p = 0; p < NN_L2 / 2; p++) {
        const NNVec both  = pair_of(y + 2 * p);
        const NNVec *rows = (const NNVec *)net->l3_pairs[bucket][p];
        for (int j = 0; j < NN_L3 / NN_WIDE_LANES; j++) {
            units[j] += pair_products(rows[j], both);
        }
    }
    i32 z[NN_L3];
    memcpy(z, units, sizeof(z));
    i64 head = 0;
    for (int k = 0; k < NN_L3; k++) {
        i32 v = z[k];
        v     = v < 0 ? 0 : v > NN_QY * NN_Q3 ? NN_QY * NN_Q3 : v;
        head += (i64)net->l4[bucket][k] * ((v + (1 << (NN_Z_SHIFT - 1))) >> NN_Z_SHIFT);
    }

    const i64 whole = (i64)linear * (NN_QY * NN_Q4 / (NN_QB * NN_QU)) + head;
    const i32 *skip = net->skip[bucket];
    return whole * NN_SCALE * 64 / ((i64)NN_QY * NN_Q4) + (i64)skip[0] * in->lead +
           (i64)skip[1] * in->reserve + (i64)skip[2] * in->komi + skip[3];
}
