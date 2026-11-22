/* policy.c - a prior over the moves of a position, from what each move does in Tak's own terms.
 *
 * Every feature reads the board the same way under its eight symmetries and from either side,
 * so the prior cannot tell a turned or recoloured position from the original. A move's features
 * index rows of 16 lanes, which are summed onto the units' biases; 15 lanes are clipped units
 * weighed by the output, and the last is a linear term. */
#include "policy.h"

#include "bitboard.h"
#include "movegen.h"

#if __has_include("policy_net.h")
#include "policy_net.h"
#endif

/* A neighbour of a placement, as one of these: empty, then each side's flat, wall, capstone and
 * stack of two or more under a flat, the side to move's first. */
#define NEIGHBOUR_CODES 9

enum {
    /* A placement, by its stone. Phase and race are the node's, and summed once a stone. */
    P_PHASE       = 0,                                      /* type, phase */
    P_RACE        = P_PHASE + 3 * 4,                        /* type, race */
    P_NEIGHBOURS  = P_RACE + 3 * 4,                         /* type, code, how many 1-4 */
    P_ROAD        = P_NEIGHBOURS + 3 * NEIGHBOUR_CODES * 4, /* type, phase, five road flags */
    P_DIST_OURS   = P_ROAD + 3 * 4 * 32,                    /* type, rel 0-3, our distance */
    P_DIST_THEIRS = P_DIST_OURS + 3 * 4 * 4,                /* type, rel 0-3, their distance */
    P_PREV        = P_DIST_THEIRS + 3 * 4 * 4,              /* type, last move, ours, its kind */
    /* A spread's source alone. */
    S_PHASE  = P_PREV + 3 * 4 * 4 * 5,
    S_TOP    = S_PHASE + 4, /* flat, wall or capstone */
    S_RACE   = S_TOP + 3,
    S_HEIGHT = S_RACE + 4,   /* 1, 2, 3-4, 5+ */
    S_PREV   = S_HEIGHT + 4, /* last move, ours, its kind */
    /* Its source and carry. */
    S_CARRY = S_PREV + 4 * 4 * 5, /* 1 to 8 */
    S_WHOLE = S_CARRY + 8,        /* carries the whole stack */
    S_LEFT  = S_WHOLE + 1,        /* leaves the source empty, ours, theirs */
    S_OWN   = S_LEFT + 3,         /* our stones carried: all, most, some, none */
    /* Where it lands, the same for every spread of a stack the same way as far. */
    S_TAKES     = S_OWN + 4,          /* tallest of theirs covered 0-3+, a crush */
    S_NEAR      = S_TAKES + 4 * 2,    /* landings on our near squares 0-2+, theirs, a road square */
    S_COUNTS    = S_NEAR + 3 * 3 * 2, /* landings on empties 0-3+, ours 0-2+, theirs 0-2+ */
    S_LAND_PREV = S_COUNTS + 4 * 3 * 3, /* last move's kind, lands on/beside it, our last */
    /* How it drops. */
    S_SWING     = S_LAND_PREV + 5 * 4 * 4, /* tops won -4 to 4, phase */
    S_HOPS      = S_SWING + 9 * 4,         /* hops 1-7, stones dropped last 1-3+ */
    S_DIST_OURS = S_HOPS + 7 * 3,      /* best rel of our new tops on empties 0-4, our distance */
    S_BREAK     = S_DIST_OURS + 5 * 4, /* covers their road stone at rel 0/1, their distance */
    S_BLOCK     = S_BREAK + 2 * 4,     /* our new top on an empty at their rel 0-2 */
    S_GIVE      = S_BLOCK + 3 * 4,     /* their new top at our rel 0/1, our distance */
    F_COUNT     = S_GIVE + 2 * 4,
};

int policy_feature_count(void) { return F_COUNT; }

/* GCC's vectors, so one source lowers to AVX2, SSE or NEON. Unaligned, because Windows does not
 * keep the stack 32-byte aligned for every compiler. */
typedef i16 PolicyVec __attribute__((vector_size(32), aligned(2), may_alias));
typedef i16 PolicyHalf __attribute__((vector_size(16), aligned(2), may_alias));
typedef i32 PolicyWide __attribute__((vector_size(32), aligned(4), may_alias));

static const i16 *Rows;
static i16 Bias[POLICY_LANES], Out[POLICY_LANES];

void policy_install(const i16 *rows, const i16 *bias, const i16 *out) {
    if (!rows) {
#if defined(POLICY_NET_FEATURES) && defined(POLICY_NET_LANES)
        if (POLICY_NET_FEATURES == F_COUNT && POLICY_NET_LANES == POLICY_LANES) {
            rows = PolicyNetRows;
            bias = PolicyNetBias;
            out  = PolicyNetOut;
        }
#endif
    }
    Rows = rows;
    for (int i = 0; i < POLICY_LANES; i++) {
        Bias[i] = rows ? bias[i] : 0;
        Out[i]  = rows ? out[i] : 0;
    }
}

static inline const i16 *rows_now(void) {
    static bool loaded;
    if (!loaded) {
        loaded = true;
        if (!Rows) {
            policy_install(NULL, NULL, NULL);
        }
    }
    return Rows;
}

/* Both sides' four edges at once, a lane each: the floods run in step, so a node pays for the
 * longest rather than for all eight. */
typedef u64 ReachVec __attribute__((vector_size(64), aligned(8), may_alias));

static inline ReachVec reach_neighbours(ReachVec x) {
    const ReachVec not_h = (ReachVec){0} + ~FILE_H, not_a = (ReachVec){0} + ~FILE_A;
    return x << 8 | x >> 8 | (x & not_h) << 1 | (x & not_a) >> 1;
}

static inline bool reach_any(ReachVec x) {
    return (x[0] | x[1] | x[2] | x[3] | x[4] | x[5] | x[6] | x[7]) != 0;
}

/* [side][edge][k]: what each side reaches from each edge through its own road stones with at
 * most k placements. Each level floods only from what it added. */
static void reach_both(const Board *b, int us, u64 out[2][4][POLICY_LEVELS + 1]) {
    const u64 mine = board_road_stones(b, us), theirs = board_road_stones(b, !us);
    const u64 w = FILE_A & b->mask, e = b->file_last, s = RANK_1 & b->mask, n = b->rank_last;
    const ReachVec edges = {w, e, s, n, w, e, s, n};
    const ReachVec own   = {mine, mine, mine, mine, theirs, theirs, theirs, theirs};
    const ReachVec empty = (ReachVec){0} + board_empty(b);
    ReachVec r           = edges & own;
    ReachVec frontier    = r;
    for (int k = 0;; k++) {
        while (reach_any(frontier)) {
            frontier = reach_neighbours(frontier) & own & ~r;
            r |= frontier;
        }
        for (int lane = 0; lane < 8; lane++) {
            out[lane >> 2][lane & 3][k] = r[lane];
        }
        if (k == POLICY_LEVELS) {
            return;
        }
        frontier = (reach_neighbours(r) | edges) & empty & ~r;
        r |= frontier;
    }
}

/* The fewest placements that finish a road through some empty square, as far as the levels go. */
static int best_through(const u64 reach[4][POLICY_LEVELS + 1], u64 empty) {
    for (int t = 1; t <= 2 * POLICY_LEVELS - 1; t++) {
        for (int i = 1; i <= POLICY_LEVELS; i++) {
            const int j = t + 1 - i;
            if (j >= 1 && j <= POLICY_LEVELS &&
                ((reach[0][i] & reach[1][j] & empty) || (reach[2][i] & reach[3][j] & empty))) {
                return t;
            }
        }
    }
    return 2 * POLICY_LEVELS;
}

/* Squares whose best road through them is within 0, 1 and 2 placements of the side's best: an
 * empty square counts itself, a stone of the side's own does not. */
static void rel_sets(const u64 reach[4][POLICY_LEVELS + 1], int best, u64 empty, u64 own,
                     u64 rel_e[3], u64 rel_o[3]) {
    for (int r = 0; r < 3; r++) {
        const int t = best + r;
        u64 e = 0, o = 0;
        for (int i = 1; i <= POLICY_LEVELS && t + 1 - i >= 1; i++) {
            const int j = t + 1 - i > POLICY_LEVELS ? POLICY_LEVELS : t + 1 - i;
            e |= (reach[0][i] & reach[1][j]) | (reach[2][i] & reach[3][j]);
        }
        for (int i = 0; i <= POLICY_LEVELS && t - i >= 0; i++) {
            const int j = t - i > POLICY_LEVELS ? POLICY_LEVELS : t - i;
            o |= (reach[0][i] & reach[1][j]) | (reach[2][i] & reach[3][j]);
        }
        rel_e[r] = e & empty;
        rel_o[r] = o & own;
    }
}

static inline int rel_at(const u64 rel[3], int sq) {
    return 3 - (int)((rel[0] >> sq) & 1) - (int)((rel[1] >> sq) & 1) - (int)((rel[2] >> sq) & 1);
}

/* The lowest rel among squares, 0-3, or 4 for none. */
static inline int rel_min(const u64 rel[3], u64 squares) {
    return !squares ? 4 : squares & rel[0] ? 0 : squares & rel[1] ? 1 : squares & rel[2] ? 2 : 3;
}

static inline int dist_bucket(int d) { return d <= 1 ? 0 : d >= 4 ? 3 : d - 1; }

/* The squares a move touched, those beside them and those two away. */
static void touched(const Board *b, Move m, u64 rings[3]) {
    const u64 bits = move_squares(m);
    rings[0]       = bits;
    rings[1]       = neighbours(bits) & b->mask & ~bits;
    rings[2]       = neighbours(rings[1]) & b->mask & ~bits & ~rings[1];
}

static inline int ring_index(const u64 rings[3], u64 bit) {
    return rings[0] & bit ? 0 : rings[1] & bit ? 1 : rings[2] & bit ? 2 : 3;
}

static inline PolicyVec add_rows(PolicyVec acc, const u16 *features, int n) {
    for (int i = 0; i < n; i++) {
        acc += *(const PolicyVec *)(Rows + (size_t)features[i] * POLICY_LANES);
    }
    return acc;
}

void policy_context(const Board *b, const u64 ends[4], const u64 their_ends[4], Move prev,
                    Move prev2, PolicyContext *ctx) {
    const int us      = board_stm(b);
    const u64 empty   = board_empty(b);
    ctx->us           = us;
    ctx->near_road[0] = board_road_near(ends, empty);
    ctx->near_road[1] = board_road_near(their_ends, empty);
    ctx->threats      = board_road_gaps(their_ends) & empty;
    ctx->ends[0]      = (ends[0] | ends[1] | ends[2] | ends[3]) & b->mask;
    ctx->ends[1]      = (their_ends[0] | their_ends[1] | their_ends[2] | their_ends[3]) & b->mask;

    const int start = board_start_flats(b->size) + board_start_capstones(b->size);
    const int ours  = b->flats[us] + b->capstones[us];
    const int other = b->flats[!us] + b->capstones[!us];
    const int left  = ours < other ? ours : other;
    const int part  = 4 * left / start;
    ctx->phase      = part >= 3 ? 0 : 3 - part;
    if (left <= 6 || popcount(empty) <= 6) {
        const int lead = board_flat_lead(b, us);
        ctx->race      = lead < 0 ? 0 : lead == 0 ? 1 : 2;
    } else {
        ctx->race = 3;
    }

    u64 reach[2][4][POLICY_LEVELS + 1];
    reach_both(b, us, reach);
    for (int side = 0; side < 2; side++) {
        ctx->dist[side] = best_through(reach[side], empty);
        rel_sets(reach[side], ctx->dist[side], empty, board_road_stones(b, side ? !us : us),
                 ctx->rel_empty[side], ctx->rel_own[side]);
    }
    touched(b, prev, ctx->prev);
    touched(b, prev2, ctx->prev2);
    ctx->prev_kind = prev == MOVE_NONE ? 4 : move_is_place(prev) ? (int)move_stone(prev) : 3;
}

static inline int neighbour_code(const Board *b, int sq, int us) {
    const int side = (b->owned[us] >> sq) & 1 ? 0 : (b->owned[!us] >> sq) & 1 ? 1 : -1;
    if (side < 0) {
        return 0;
    }
    const StoneType top = board_top(b, sq);
    const int kind      = top == FLAT && ((b->tall >> sq) & 1) ? 3 : (int)top;
    return 1 + side * 4 + kind;
}

/* What a placement's features read off its square, the same for every stone placed there. */
typedef struct PlaceSquare {
    u8 codes, code[4], count[4]; /* the kinds of neighbour present, in code order, and how many */
    u8 road;                     /* near ours, near theirs, a road square, ends ours, theirs */
    u8 rel_ours, rel_theirs, prev, prev2;
} PlaceSquare;

static void place_square(const Board *b, const PolicyContext *ctx, int sq, PlaceSquare *p) {
    const u64 bit  = 1ULL << sq;
    const int file = sq & 7, rank = sq >> 3, last = b->size - 1;
    int counts[NEIGHBOUR_CODES] = {0};
    if (rank < last) {
        counts[neighbour_code(b, sq + 8, ctx->us)]++;
    }
    if (rank > 0) {
        counts[neighbour_code(b, sq - 8, ctx->us)]++;
    }
    if (file < last) {
        counts[neighbour_code(b, sq + 1, ctx->us)]++;
    }
    if (file > 0) {
        counts[neighbour_code(b, sq - 1, ctx->us)]++;
    }
    p->codes = 0;
    for (int code = 0; code < NEIGHBOUR_CODES; code++) {
        if (counts[code]) {
            p->code[p->codes]    = (u8)code;
            p->count[p->codes++] = (u8)counts[code];
        }
    }
    p->road       = (u8)(((ctx->near_road[0] & bit) != 0) | ((ctx->near_road[1] & bit) != 0) << 1 |
                   ((ctx->threats & bit) != 0) << 2 | ((ctx->ends[0] & bit) != 0) << 3 |
                   ((ctx->ends[1] & bit) != 0) << 4);
    p->rel_ours   = (u8)rel_at(ctx->rel_empty[0], sq);
    p->rel_theirs = (u8)rel_at(ctx->rel_empty[1], sq);
    p->prev       = (u8)ring_index(ctx->prev, bit);
    p->prev2      = (u8)ring_index(ctx->prev2, bit);
}

static int place_emit(const PolicyContext *ctx, const PlaceSquare *p, int type, u16 *out) {
    int n    = 0;
    out[n++] = (u16)(P_PHASE + type * 4 + ctx->phase);
    out[n++] = (u16)(P_RACE + type * 4 + ctx->race);
    for (int k = 0; k < p->codes; k++) {
        out[n++] =
            (u16)(P_NEIGHBOURS + (type * NEIGHBOUR_CODES + p->code[k]) * 4 + p->count[k] - 1);
    }
    out[n++] = (u16)(P_ROAD + (type * 4 + ctx->phase) * 32 + p->road);
    out[n++] = (u16)(P_DIST_OURS + (type * 4 + p->rel_ours) * 4 + dist_bucket(ctx->dist[0]));
    out[n++] = (u16)(P_DIST_THEIRS + (type * 4 + p->rel_theirs) * 4 + dist_bucket(ctx->dist[1]));
    if (p->prev < 3 || p->prev2 < 3) {
        out[n++] = (u16)(P_PREV + ((type * 4 + p->prev) * 4 + p->prev2) * 5 + ctx->prev_kind);
    }
    return n;
}

/* A spread's features in four parts, by what they depend on: its source; its source and carry;
 * its source, direction and hops, which fix where it lands; and the stones it drops where. `need`
 * says which to write, a bit each in that order, into src, carry, land and drop. */
typedef struct SpreadParts {
    u16 src[8], carry[4], land[8], drop[8];
    int src_n, carry_n, land_n, drop_n;
} SpreadParts;

static void spread_parts(const Board *b, const PolicyContext *ctx, Move m, unsigned need,
                         SpreadParts *s) {
    const int us      = ctx->us;
    const int sq      = move_square(m);
    const int step    = DirectionStep[move_direction(m)];
    const int pattern = move_pattern(m);
    const int carry   = move_carry(m);
    const int hops    = __builtin_popcount((unsigned)pattern);
    const int height  = board_short_height(b, sq);
    const u64 word    = b->stones[sq];
    const u64 mine = b->owned[us], theirs = b->owned[!us], empty = board_empty(b);
    const u64 source     = 1ULL << sq;
    const bool left      = height > carry;
    const bool left_ours = left && ((word >> carry) & 1) == (u64)us;

    if (need & 1) {
        int k       = 0;
        s->src[k++] = (u16)(S_PHASE + ctx->phase);
        s->src[k++] = (u16)(S_TOP + (int)board_top(b, sq));
        s->src[k++] = (u16)(S_RACE + ctx->race);
        s->src[k++] = (u16)(S_HEIGHT + (height <= 2 ? height - 1 : height <= 4 ? 2 : 3));
        const int a = ring_index(ctx->prev, source), z = ring_index(ctx->prev2, source);
        if (a < 3 || z < 3) {
            s->src[k++] = (u16)(S_PREV + (a * 4 + z) * 5 + ctx->prev_kind);
        }
        s->src_n = k;
    }
    if (need & 2) {
        int k         = 0;
        s->carry[k++] = (u16)(S_CARRY + carry - 1);
        if (carry == height) {
            s->carry[k++] = (u16)S_WHOLE;
        }
        s->carry[k++]     = (u16)(S_LEFT + (left ? (left_ours ? 1 : 2) : 0));
        const int blacks  = __builtin_popcountll(word & ((1ULL << carry) - 1));
        const int carried = us == BLACK ? blacks : carry - blacks;
        s->carry[k++]     = (u16)(S_OWN + (carried == carry       ? 0
                                           : 2 * carried >= carry ? 1
                                           : carried              ? 2
                                                                  : 3));
        s->carry_n        = k;
    }
    const u64 landed = MoveLine[sq][move_direction(m)][hops];
    const int to     = sq + step * hops;
    if (need & 4) {
        int tallest = 0;
        for (u64 their_landed = landed & theirs; their_landed;) {
            const int at = pop_lsb(&their_landed);
            const int h  = board_short_height(b, at);
            tallest      = h > tallest ? h : tallest;
        }
        const int near_ours   = popcount(landed & ctx->near_road[0]);
        const int near_theirs = popcount(landed & ctx->near_road[1]);
        const int on_empty    = popcount(landed & empty);
        const int on_ours     = popcount(landed & mine);
        const int on_theirs   = popcount(landed & theirs);
        int k                 = 0;
        s->land[k++] =
            (u16)(S_TAKES + (tallest < 3 ? tallest : 3) * 2 + (int)((b->walls >> to) & 1));
        s->land[k++] =
            (u16)(S_NEAR +
                  ((near_ours < 2 ? near_ours : 2) * 3 + (near_theirs < 2 ? near_theirs : 2)) * 2 +
                  ((landed & ctx->threats) != 0));
        s->land[k++] =
            (u16)(S_COUNTS + ((on_empty < 3 ? on_empty : 3) * 3 + (on_ours < 2 ? on_ours : 2)) * 3 +
                  (on_theirs < 2 ? on_theirs : 2));
        const int a = ((landed & ctx->prev[0]) != 0) | ((landed & ctx->prev[1]) != 0) << 1;
        const int z = ((landed & ctx->prev2[0]) != 0) | ((landed & ctx->prev2[1]) != 0) << 1;
        if (a | z) {
            s->land[k++] = (u16)(S_LAND_PREV + (ctx->prev_kind * 4 + a) * 4 + z);
        }
        s->land_n = k;
    }
    if (need & 8) {
        /* Which tops become ours, read off the stack's bits: a hop's top is its last stone. */
        u64 ours_new = 0;
        int at       = sq;
        for (int bits = pattern; bits; bits &= bits - 1) {
            at += step;
            ours_new |= ((word >> (carry - 1 - __builtin_ctz((unsigned)bits))) & 1) == (u64)us
                            ? 1ULL << at
                            : 0;
        }
        const u64 theirs_new = landed & ~ours_new;
        int swing            = -1 + (left ? (left_ours ? 1 : -1) : 0) + popcount(ours_new) -
                    popcount(theirs_new) - popcount(landed & mine) + popcount(landed & theirs);
        swing              = swing < -4 ? -4 : swing > 4 ? 4 : swing;
        const int top      = carry - 1;
        const int rest     = pattern & ~(1 << top);
        const int last     = top - (rest ? 31 - __builtin_clz((unsigned)rest) : -1);
        int k              = 0;
        s->drop[k++]       = (u16)(S_SWING + (swing + 4) * 4 + ctx->phase);
        s->drop[k++]       = (u16)(S_HOPS + (hops - 1) * 3 + (last < 3 ? last - 1 : 2));
        const u64 on_empty = ours_new & empty;
        const u64 flats    = ~b->walls;
        s->drop[k++]       = (u16)(S_DIST_OURS + rel_min(ctx->rel_empty[0], on_empty) * 4 +
                             dist_bucket(ctx->dist[0]));
        const u64 broken   = ours_new & theirs & flats;
        const int brk      = broken & ctx->rel_own[1][0] ? 0 : broken & ctx->rel_own[1][1] ? 1 : 2;
        if (brk < 2) {
            s->drop[k++] = (u16)(S_BREAK + brk * 4 + dist_bucket(ctx->dist[1]));
        }
        const int block = rel_min(ctx->rel_empty[1], on_empty);
        if (block < 3) {
            s->drop[k++] = (u16)(S_BLOCK + block * 4 + dist_bucket(ctx->dist[1]));
        }
        const u64 given      = left && !left_ours && !((b->walls >> sq) & 1) ? source : 0;
        const u64 lost_empty = theirs_new & empty;
        const u64 lost_own   = (theirs_new & mine & flats) | given;
        const int give = (lost_empty & ctx->rel_empty[0][0]) || (lost_own & ctx->rel_own[0][0]) ? 0
                         : (lost_empty & ctx->rel_empty[0][1]) || (lost_own & ctx->rel_own[0][1])
                             ? 1
                             : 2;
        if (give < 2) {
            s->drop[k++] = (u16)(S_GIVE + give * 4 + dist_bucket(ctx->dist[0]));
        }
        s->drop_n = k;
    }
}

int policy_features(const Board *b, const PolicyContext *ctx, Move m, u16 *out) {
    if (move_is_place(m)) {
        PlaceSquare p;
        place_square(b, ctx, move_square(m), &p);
        return place_emit(ctx, &p, (int)move_stone(m), out);
    }
    SpreadParts s;
    spread_parts(b, ctx, m, 15, &s);
    int n = 0;
    for (int i = 0; i < s.src_n; i++) {
        out[n++] = s.src[i];
    }
    for (int i = 0; i < s.carry_n; i++) {
        out[n++] = s.carry[i];
    }
    for (int i = 0; i < s.land_n; i++) {
        out[n++] = s.land[i];
    }
    for (int i = 0; i < s.drop_n; i++) {
        out[n++] = s.drop[i];
    }
    return n;
}

/* A placement's rows for policy_logits(), the same features as place_square() and place_emit()
 * give, laid out so that scoring one is eight row sums and no branch: which neighbours a square
 * has and what the last moves were differ from square to square with no pattern a predictor can
 * learn, and a miss costs more than the sums do. An offset is within its group for one stone; a
 * row that is not there is the row of zeros. */
typedef struct FastSquare {
    u16 off[8];
    unsigned used;
} FastSquare;

#define NEIGHBOUR_ROWS (NEIGHBOUR_CODES * 4) /* a stone's rows of P_NEIGHBOURS */
static const u16 FastBase[8]   = {P_NEIGHBOURS, P_NEIGHBOURS, P_NEIGHBOURS,  P_NEIGHBOURS,
                                  P_ROAD,       P_DIST_OURS,  P_DIST_THEIRS, P_PREV};
static const u16 FastStride[8] = {NEIGHBOUR_ROWS, NEIGHBOUR_ROWS, NEIGHBOUR_ROWS, NEIGHBOUR_ROWS,
                                  4 * 32,         4 * 4,          4 * 4,          4 * 4 * 5};
static const i16 ZeroRow[POLICY_LANES];

/* neighbour_code() without its branches. */
static inline int code_at(const Board *b, int sq, int us) {
    const int mine = (int)((b->owned[us] >> sq) & 1), theirs = (int)((b->owned[!us] >> sq) & 1);
    const int wall = (int)((b->walls >> sq) & 1), cap = (int)((b->caps >> sq) & 1);
    const int tall = (int)((b->tall >> sq) & 1);
    return (mine | theirs) * (1 + theirs * 4 + wall + 2 * cap + 3 * (tall & ~wall & ~cap));
}

static inline void fast_square(const Board *b, const PolicyContext *ctx, int sq, FastSquare *s) {
    const u64 bit  = 1ULL << sq;
    const int file = sq & 7, rank = sq >> 3, last = b->size - 1;
    /* North, south, east and west. */
    const int valid[4] = {(rank < last), (rank > 0), (file < last), (file > 0)};
    const int at[4]    = {(sq + 8) & 63, (sq - 8) & 63, (sq + 1) & 63, (sq - 1) & 63};
    int code[4];
    for (int i = 0; i < 4; i++) {
        /* A neighbour off the board is a kind of its own, so it matches nothing. */
        code[i] = valid[i] ? code_at(b, at[i], ctx->us) : 16 + i;
    }
    unsigned used = 0x70;
    for (int i = 0; i < 4; i++) {
        int same = 0, earlier = 0;
        for (int j = 0; j < 4; j++) {
            const int eq = code[j] == code[i];
            same += eq;
            earlier |= eq & (j < i);
        }
        const int first = valid[i] & !earlier;
        s->off[i]       = (u16)(first ? code[i] * 4 + same - 1 : 0);
        used |= (unsigned)first << i;
    }
    const int road = ((ctx->near_road[0] & bit) != 0) | ((ctx->near_road[1] & bit) != 0) << 1 |
                     ((ctx->threats & bit) != 0) << 2 | ((ctx->ends[0] & bit) != 0) << 3 |
                     ((ctx->ends[1] & bit) != 0) << 4;
    s->off[4]      = (u16)(ctx->phase * 32 + road);
    s->off[5]      = (u16)(rel_at(ctx->rel_empty[0], sq) * 4 + dist_bucket(ctx->dist[0]));
    s->off[6]      = (u16)(rel_at(ctx->rel_empty[1], sq) * 4 + dist_bucket(ctx->dist[1]));
    const int prev = ring_index(ctx->prev, bit), prev2 = ring_index(ctx->prev2, bit);
    const int any = (prev < 3) | (prev2 < 3);
    s->off[7]     = (u16)(any ? (prev * 4 + prev2) * 5 + ctx->prev_kind : 0);
    s->used       = used | (unsigned)any << 7;
}

static inline PolicyVec fast_place(PolicyVec acc, const FastSquare *s, int type) {
    for (int k = 0; k < 8; k++) {
        const i16 *row =
            (s->used >> k) & 1
                ? Rows + (size_t)(FastBase[k] + type * FastStride[k] + s->off[k]) * POLICY_LANES
                : ZeroRow;
        acc += *(const PolicyVec *)row;
    }
    return acc;
}

/* 256ths of a nat: the units' sum is in QA * QB a nat and the linear lane in QA. */
static inline int finish(PolicyVec acc) {
    const PolicyVec zero = {0}, top = zero + POLICY_QA;
    PolicyVec h          = acc & (PolicyVec)(acc > zero);
    const PolicyVec over = (PolicyVec)(h > top);
    h                    = (h & ~over) | (top & over);
    /* Widened by halves and multiplied as vectors. Left as a loop over the lanes, the compiler
     * takes each unit out and multiplies it alone wherever this is inlined into a longer
     * function, which is a fifth of what a move costs to score: E103. */
    i16 units[POLICY_LANES];
    *(PolicyVec *)units = h;
    const PolicyWide products =
        __builtin_convertvector(*(const PolicyHalf *)units, PolicyWide) *
            __builtin_convertvector(*(const PolicyHalf *)Out, PolicyWide) +
        __builtin_convertvector(*(const PolicyHalf *)(units + POLICY_LANES / 2), PolicyWide) *
            __builtin_convertvector(*(const PolicyHalf *)(Out + POLICY_LANES / 2), PolicyWide);
    int dot = 0;
    for (int k = 0; k < POLICY_LANES / 2; k++) {
        dot += products[k];
    }
    _Static_assert(POLICY_QA * POLICY_QB == 256 << 7, "the units' sum is shifted to 256ths");
    return (dot >> 7) + acc[POLICY_LANES - 1] * (256 / POLICY_QA);
}

int policy_logit(const Board *b, const PolicyContext *ctx, Move m) {
    if (!rows_now()) {
        return 0;
    }
    u16 features[POLICY_MAX_ACTIVE];
    const int n = policy_features(b, ctx, m, features);
    return finish(add_rows(*(const PolicyVec *)Bias, features, n));
}

void policy_logits(const Board *b, const PolicyContext *ctx, const Move *moves, int count,
                   i32 *out) {
    if (!rows_now()) {
        for (int i = 0; i < count; i++) {
            out[i] = 0;
        }
        return;
    }
    /* After rows_now(), which installs the biases on the first call. */
    const PolicyVec base = *(const PolicyVec *)Bias;
    const PolicyVec zero = {0};
    FastSquare squares[SQUARE_COUNT];
    PolicyVec stone_acc[3], src_acc[SQUARE_COUNT], carry_acc[SQUARE_COUNT][8];
    PolicyVec land_acc[4][8], step_acc[2][SQUARE_COUNT];
    u64 have_square = 0, have_src = 0, have_carry[8] = {0}, have_step[2] = {0, 0};
    unsigned have_stone = 0, have_land = 0;
    int land_source = -1;

    for (int i = 0; i < count; i++) {
        const Move m = moves[i];
        const int sq = move_square(m);
        const u64 at = 1ULL << sq;
        if (move_is_place(m)) {
            const int type = (int)move_stone(m);
            if (!(have_square & at)) {
                fast_square(b, ctx, sq, &squares[sq]);
                have_square |= at;
            }
            if (!(have_stone & (1u << type))) {
                const u16 node[2] = {(u16)(P_PHASE + type * 4 + ctx->phase),
                                     (u16)(P_RACE + type * 4 + ctx->race)};
                stone_acc[type]   = add_rows(base, node, 2);
                have_stone |= 1u << type;
            }
            out[i] = finish(fast_place(stone_acc[type], &squares[sq], type));
            continue;
        }

        const int c   = move_carry(m) - 1;
        unsigned need = have_carry[c] & at ? 0 : have_src & at ? 2 : 3;
        /* One stone one square, leaving the source empty or ours: where it lands is all the rest
         * reads, so every stone that steps there shares it. */
        int key = -1, to = 0;
        if (move_pattern(m) == 1) {
            const u64 word = b->stones[sq];
            if (word < 4 || ((word >> 1) & 1) == (u64)ctx->us) {
                key = word >= 4;
                to  = sq + DirectionStep[move_direction(m)];
            }
        }
        /* A stack's spreads come together, so where they land is kept for one source at a time. */
        const int lane = (int)move_direction(m) * 8 + __builtin_popcount(move_pattern(m)) - 1;
        if (key >= 0) {
            need |= (have_step[key] >> to) & 1 ? 0 : 12;
        } else {
            if (sq != land_source) {
                land_source = sq;
                have_land   = 0;
            }
            need |= (have_land >> lane) & 1 ? 8 : 12;
        }
        SpreadParts s;
        spread_parts(b, ctx, m, need, &s);
        if (need & 1) {
            src_acc[sq] = add_rows(base, s.src, s.src_n);
            have_src |= at;
        }
        if (need & 2) {
            carry_acc[sq][c] = add_rows(src_acc[sq], s.carry, s.carry_n);
            have_carry[c] |= at;
        }
        if (key >= 0) {
            if (need & 12) {
                step_acc[key][to] = add_rows(add_rows(zero, s.land, s.land_n), s.drop, s.drop_n);
                have_step[key] |= 1ULL << to;
            }
            out[i] = finish(carry_acc[sq][c] + step_acc[key][to]);
            continue;
        }
        if (need & 4) {
            land_acc[lane >> 3][lane & 7] = add_rows(zero, s.land, s.land_n);
            have_land |= 1u << lane;
        }
        out[i] =
            finish(add_rows(carry_acc[sq][c] + land_acc[lane >> 3][lane & 7], s.drop, s.drop_n));
    }
}
