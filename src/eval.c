/* eval.c - a linear evaluation: how much of each term a position has, times what that term
 * is worth. Play adds them up; a trace also keeps the counts, from the same body, so that
 * what a fit reads cannot drift from what plays. */
#include "eval.h"

#include "bitboard.h"
#include "weights.h"

#define X(name) #name,
const char *const EvalTermNames[TERM_COUNT] = {EVAL_TERMS(X)};
#undef X

typedef struct Sum {
    i32 score;
    EvalTrace *trace;
} Sum;

static inline __attribute__((always_inline)) void add(Sum *sum, int term, int count) {
    sum->score += Weights[term] * count;
    if (sum->trace) {
        sum->trace->counts[term] += count;
    }
}

typedef struct Regions {
    u64 inner; /* off the edge */
    u64 core;  /* off the edge by two */
    u64 corners;
    u64 empty;
} Regions;

/* How many files the group touches. Folding the ranks together leaves one byte. */
static inline int files_spanned(u64 group) {
    group |= group >> 32;
    group |= group >> 16;
    group |= group >> 8;
    return popcount(group & 0xFF);
}

/* How many ranks the group touches: the top bit of a byte is set if any bit of it is. */
static inline int ranks_spanned(u64 group) {
    const u64 low = 0x7F7F7F7F7F7F7F7FULL;
    return popcount((((group & low) + low) | group) & ~low);
}

static inline __attribute__((always_inline)) void group_terms(Sum *sum, const Board *b, int color,
                                                              int side) {
    u64 left = board_road_stones(b, color);
    while (left) {
        const u64 group = flood(left & -left, left);
        left &= ~group;
        const int files             = files_spanned(group);
        const int ranks             = ranks_spanned(group);
        const int short_of_spanning = b->size - (files > ranks ? files : ranks);
        if (short_of_spanning >= 1 && short_of_spanning <= 5) {
            add(sum, T_GROUP_1 + short_of_spanning - 1, side);
        }
    }
}

/* Stones under a top stone can be carried away with it, as far as the carry limit. */
static inline __attribute__((always_inline)) void stack_terms(Sum *sum, const Board *b, int color,
                                                              int side) {
    u64 stacks = b->owned[color] & b->tall;
    while (stacks) {
        const int sq      = pop_lsb(&stacks);
        const int height  = board_short_height(b, sq);
        const int under   = height - 1 < b->size - 1 ? height - 1 : b->size - 1;
        const u64 window  = (b->stones[sq] >> 1) & ((1ULL << under) - 1);
        const int blacks  = popcount(window);
        const int friends = color == BLACK ? blacks : under - blacks;
        const int top     = (int)board_top(b, sq);
        add(sum, T_FRIEND_FLAT + top, side * friends);
        add(sum, T_CAPTIVE_FLAT + top, side * (under - friends));
        if (top == CAP && (int)(window & 1) == color) {
            add(sum, T_HARD_CAP, side);
        }
    }
}

/* Pairs of stones, each pair counted once. */
static inline int adjacent_pairs(u64 stones) {
    return popcount(stones & shift_east(stones)) + popcount(stones & stones << 8);
}
static inline int diagonal_pairs(u64 stones) {
    const u64 north = stones << 8;
    return popcount(stones & shift_east(north)) + popcount(stones & shift_west(north));
}

static inline __attribute__((always_inline)) void side_terms(Sum *sum, const Board *b, int color,
                                                             int side, const Regions *regions) {
    const u64 mine        = b->owned[color];
    const u64 flats       = board_flats(b, color);
    const u64 walls       = mine & b->walls;
    const u64 caps        = mine & b->caps;
    const u64 road        = flats | caps;
    const u64 their_flats = board_flats(b, !color);
    const u64 their_road  = board_road_stones(b, !color);

    add(sum, T_WALL, side * popcount(walls));
    add(sum, T_CAP, side * popcount(caps));
    add(sum, T_FLAT_INNER, side * popcount(flats & regions->inner));
    add(sum, T_FLAT_CORE, side * popcount(flats & regions->core));
    add(sum, T_FLAT_CORNER, side * popcount(flats & regions->corners));
    add(sum, T_WALL_INNER, side * popcount(walls & regions->inner));
    add(sum, T_WALL_CORE, side * popcount(walls & regions->core));
    add(sum, T_CAP_INNER, side * popcount(caps & regions->inner));
    add(sum, T_CAP_CORE, side * popcount(caps & regions->core));

    add(sum, T_ADJACENT, side * adjacent_pairs(road));
    add(sum, T_DIAGONAL, side * diagonal_pairs(road));
    add(sum, T_LIBERTY, side * popcount(neighbours(road) & regions->empty));
    add(sum, T_FLAT_CONTACT, side * popcount(neighbours(flats) & their_flats));
    add(sum, T_WALL_CONTACT, side * popcount(neighbours(walls) & their_road));
    add(sum, T_CAP_CONTACT, side * popcount(neighbours(caps) & (b->owned[!color] & ~b->caps)));

    stack_terms(sum, b, color, side);
    group_terms(sum, b, color, side);
}

static inline int unpack_opening(i32 packed) { return (i16)(packed & 0xFFFF); }
static inline int unpack_ending(i32 packed) { return (packed + 0x8000) >> 16; }

static inline int blend(i32 packed, int phase) {
    return (unpack_opening(packed) * (PHASE_END - phase) + unpack_ending(packed) * phase) >> 8;
}

/* Which LEAD term applies with this many stones left to place. */
static inline int lead_term(int left) {
    static const u8 Buckets[16] = {
        T_LEAD_1, T_LEAD_1, T_LEAD_2,  T_LEAD_3,  T_LEAD_4,  T_LEAD_6,  T_LEAD_6,  T_LEAD_9,
        T_LEAD_9, T_LEAD_9, T_LEAD_14, T_LEAD_14, T_LEAD_14, T_LEAD_14, T_LEAD_14, T_LEAD_MORE};
    return Buckets[left < 15 ? left : 15];
}

static inline __attribute__((always_inline)) int evaluate_body(const Board *b, u64 threats,
                                                               EvalTrace *trace) {
    const int us         = board_stm(b);
    const int mine_left  = b->flats[us] + b->capstones[us];
    const int their_left = b->flats[!us] + b->capstones[!us];
    const int start      = board_start_flats(b->size) + board_start_capstones(b->size);
    const u64 empty      = board_empty(b);

    /* A game ends when a side runs out of stones or the board fills, whichever is first. */
    int left         = mine_left < their_left ? mine_left : their_left;
    const int spaces = popcount(empty);
    left             = spaces < left ? spaces : left;
    const int phase  = (start - (left < start ? left : start)) * PHASE_END / start;

    const u64 edges       = (FILE_A | RANK_1 | b->file_last | b->rank_last) & b->mask;
    const Regions regions = {.inner   = b->mask & ~edges,
                             .core    = b->mask & ~edges & ~neighbours(edges),
                             .corners = (FILE_A | b->file_last) & (RANK_1 | b->rank_last) & b->mask,
                             .empty   = empty};

    Sum sum = {0, trace};
    side_terms(&sum, b, us, 1, &regions);
    side_terms(&sum, b, !us, -1, &regions);

    const int komi      = us == WHITE ? -b->halfkomi : b->halfkomi;
    const int flat_lead = popcount(board_flats(b, us)) - popcount(board_flats(b, !us));
    add(&sum, lead_term(left), flat_lead * 2 + komi);
    add(&sum, T_KOMI, komi);
    add(&sum, T_RESERVE, mine_left - their_left);

    /* The side to move has already had its chance to win on the spot, so a threat by the
     * other side is one this side still has to answer. */
    if (threats) {
        add(&sum, (threats & (threats - 1)) ? T_THREAT_MANY : T_THREAT_ONE, 1);
    }
    add(&sum, T_TEMPO, 1);

    if (trace) {
        trace->phase = phase;
    }
    return blend(sum.score, phase);
}

int evaluate(const Board *b, u64 threats) { return evaluate_body(b, threats, NULL); }

int evaluate_trace(const Board *b, u64 threats, EvalTrace *trace) {
    *trace = (EvalTrace){0};
    return evaluate_body(b, threats, trace);
}

int eval_from_trace(const EvalTrace *trace) {
    i32 score = 0;
    for (int term = 0; term < TERM_COUNT; term++) {
        score += Weights[term] * trace->counts[term];
    }
    return blend(score, trace->phase);
}

void eval_weights(double *opening, double *ending) {
    for (int term = 0; term < TERM_COUNT; term++) {
        opening[term] = unpack_opening(Weights[term]);
        ending[term]  = unpack_ending(Weights[term]);
    }
}
