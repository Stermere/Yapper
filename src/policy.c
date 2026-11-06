/* policy.c - a prior over the moves of a position, from what each move does in Tak's own terms.
 *
 * Every feature reads the board the same way under its eight symmetries and from either side,
 * so the prior cannot tell a turned or recoloured position from the original. */
#include "policy.h"

#include "bitboard.h"
#include "movegen.h"
#include "policy_net.h"

/* A neighbour of a placement, as one of these: empty, then each side's flat, wall, capstone and
 * stack of two or more under a flat, the side to move's first. */
#define NEIGHBOUR_CODES 9

enum {
    F_TYPE_PHASE         = 0,                                           /* type, phase */
    F_TYPE_RING          = F_TYPE_PHASE + 3 * 4,                        /* type, ring */
    F_TYPE_CORNER        = F_TYPE_RING + 3 * 4,                         /* type */
    F_TYPE_NEIGHBOURS    = F_TYPE_CORNER + 3,                           /* type, code, how many */
    F_TYPE_NEAR_OURS     = F_TYPE_NEIGHBOURS + 3 * NEIGHBOUR_CODES * 4, /* type, phase */
    F_TYPE_NEAR_THEIRS   = F_TYPE_NEAR_OURS + 3 * 4,                    /* type, phase */
    F_TYPE_THREAT        = F_TYPE_NEAR_THEIRS + 3 * 4,                  /* type */
    F_TYPE_ENDS_OURS     = F_TYPE_THREAT + 3,                           /* type */
    F_TYPE_ENDS_THEIRS   = F_TYPE_ENDS_OURS + 3,                        /* type */
    F_TYPE_RACE          = F_TYPE_ENDS_THEIRS + 3,                      /* type, race */
    F_SPREAD_PHASE       = F_TYPE_RACE + 3 * 4,                         /* phase */
    F_SPREAD_CARRY       = F_SPREAD_PHASE + 4,                          /* 1 to 8 */
    F_SPREAD_HOPS        = F_SPREAD_CARRY + 8,                          /* 1 to 7 */
    F_SPREAD_WHOLE       = F_SPREAD_HOPS + 7,        /* carries the whole stack */
    F_SPREAD_SWING       = F_SPREAD_WHOLE + 1,       /* tops won -4 to 4, phase */
    F_SPREAD_TOP         = F_SPREAD_SWING + 9 * 4,   /* flat, wall or capstone */
    F_SPREAD_SOURCE      = F_SPREAD_TOP + 3,         /* left empty, ours, theirs */
    F_SPREAD_TAKES       = F_SPREAD_SOURCE + 3,      /* tallest of theirs covered: none, 1, 2, 3+ */
    F_SPREAD_NEAR_OURS   = F_SPREAD_TAKES + 4,       /* landings on our near squares: 0, 1, 2+ */
    F_SPREAD_NEAR_THEIRS = F_SPREAD_NEAR_OURS + 3,   /* and on theirs */
    F_SPREAD_THREAT      = F_SPREAD_NEAR_THEIRS + 3, /* lands where they would place a road */
    F_SPREAD_CRUSH       = F_SPREAD_THREAT + 1,
    F_SPREAD_RACE        = F_SPREAD_CRUSH + 1, /* race */
    F_SPREAD_RING        = F_SPREAD_RACE + 4,  /* ring of the last square */
    F_COUNT              = F_SPREAD_RING + 4,
};

static const i16 Zeros[F_COUNT];
static const i16 *Weights;

int policy_feature_count(void) { return F_COUNT; }

void policy_install(const i16 *weights) { Weights = weights; }

static inline const i16 *weights_now(void) {
    if (Weights) {
        return Weights;
    }
    /* A table fitted for another set of features is no table at all. */
    return POLICY_NET_FEATURES == F_COUNT ? PolicyNet : Zeros;
}

/* How far in from the nearest edge, up to 3. */
static inline int ring_of(const Board *b, int sq) {
    const int file = sq & 7, rank = sq >> 3, last = b->size - 1;
    int ring = file < rank ? file : rank;
    ring     = last - file < ring ? last - file : ring;
    ring     = last - rank < ring ? last - rank : ring;
    return ring < 3 ? ring : 3;
}

void policy_context(const Board *b, const u64 ends[4], const u64 their_ends[4],
                    PolicyContext *ctx) {
    const int us     = board_stm(b);
    const u64 empty  = board_empty(b);
    ctx->us          = us;
    ctx->near_ours   = board_road_near(ends, empty);
    ctx->near_theirs = board_road_near(their_ends, empty);
    ctx->threats     = board_road_gaps(their_ends) & empty;
    ctx->ends_ours   = (ends[0] | ends[1] | ends[2] | ends[3]) & b->mask;
    ctx->ends_theirs = (their_ends[0] | their_ends[1] | their_ends[2] | their_ends[3]) & b->mask;

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

static int placement_features(const Board *b, const PolicyContext *ctx, Move m, u16 *out) {
    const int sq   = move_square(m);
    const int type = (int)move_stone(m);
    const u64 bit  = 1ULL << sq;
    int n          = 0;
    out[n++]       = (u16)(F_TYPE_PHASE + type * 4 + ctx->phase);
    const int ring = ring_of(b, sq);
    out[n++]       = (u16)(F_TYPE_RING + type * 4 + ring);
    const int file = sq & 7, rank = sq >> 3, last = b->size - 1;
    if ((file == 0 || file == last) && (rank == 0 || rank == last)) {
        out[n++] = (u16)(F_TYPE_CORNER + type);
    }

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
    for (int code = 0; code < NEIGHBOUR_CODES; code++) {
        if (counts[code]) {
            out[n++] =
                (u16)(F_TYPE_NEIGHBOURS + (type * NEIGHBOUR_CODES + code) * 4 + counts[code] - 1);
        }
    }

    if (ctx->near_ours & bit) {
        out[n++] = (u16)(F_TYPE_NEAR_OURS + type * 4 + ctx->phase);
    }
    if (ctx->near_theirs & bit) {
        out[n++] = (u16)(F_TYPE_NEAR_THEIRS + type * 4 + ctx->phase);
    }
    if (ctx->threats & bit) {
        out[n++] = (u16)(F_TYPE_THREAT + type);
    }
    if (ctx->ends_ours & bit) {
        out[n++] = (u16)(F_TYPE_ENDS_OURS + type);
    }
    if (ctx->ends_theirs & bit) {
        out[n++] = (u16)(F_TYPE_ENDS_THEIRS + type);
    }
    out[n++] = (u16)(F_TYPE_RACE + type * 4 + ctx->race);
    return n;
}

static int spread_features(const Board *b, const PolicyContext *ctx, Move m, u16 *out) {
    const int us     = ctx->us;
    const int sq     = move_square(m);
    const int step   = DirectionStep[move_direction(m)];
    const int carry  = move_carry(m);
    const int height = board_short_height(b, sq);
    const u64 word   = b->stones[sq];
    int n            = 0;
    out[n++]         = (u16)(F_SPREAD_PHASE + ctx->phase);
    out[n++]         = (u16)(F_SPREAD_CARRY + carry - 1);
    out[n++]         = (u16)(F_SPREAD_HOPS + __builtin_popcount(move_pattern(m)) - 1);
    if (carry == height) {
        out[n++] = (u16)F_SPREAD_WHOLE;
    }
    int swing = movegen_spread_swing(b, m, us);
    swing     = swing < -4 ? -4 : swing > 4 ? 4 : swing;
    out[n++]  = (u16)(F_SPREAD_SWING + (swing + 4) * 4 + ctx->phase);
    out[n++]  = (u16)(F_SPREAD_TOP + (int)board_top(b, sq));
    out[n++] =
        (u16)(F_SPREAD_SOURCE + (height > carry ? (((word >> carry) & 1) == (u64)us ? 1 : 2) : 0));

    u64 landed  = 0;
    int tallest = 0;
    int to      = sq;
    for (int hops = move_pattern(m); hops; hops &= hops - 1) {
        to += step;
        landed |= 1ULL << to;
        if ((b->owned[!us] >> to) & 1) {
            const int h = board_short_height(b, to);
            tallest     = h > tallest ? h : tallest;
        }
    }
    out[n++]        = (u16)(F_SPREAD_TAKES + (tallest < 3 ? tallest : 3));
    const int ours  = popcount(landed & ctx->near_ours);
    const int other = popcount(landed & ctx->near_theirs);
    out[n++]        = (u16)(F_SPREAD_NEAR_OURS + (ours < 2 ? ours : 2));
    out[n++]        = (u16)(F_SPREAD_NEAR_THEIRS + (other < 2 ? other : 2));
    if (landed & ctx->threats) {
        out[n++] = (u16)F_SPREAD_THREAT;
    }
    if ((b->walls >> to) & 1) {
        out[n++] = (u16)F_SPREAD_CRUSH;
    }
    out[n++] = (u16)(F_SPREAD_RACE + ctx->race);
    out[n++] = (u16)(F_SPREAD_RING + ring_of(b, to));
    return n;
}

int policy_features(const Board *b, const PolicyContext *ctx, Move m, u16 *out) {
    return move_is_place(m) ? placement_features(b, ctx, m, out) : spread_features(b, ctx, m, out);
}

int policy_logit(const Board *b, const PolicyContext *ctx, Move m) {
    u16 features[POLICY_MAX_ACTIVE];
    const int n      = policy_features(b, ctx, m, features);
    const i16 *table = weights_now();
    int logit        = 0;
    for (int i = 0; i < n; i++) {
        logit += table[features[i]];
    }
    return logit;
}
