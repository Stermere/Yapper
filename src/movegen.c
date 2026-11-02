/* movegen.c - placements, and spreads from precomputed pattern lists. */
#include "movegen.h"

#include "bitboard.h"

/* Which spreads a stack has depends on two numbers and nothing else: how many stones it
 * may carry, and how many squares are open. So each (carry, reach) pair owns one list of
 * shifted patterns in a shared pool, and a direction costs a table lookup plus an OR over
 * a run of u16s, written as vectors that lower to SSE, AVX or NEON alike. */
typedef struct {
    u16 start;
    u16 count;
} PatternRun;

/* (carry 1-8) x (reach 0-7). Summed over every pair there are under 5000 entries, which
 * leaves emit_run() room to read a whole lane past the last. */
#define PATTERN_POOL 8192

/* The runs are mostly a few moves long, and a loop over them stops to think about it at
 * every one. A lane always copies eight, with the move list advanced by what was meant. */
#define LANE_MOVES 8
typedef u16 Lane __attribute__((vector_size(LANE_MOVES * sizeof(u16)), aligned(2), may_alias));

static u16 Pool[PATTERN_POOL];
static PatternRun Spreads[MAX_SIZE + 1][MAX_SIZE];
/* Spreads along a whole line, by carry and a value of LineReach. */
static u16 LineSpreads[MAX_SIZE + 1][128];
/* Same index, for a lone capstone flattening the wall just past `reach` open squares. */
static PatternRun Crushes[MAX_SIZE + 1][MAX_SIZE];

/* The files of the squares in a byte, in order, and a flat then a wall on each of them.
 * Adding the move that file 0 of a rank would make turns these into real moves, so a rank
 * costs the same however many it holds. */
static u16 Files[256][LANE_MOVES];
static u16 FlatsAndWalls[256][2 * LANE_MOVES];

/* Open squares in each direction along a line of eight, from the byte of blockers on that
 * line and a position in it: low nibble towards bit 7, high nibble towards bit 0. */
static u8 LineReach[256][8];

static int pattern_hops(int pattern) { return __builtin_popcount(pattern); }
static int pattern_carry(int pattern) { return 32 - __builtin_clz(pattern); }

static inline int carry_limit(const Board *b, int sq) {
    const int height = board_short_height(b, sq);
    return height < b->size ? height : b->size;
}

void movegen_init(void) {
    for (int empty = 0; empty < 256; empty++) {
        int found = 0;
        for (int file = 0; file < 8; file++) {
            if ((empty >> file) & 1) {
                FlatsAndWalls[empty][2 * found]     = move_place(file, FLAT);
                FlatsAndWalls[empty][2 * found + 1] = move_place(file, WALL);
                Files[empty][found]                 = (u16)file;
                found++;
            }
        }
    }

    for (int line = 0; line < 256; line++) {
        for (int pos = 0; pos < 8; pos++) {
            int up = 0, down = 0;
            while (pos + up < 7 && !((line >> (pos + up + 1)) & 1)) {
                up++;
            }
            while (pos - down > 0 && !((line >> (pos - down - 1)) & 1)) {
                down++;
            }
            LineReach[line][pos] = (u8)(up | down << 4);
        }
    }

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
        for (int up = 0; up < MAX_SIZE; up++) {
            for (int down = 0; down < MAX_SIZE; down++) {
                LineSpreads[carry][up | down << 4] =
                    (u16)(Spreads[carry][up].count + Spreads[carry][down].count);
            }
        }
    }
}

/* A file as a byte, rank 1 lowest. Nothing the multiply adds up can carry. */
static inline int file_byte(u64 squares, int file) {
    return (int)(((squares >> file) & FILE_A) * 0x0102040810204080ULL >> 56);
}

/* Open squares from `sq` before a blocker or the edge. `blockers` includes everything off
 * the board, and the line table stops at the end of the 8x8 layout, which is what bounds
 * it on 8x8 where nothing is off the board. */
static inline void stack_reaches(u64 blockers, int sq, int reach[4]) {
    const int rank = sq >> 3, file = sq & 7;
    const int across = LineReach[(blockers >> (sq & 56)) & 255][file];
    const int along  = LineReach[file_byte(blockers, file)][rank];
    reach[NORTH]     = along & 15;
    reach[EAST]      = across & 15;
    reach[SOUTH]     = along >> 4;
    reach[WEST]      = across >> 4;
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
    int i                    = 0;
    do {
        *(Lane *)(out + i) = *(const Lane *)(pool + i) | base;
        i += LANE_MOVES;
    } while (i < run.count);
    return out + run.count;
}

/* A move from each square of a byte of a rank, given the same move from file 0. */
static inline Move *emit_files(Move *restrict out, int line, u16 first) {
    *(Lane *)out = *(const Lane *)Files[line] + first;
    return out + __builtin_popcount(line);
}

int movegen_all(const Board *b, Move *list) {
    Move *out    = list;
    const int us = board_stm(b);
    u64 empty    = board_empty(b);

    if (b->ply < 2) {
        while (empty) {
            *out++ = move_place(pop_lsb(&empty), FLAT);
        }
        return (int)(out - list);
    }

    if (b->flats[us]) {
        for (int rank = 0; rank < b->size; rank++) {
            const int line   = (int)((empty >> (rank * 8)) & 255);
            const Lane *from = (const Lane *)FlatsAndWalls[line];
            ((Lane *)out)[0] = from[0] + (u16)(rank * 8);
            ((Lane *)out)[1] = from[1] + (u16)(rank * 8);
            out += 2 * __builtin_popcount(line);
        }
    }
    if (b->capstones[us]) {
        for (int rank = 0; rank < b->size; rank++) {
            out = emit_files(out, (int)((empty >> (rank * 8)) & 255), move_place(rank * 8, CAP));
        }
    }

    /* Most stones are alone with no crush to think about, and such a stone has one move per
     * open neighbour, so they go a direction and a rank at a time. Everything else follows,
     * which is not square order: search sees these first. */
    const u64 open      = b->mask & ~(b->walls | b->caps);
    u64 stacks          = b->owned[us] & (b->tall | b->caps);
    const u64 singles   = b->owned[us] ^ stacks;
    const u64 moving[4] = {singles & open >> 8, singles & shift_west(open), singles & open << 8,
                           singles & shift_east(open)};
    for (int dir = 0; dir < 4; dir++) {
        for (int rank = 0; rank < b->size; rank++) {
            out = emit_files(out, (int)((moving[dir] >> (rank * 8)) & 255),
                             move_spread(rank * 8, (Direction)dir, 1));
        }
    }

    const u64 blockers = b->walls | b->caps | ~b->mask;
    while (stacks) {
        const int sq    = pop_lsb(&stacks);
        const int carry = carry_limit(b, sq);
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

/* Out of line, so that the common count does not pay for the registers this needs. */
static __attribute__((noinline)) int crush_count(const Board *b, u64 crushers, u64 blockers) {
    int count = 0;
    while (crushers) {
        const int sq   = pop_lsb(&crushers);
        const int rank = sq >> 3, file = sq & 7;
        const int carry = carry_limit(b, sq);
        int reach[4];
        stack_reaches(blockers, sq, reach);

        /* The square past the reach, as a position on the line. A byte of walls has nothing
         * beyond it, so a reach to the edge finds no wall. */
        const int row = (int)((b->walls >> (sq & 56)) & 255);
        const int col = file_byte(b->walls, file);
        count += Crushes[carry][reach[EAST]].count * ((row >> (file + reach[EAST] + 1)) & 1);
        count += Crushes[carry][reach[WEST]].count * (((row << 1) >> (file - reach[WEST])) & 1);
        count += Crushes[carry][reach[NORTH]].count * ((col >> (rank + reach[NORTH] + 1)) & 1);
        count += Crushes[carry][reach[SOUTH]].count * (((col << 1) >> (rank - reach[SOUTH])) & 1);
    }
    return count;
}

int movegen_count(const Board *b) {
    const int us    = board_stm(b);
    const int empty = popcount(board_empty(b));

    if (b->ply < 2) {
        return empty;
    }

    int count = empty * ((b->flats[us] ? 2 : 0) + (b->capstones[us] ? 1 : 0));

    /* A lone stone without a crush to think about has one move per open neighbour, so
     * every such stone is counted in one go. Those are most of the stones on the board. */
    const u64 open    = b->mask & ~(b->walls | b->caps);
    u64 stacks        = b->owned[us] & (b->tall | b->caps);
    const u64 singles = b->owned[us] ^ stacks;
    count += popcount(singles & open >> 8) + popcount(singles & open << 8) +
             popcount(singles & shift_east(open)) + popcount(singles & shift_west(open));

    const u64 blockers = b->walls | b->caps | ~b->mask;
    while (stacks) {
        const int sq     = pop_lsb(&stacks);
        const int carry  = carry_limit(b, sq);
        const int across = LineReach[(blockers >> (sq & 56)) & 255][sq & 7];
        const int along  = LineReach[file_byte(blockers, sq & 7)][sq >> 3];
        count += LineSpreads[carry][across] + LineSpreads[carry][along];
    }

    const u64 crushers = b->owned[us] & b->caps;
    if (crushers && b->walls) {
        count += crush_count(b, crushers, blockers);
    }
    return count;
}

/* A stack's square and the next `hops` squares of a direction, all known to be on the board. */
static inline u64 line_from(int sq, int dir, int hops) {
    const u64 across = (2ULL << hops) - 1;
    const u64 along  = FILE_A & (~0ULL >> (56 - 8 * hops));
    switch (dir) {
    case NORTH: return along << sq;
    case EAST: return across << sq;
    case SOUTH: return along << (sq - 8 * hops);
    default: return across << (sq - hops);
    }
}

/* A road's first new square touches one end of it and its last touches the other. */
static inline bool line_joins(u64 line, const u64 ends[4]) {
    return ((line & ends[0]) && (line & ends[1])) || ((line & ends[2]) && (line & ends[3]));
}

/* Would this spread finish a road through `area`, the road stones of `us`? A tall stack has
 * dozens of patterns on a line that gets this far, nearly all for nothing, so what each one
 * leaves on top is read off the stack's bits rather than played. A stone a pattern ends a hop
 * on is the top of that square. */
static inline bool spread_wins(const Board *b, Move m, u64 area, int us) {
    const int sq    = move_square(m);
    const int step  = DirectionStep[move_direction(m)];
    const int carry = move_carry(m);
    const u64 word  = b->stones[sq];

    u64 after = area & ~(1ULL << sq);
    if (board_short_height(b, sq) > carry && ((word >> carry) & 1) == (u64)us) {
        after |= 1ULL << sq;
    }
    int to = sq;
    for (int hops = move_pattern(m); hops; hops &= hops - 1) {
        to += step;
        after &= ~(1ULL << to);
        after |= (u64)(((word >> (carry - 1 - __builtin_ctz(hops))) & 1) == (u64)us) << to;
    }
    if ((b->walls >> sq) & 1) {
        after &= ~(1ULL << to);
    }
    return board_road_through(b, after, after & ~area);
}

bool movegen_road_spread(const Board *b, int us, const u64 ends[4]) {
    const u64 area   = board_road_stones(b, us);
    const u64 tops   = b->walls | b->caps;
    const u64 stacks = b->owned[us] & (b->tall | b->caps);

    /* A lone stone adds one square to a road, so it has to land on a gap. A lone wall adds
     * nothing at all. */
    const u64 singles   = b->owned[us] & ~(stacks | b->walls);
    const u64 gaps      = board_road_gaps(ends) & b->mask & ~tops;
    const u64 moving[4] = {singles & gaps >> 8, singles & shift_west(gaps), singles & gaps << 8,
                           singles & shift_east(gaps)};
    for (int dir = 0; dir < 4; dir++) {
        u64 stones = moving[dir];
        while (stones) {
            if (spread_wins(b, move_spread(pop_lsb(&stones), (Direction)dir, 1), area, us)) {
                return true;
            }
        }
    }

    /* Anything else has to cover a line touching both ends of a road, which all but a few
     * of its directions do not. */
    const u64 blockers = tops | ~b->mask;
    u64 rest           = stacks;
    while (rest) {
        const int sq    = pop_lsb(&rest);
        const int carry = carry_limit(b, sq);
        int reach[4];
        stack_reaches(blockers, sq, reach);
        for (int dir = 0; dir < 4; dir++) {
            const bool crush = ((b->caps >> sq) & 1) && wall_past(b, sq, dir, reach[dir]);
            const int open   = reach[dir] + crush;
            const int hops   = open < carry ? open : carry;
            if (hops == 0) {
                continue;
            }
            if (!line_joins(line_from(sq, dir, hops), ends)) {
                continue;
            }
            Move list[2 * 255 + LANE_MOVES];
            const u16 base = (u16)(sq | dir << 6);
            Move *end      = emit_run(list, Spreads[carry][reach[dir]], base);
            if (crush) {
                end = emit_run(end, Crushes[carry][reach[dir]], base);
            }
            for (const Move *m = list; m < end; m++) {
                if (spread_wins(b, *m, area, us)) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool movegen_is_legal(const Board *b, Move m) {
    const int sq = move_square(m);
    const int us = board_stm(b);

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
    if (carry > carry_limit(b, sq)) {
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
