/* eval.c - a small hand-written evaluation, to be replaced by a tuned or learned one. */
#include "eval.h"

#include "bitboard.h"

/* Nothing here has been measured: these are starting values for a tuner. */
#define FLAT_OPENING    100
#define FLAT_ENDING     180
#define WALL_VALUE      35
#define CAP_VALUE       80
#define INNER_BONUS     12
#define FRIEND_BONUS    18
#define CAPTIVE_PENALTY 8
#define THREAT_ONE      60
#define THREAT_MANY     220
#define TEMPO_BONUS     20

/* By how far a group is from spanning the board. */
static const int GroupBonus[MAX_SIZE] = {0, 140, 55, 18, 5, 0, 0, 0};

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

static int groups_score(const Board *b, int color) {
    int score = 0;
    u64 left  = board_road_stones(b, color);
    while (left) {
        const u64 group = flood(left & -left, left);
        left &= ~group;
        const int files = files_spanned(group);
        const int ranks = ranks_spanned(group);
        score += GroupBonus[b->size - (files > ranks ? files : ranks)];
    }
    return score;
}

/* Stones under a top stone can be carried away with it, as far as the carry limit. */
static int stacks_score(const Board *b, int color) {
    int score  = 0;
    u64 stacks = b->owned[color];
    while (stacks) {
        const int sq     = pop_lsb(&stacks);
        const int height = b->height[sq];
        if (height < 2) {
            continue;
        }
        const int under   = height - 1 < b->size - 1 ? height - 1 : b->size - 1;
        const u64 window  = (b->stones[sq] >> 1) & ((1ULL << under) - 1);
        const int blacks  = popcount(window);
        const int friends = color == BLACK ? blacks : under - blacks;
        score += friends * FRIEND_BONUS - (under - friends) * CAPTIVE_PENALTY;
    }
    return score;
}

static int side_score(const Board *b, int color, int flat_value, u64 inner) {
    const u64 flats = board_flats(b, color);
    const u64 mine  = b->owned[color];
    return popcount(flats) * flat_value + popcount(mine & b->walls) * WALL_VALUE +
           popcount(mine & b->caps) * CAP_VALUE + popcount(mine & ~b->walls & inner) * INNER_BONUS +
           stacks_score(b, color) + groups_score(b, color);
}

int evaluate(const Board *b, u64 threats) {
    const int start  = board_start_flats(b->size);
    const int fewest = b->flats[WHITE] < b->flats[BLACK] ? b->flats[WHITE] : b->flats[BLACK];
    /* The flat count decides the game as the reserves run out. */
    const int flat_value = FLAT_OPENING + (FLAT_ENDING - FLAT_OPENING) * (start - fewest) / start;

    const u64 edges = FILE_A | RANK_1 | b->file_last | b->rank_last;
    const u64 inner = b->mask & ~edges;

    int score = side_score(b, WHITE, flat_value, inner) - side_score(b, BLACK, flat_value, inner);
    score -= b->halfkomi * flat_value / 2;
    if (b->stm == BLACK) {
        score = -score;
    }

    /* The side to move has already had its chance to win on the spot, so a threat by the
     * other side is one this side still has to answer. */
    if (threats) {
        score -= (threats & (threats - 1)) ? THREAT_MANY : THREAT_ONE;
    }
    return score + TEMPO_BONUS;
}
