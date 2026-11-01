/* movegen.c - placements, and spreads from precomputed pattern lists. */
#include "movegen.h"

#include "bitboard.h"

/* Which spreads a stack has depends on two numbers and nothing else: how many stones it
 * may carry, and how many squares are open. So each (carry, reach) pair owns one list of
 * shifted patterns in a shared pool, and a direction costs a table lookup plus an OR over
 * a run of u16s, which the compiler vectorises for x86 and NEON alike. */
typedef struct {
    u16 start;
    u16 count;
} PatternRun;

/* (carry 1-8) x (reach 0-7). Summed over every pair there are under 5000 entries. */
#define PATTERN_POOL 8192

static u16 Pool[PATTERN_POOL];
static PatternRun Spreads[MAX_SIZE + 1][MAX_SIZE];
/* Same index, for a lone capstone flattening the wall just past `reach` open squares. */
static PatternRun Crushes[MAX_SIZE + 1][MAX_SIZE];

static int pattern_hops(int pattern) { return __builtin_popcount(pattern); }
static int pattern_carry(int pattern) { return 32 - __builtin_clz(pattern); }

void movegen_init(void) {
    int used = 0;
    for (int carry = 1; carry <= MAX_SIZE; carry++) {
        for (int reach = 0; reach < MAX_SIZE; reach++) {
            Spreads[carry][reach].start = (u16)used;
            for (int pattern = 1; pattern < 256; pattern++) {
                if (pattern_carry(pattern) <= carry && pattern_hops(pattern) <= reach) {
                    Pool[used++] = (u16)(pattern << 8);
                }
            }
            Spreads[carry][reach].count = (u16)(used - Spreads[carry][reach].start);

            Crushes[carry][reach].start = (u16)used;
            for (int pattern = 1; pattern < 256; pattern++) {
                const int top_hop_is_single =
                    pattern_carry(pattern) == 1 || ((pattern >> (pattern_carry(pattern) - 2)) & 1);
                if (pattern_carry(pattern) <= carry && pattern_hops(pattern) == reach + 1 &&
                    top_hop_is_single) {
                    Pool[used++] = (u16)(pattern << 8);
                }
            }
            Crushes[carry][reach].count = (u16)(used - Crushes[carry][reach].start);
        }
    }
}

/* Open squares from `sq` before a blocker or the edge. `blockers` includes everything off
 * the board, and each ray ORs in a stop at the edge of the 8x8 layout, which is what
 * bounds it on 8x8 where nothing is off the board. */
static inline int reach_north(u64 blockers, int sq) {
    const u64 ray = (blockers & (FILE_A << (sq & 7))) >> sq >> 8;
    return lsb(ray | 1ULL << (56 - (sq & 56))) / 8;
}

static inline int reach_south(u64 blockers, int sq) {
    const u64 ray = (blockers & (FILE_A << (sq & 7))) << (63 - sq) << 8;
    return __builtin_clzll(ray | 1ULL << (63 - (sq & 56))) / 8;
}

static inline int reach_east(u64 blockers, int sq) {
    const u64 ray = (blockers | FILE_A) >> sq >> 1;
    return lsb(ray | 1ULL << (7 - (sq & 7)));
}

static inline int reach_west(u64 blockers, int sq) {
    const u64 ray = (blockers | FILE_H) << (63 - sq) << 1;
    return __builtin_clzll(ray | 1ULL << (63 - (sq & 7)));
}

static inline void stack_reaches(u64 blockers, int sq, int reach[4]) {
    reach[NORTH] = reach_north(blockers, sq);
    reach[EAST]  = reach_east(blockers, sq);
    reach[SOUTH] = reach_south(blockers, sq);
    reach[WEST]  = reach_west(blockers, sq);
}

/* Is the square just past `reach` open squares a wall? Reach stops at the board edge too,
 * so the target must be checked to be on the board before it means anything. */
static inline bool wall_past(const Board *b, int sq, int dir, int reach) {
    const int rank = sq >> 3, file = sq & 7;
    const int room[4] = {b->size - 1 - rank, b->size - 1 - file, rank, file};
    return reach < room[dir] && (b->walls >> (sq + DirectionStep[dir] * (reach + 1))) & 1;
}

static inline Move *emit_run(Move *restrict out, PatternRun run, u16 base) {
    const u16 *restrict pool = Pool + run.start;
    for (int i = 0; i < run.count; i++) {
        out[i] = pool[i] | base;
    }
    return out + run.count;
}

int movegen_all(const Board *b, Move *list) {
    Move *out    = list;
    const int us = b->stm;
    u64 empty    = board_empty(b);

    if (b->ply < 2) {
        while (empty) {
            *out++ = move_place(pop_lsb(&empty), FLAT);
        }
        return (int)(out - list);
    }

    if (b->flats[us]) {
        u64 squares = empty;
        while (squares) {
            const int sq = pop_lsb(&squares);
            *out++       = move_place(sq, FLAT);
            *out++       = move_place(sq, WALL);
        }
    }
    if (b->capstones[us]) {
        u64 squares = empty;
        while (squares) {
            *out++ = move_place(pop_lsb(&squares), CAP);
        }
    }

    const u64 blockers = b->walls | b->caps | ~b->mask;
    u64 stacks         = b->owned[us];
    while (stacks) {
        const int sq    = pop_lsb(&stacks);
        const int carry = b->height[sq] < b->size ? b->height[sq] : b->size;
        int reach[4];
        stack_reaches(blockers, sq, reach);
        for (int dir = 0; dir < 4; dir++) {
            out = emit_run(out, Spreads[carry][reach[dir]], (u16)(sq | dir << 6));
        }
        if (unlikely((b->caps >> sq) & 1)) {
            for (int dir = 0; dir < 4; dir++) {
                if (wall_past(b, sq, dir, reach[dir])) {
                    out = emit_run(out, Crushes[carry][reach[dir]], (u16)(sq | dir << 6));
                }
            }
        }
    }
    return (int)(out - list);
}

int movegen_count(const Board *b) {
    const int us    = b->stm;
    const int empty = popcount(board_empty(b));

    if (b->ply < 2) {
        return empty;
    }

    int count = empty * ((b->flats[us] ? 2 : 0) + (b->capstones[us] ? 1 : 0));

    const u64 blockers = b->walls | b->caps | ~b->mask;
    u64 stacks         = b->owned[us];
    while (stacks) {
        const int sq    = pop_lsb(&stacks);
        const int carry = b->height[sq] < b->size ? b->height[sq] : b->size;
        int reach[4];
        stack_reaches(blockers, sq, reach);
        for (int dir = 0; dir < 4; dir++) {
            count += Spreads[carry][reach[dir]].count;
        }
        if (unlikely((b->caps >> sq) & 1)) {
            for (int dir = 0; dir < 4; dir++) {
                if (wall_past(b, sq, dir, reach[dir])) {
                    count += Crushes[carry][reach[dir]].count;
                }
            }
        }
    }
    return count;
}

bool movegen_is_legal(const Board *b, Move m) {
    const int sq = move_square(m);
    const int us = b->stm;

    if (!((b->mask >> sq) & 1)) {
        return false;
    }

    if (move_is_place(m)) {
        /* Kind 0 with no pattern is no move at all, not only on a1: any stale word from
         * the table can decode to it. */
        if (move_kind(m) == 0 || !((board_empty(b) >> sq) & 1)) {
            return false;
        }
        const StoneType type = move_stone(m);
        if (b->ply < 2) {
            return type == FLAT;
        }
        return type == CAP ? b->capstones[us] > 0 : b->flats[us] > 0;
    }

    if (b->ply < 2 || !((b->owned[us] >> sq) & 1)) {
        return false;
    }

    const int pattern = move_pattern(m);
    const int carry   = pattern_carry(pattern);
    const int hops    = pattern_hops(pattern);
    if (carry > b->size || carry > b->height[sq]) {
        return false;
    }

    const int dir = move_direction(m);
    int reach[4];
    stack_reaches(b->walls | b->caps | ~b->mask, sq, reach);
    if (hops <= reach[dir]) {
        return true;
    }

    const bool top_hop_is_single = carry == 1 || ((pattern >> (carry - 2)) & 1);
    return hops == reach[dir] + 1 && top_hop_is_single && ((b->caps >> sq) & 1) &&
           wall_past(b, sq, dir, reach[dir]);
}
