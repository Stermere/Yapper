/* policy.c - a prior over the moves of a position, from what each move does in Tak's own terms.
 *
 * Every feature reads the board the same way under its eight symmetries and from either side,
 * so the prior cannot tell a turned or recoloured position from the original. A move's features
 * index rows of 16 lanes, which are summed with the position's own rows; 15 lanes are clipped
 * units weighed by the output, and the last is a linear term. */
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
    P_CLASS       = P_PREV + 3 * 4 * 4 * 5,                 /* type, square class */
    P_RUN         = P_CLASS + 3 * 10,                       /* flat or cap, our line 1-5 */
    P_CUT         = P_RUN + 2 * 5,                          /* type, their line it cuts 0-4 */
    /* A spread's source alone. */
    S_PHASE  = P_CUT + 3 * 5,
    S_TOP    = S_PHASE + 4, /* flat, wall or capstone */
    S_RACE   = S_TOP + 3,
    S_HEIGHT = S_RACE + 4,   /* 1, 2, 3-4, 5+ */
    S_PREV   = S_HEIGHT + 4, /* last move, ours, its kind */
    S_CLASS  = S_PREV + 4 * 4 * 5,
    /* Its source and carry. */
    S_CARRY = S_CLASS + 10, /* 1 to 8 */
    S_WHOLE = S_CARRY + 8,  /* carries the whole stack */
    S_LEFT  = S_WHOLE + 1,  /* leaves the source empty, ours, theirs */
    S_OWN   = S_LEFT + 3,   /* our stones carried: all, most, some, none */
    /* Where it lands, the same for every spread of a stack the same way as far. */
    S_TAKES      = S_OWN + 4,       /* tallest of theirs covered 0-3+, a crush */
    S_NEAR       = S_TAKES + 4 * 2, /* landings on our near squares 0-2+, theirs, a road square */
    S_COUNTS     = S_NEAR + 3 * 3 * 2,      /* landings on empties 0-3+, ours 0-2+, theirs 0-2+ */
    S_LAND_PREV  = S_COUNTS + 4 * 3 * 3,    /* last move's kind, lands on/beside it, our last */
    S_LAST_CLASS = S_LAND_PREV + 5 * 4 * 4, /* class of the last square */
    /* How it drops. */
    S_SWING     = S_LAST_CLASS + 10,   /* tops won -4 to 4, phase */
    S_HOPS      = S_SWING + 9 * 4,     /* hops 1-7, stones dropped last 1-3+ */
    S_DIST_OURS = S_HOPS + 7 * 3,      /* best rel of our new tops on empties 0-4, our distance */
    S_BREAK     = S_DIST_OURS + 5 * 4, /* covers their road stone at rel 0/1, their distance */
    S_BLOCK     = S_BREAK + 2 * 4,     /* our new top on an empty at their rel 0-2 */
    S_GIVE      = S_BLOCK + 3 * 4,     /* their new top at our rel 0/1, our distance */
    /* The position alone. */
    C_PHASE       = S_GIVE + 2 * 4,
    C_RACE        = C_PHASE + 4,
    C_DIST_OURS   = C_RACE + 4,
    C_DIST_THEIRS = C_DIST_OURS + 4,
    C_LEAD        = C_DIST_THEIRS + 4, /* flats ahead with komi, seven buckets */
    C_CAPS        = C_LEAD + 7,        /* a capstone in hand, ours and theirs */
    C_EMPTIES     = C_CAPS + 2,        /* up to 6, 12, 20, more */
    F_COUNT       = C_EMPTIES + 4,
};

int policy_feature_count(void) { return F_COUNT; }

/* GCC's vectors, so one source lowers to AVX2, SSE or NEON. Unaligned, because Windows does not
 * keep the stack 32-byte aligned for every compiler. */
typedef i16 PolicyVec __attribute__((vector_size(32), aligned(2), may_alias));

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

/* How far in from the nearest edge across the files and across the ranks, sorted, each up to 3:
 * ten classes, the same under every symmetry, which say a ring and a corner too. */
static inline int class_of(const Board *b, int sq) {
    static const u8 Index[4][4] = {{0, 1, 2, 3}, {1, 4, 5, 6}, {2, 5, 7, 8}, {3, 6, 8, 9}};
    const int file = sq & 7, rank = sq >> 3, last = b->size - 1;
    int f = file < last - file ? file : last - file;
    int r = rank < last - rank ? rank : last - rank;
    f     = f < 3 ? f : 3;
    r     = r < 3 ? r : 3;
    return Index[f][r];
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
    if (m == MOVE_NONE) {
        rings[0] = rings[1] = rings[2] = 0;
        return;
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
    rings[0] = bits;
    rings[1] = neighbours(bits) & b->mask & ~bits;
    rings[2] = neighbours(rings[1]) & b->mask & ~bits & ~rings[1];
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
        const int lead = 2 * (popcount(board_flats(b, us)) - popcount(board_flats(b, !us))) +
                         (us == BLACK ? b->halfkomi : -b->halfkomi);
        ctx->race = lead < 0 ? 0 : lead == 0 ? 1 : 2;
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

    PolicyVec acc = *(const PolicyVec *)Bias;
    if (rows_now()) {
        u16 features[POLICY_MAX_ACTIVE];
        acc = add_rows(acc, features, policy_context_features(b, ctx, features));
    }
    *(PolicyVec *)ctx->acc = acc;
}

int policy_context_features(const Board *b, const PolicyContext *ctx, u16 *out) {
    const int us   = ctx->us;
    int n          = 0;
    out[n++]       = (u16)(C_PHASE + ctx->phase);
    out[n++]       = (u16)(C_RACE + ctx->race);
    out[n++]       = (u16)(C_DIST_OURS + dist_bucket(ctx->dist[0]));
    out[n++]       = (u16)(C_DIST_THEIRS + dist_bucket(ctx->dist[1]));
    const int lead = 2 * (popcount(board_flats(b, us)) - popcount(board_flats(b, !us))) +
                     (us == BLACK ? b->halfkomi : -b->halfkomi);
    out[n++] = (u16)(C_LEAD + (lead <= -4   ? 0
                               : lead <= -2 ? 1
                               : lead < 0   ? 2
                               : lead == 0  ? 3
                               : lead < 2   ? 4
                               : lead < 4   ? 5
                                            : 6));
    if (b->capstones[us]) {
        out[n++] = (u16)C_CAPS;
    }
    if (b->capstones[!us]) {
        out[n++] = (u16)(C_CAPS + 1);
    }
    const int e = popcount(board_empty(b));
    out[n++]    = (u16)(C_EMPTIES + (e <= 6 ? 0 : e <= 12 ? 1 : e <= 20 ? 2 : 3));
    return n;
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

/* Road stones of a set in a line through sq, sq counted in, the longer of the two lines. */
static inline int run_through(const Board *b, u64 stones, int sq) {
    const int file = sq & 7, rank = sq >> 3, size = b->size;
    int h = 1, v = 1;
    for (int f = file + 1; f < size && ((stones >> (sq + f - file)) & 1); f++) {
        h++;
    }
    for (int f = file - 1; f >= 0 && ((stones >> (sq + f - file)) & 1); f--) {
        h++;
    }
    for (int r = rank + 1; r < size && ((stones >> (sq + 8 * (r - rank))) & 1); r++) {
        v++;
    }
    for (int r = rank - 1; r >= 0 && ((stones >> (sq + 8 * (r - rank))) & 1); r--) {
        v++;
    }
    return h > v ? h : v;
}

/* What a placement's features read off its square, the same for every stone placed there. */
typedef struct PlaceSquare {
    u8 codes, code[4], count[4]; /* the kinds of neighbour present, in code order, and how many */
    u8 road;                     /* near ours, near theirs, a road square, ends ours, theirs */
    u8 rel_ours, rel_theirs, prev, prev2, cls, run, cut;
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
    p->cls        = (u8)class_of(b, sq);
    const int run = run_through(b, board_road_stones(b, ctx->us), sq);
    const int cut = run_through(b, board_road_stones(b, !ctx->us), sq) - 1;
    p->run        = (u8)(run < 5 ? run : 5);
    p->cut        = (u8)(cut < 4 ? cut : 4);
}

/* `node_rows` false leaves out the two features of the stone and the node alone, which a node
 * sums once for each stone. */
static int place_emit(const PolicyContext *ctx, const PlaceSquare *p, int type, u16 *out,
                      bool node_rows) {
    int n = 0;
    if (node_rows) {
        out[n++] = (u16)(P_PHASE + type * 4 + ctx->phase);
        out[n++] = (u16)(P_RACE + type * 4 + ctx->race);
    }
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
    out[n++] = (u16)(P_CLASS + type * 10 + p->cls);
    if (type != WALL) {
        out[n++] = (u16)(P_RUN + (type == CAP) * 5 + p->run - 1);
    }
    out[n++] = (u16)(P_CUT + type * 5 + p->cut);
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
        s->src[k++] = (u16)(S_CLASS + class_of(b, sq));
        s->src_n    = k;
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
    u64 landed = 0;
    int to     = sq;
    for (int h = 0; h < hops; h++) {
        to += step;
        landed |= 1ULL << to;
    }
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
        s->land[k++] = (u16)(S_LAST_CLASS + class_of(b, to));
        s->land_n    = k;
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
        return place_emit(ctx, &p, (int)move_stone(m), out, true);
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

/* 256ths of a nat: the units' sum is in QA * QB a nat and the linear lane in QA. */
static inline int finish(PolicyVec acc) {
    const PolicyVec zero = {0}, top = zero + POLICY_QA;
    PolicyVec h          = acc & (PolicyVec)(acc > zero);
    const PolicyVec over = (PolicyVec)(h > top);
    h                    = (h & ~over) | (top & over);
    i16 units[POLICY_LANES];
    *(PolicyVec *)units = h;
    int dot             = 0;
    for (int k = 0; k < POLICY_LANES; k++) {
        dot += (int)units[k] * Out[k];
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
    return finish(add_rows(*(const PolicyVec *)ctx->acc, features, n));
}

void policy_logits(const Board *b, const PolicyContext *ctx, const Move *moves, int count,
                   i32 *out) {
    if (!rows_now()) {
        for (int i = 0; i < count; i++) {
            out[i] = 0;
        }
        return;
    }
    const PolicyVec base = *(const PolicyVec *)ctx->acc;
    const PolicyVec zero = {0};
    PlaceSquare squares[SQUARE_COUNT];
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
                place_square(b, ctx, sq, &squares[sq]);
                have_square |= at;
            }
            if (!(have_stone & (1u << type))) {
                const u16 node[2] = {(u16)(P_PHASE + type * 4 + ctx->phase),
                                     (u16)(P_RACE + type * 4 + ctx->race)};
                stone_acc[type]   = add_rows(base, node, 2);
                have_stone |= 1u << type;
            }
            u16 f[POLICY_MAX_ACTIVE];
            const int n = place_emit(ctx, &squares[sq], type, f, false);
            out[i]      = finish(add_rows(stone_acc[type], f, n));
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
