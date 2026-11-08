/* board.c - make/unmake and the rules of how a game ends. */
#include "board.h"

#include <assert.h>
#include <string.h>

#include "bitboard.h"

/* Per the USTAK rules: {stones, capstones}. 3x3 is outside them but is standard PTN, and
 * small enough to exercise the end-of-game rules exhaustively. */
static const u8 StartReserves[MAX_SIZE + 1][2] = {
    [3] = {10, 0}, [4] = {15, 0}, [5] = {21, 1}, [6] = {30, 1}, [7] = {40, 2}, [8] = {50, 2},
};

/* The hash is the sum over the squares of each stack's word times the key of its square,
 * plus a key per wall or capstone. Moving stones then costs one multiply for each square
 * touched, however many stones it moves, and no table has to span every height. A sum
 * says little in its low bits, so a reader that needs those has to mix it first. */
static u64 StackKeys[SQUARE_COUNT];
static u64 TowerKeys[SQUARE_COUNT];
static u64 TypeKeys[SQUARE_COUNT][3]; /* zero for a flat */
/* An empty square turning into a lone stone, by square, type and colour. */
static u64 PlaceKeys[SQUARE_COUNT][3][2];
static u64 SideKey;

/* Fixed seed. Keys taken from any entropy source would make bench unrepeatable. */
static u64 splitmix64(u64 *state) {
    u64 z = (*state += 0x9E3779B97F4A7C15ULL);
    z     = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z     = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

void board_init(void) {
    u64 state = 0x59415050455221ULL; /* "YAPPER!" */
    for (int sq = 0; sq < SQUARE_COUNT; sq++) {
        StackKeys[sq]      = splitmix64(&state);
        TowerKeys[sq]      = splitmix64(&state);
        TypeKeys[sq][FLAT] = 0;
        TypeKeys[sq][WALL] = splitmix64(&state);
        TypeKeys[sq][CAP]  = splitmix64(&state);
        for (int top = 0; top < 3; top++) {
            for (int color = 0; color < 2; color++) {
                /* From a word of 1 to a word of 2 or 3. */
                PlaceKeys[sq][top][color] = StackKeys[sq] * (u64)(1 + color) + TypeKeys[sq][top];
            }
        }
    }
    SideKey = splitmix64(&state);
}

int board_start_flats(int size) { return StartReserves[size][0]; }
int board_start_capstones(int size) { return StartReserves[size][1]; }

static inline u64 stack_key(const Board *b, int sq) {
    return StackKeys[sq] * b->stones[sq] + TowerKeys[sq] * b->deep[sq];
}

u64 board_compute_hash(const Board *b) {
    u64 hash    = 0;
    u64 squares = b->mask;
    while (squares) {
        const int sq = pop_lsb(&squares);
        hash += stack_key(b, sq) + TypeKeys[sq][board_top(b, sq)];
    }
    return hash;
}

u64 board_key(const Board *b) { return b->hash + (SideKey & -(u64)board_stm(b)); }

void board_clear(Board *b, int size, int halfkomi) {
    assert(size >= MIN_SIZE && size <= MAX_SIZE);
    memset(b, 0, sizeof(*b));
    for (int sq = 0; sq < SQUARE_COUNT; sq++) {
        b->stones[sq] = 1;
    }
    b->size        = size;
    b->halfkomi    = halfkomi;
    const u64 rank = size == 8 ? RANK_1 : (1ULL << size) - 1;
    b->mask        = 0;
    for (int r = 0; r < size; r++) {
        b->mask |= rank << (r * 8);
    }
    b->file_last = FILE_A << (size - 1);
    b->rank_last = RANK_1 << ((size - 1) * 8);
    /* Bit 7 of every rank below the last, and every file below the last: see
     * road_conceivable(). */
    b->rank_links = 0x8080808080808080ULL & ((1ULL << ((size - 1) * 8)) - 1);
    b->file_links = (1ULL << (size - 1)) - 1;
    for (int color = 0; color < 2; color++) {
        b->flats[color]     = StartReserves[size][0];
        b->capstones[color] = StartReserves[size][1];
    }
    memcpy(b->stack_keys, StackKeys, sizeof(StackKeys));
    b->hash = board_compute_hash(b);
}

#define TOWER_FLAG (1ULL << 63)

/* A stack as one 128-bit shift register, which is what `stones` and `deep` are unless the
 * stones run past the first word. */
typedef struct {
    u64 high;
    u64 low;
} Tower;

static inline Tower tower_load(const Board *b, int sq) {
    const u64 stones = b->stones[sq];
    const u64 deep   = b->deep[sq];
    if (deep == 0) {
        return (Tower){0, stones};
    }
    return (Tower){deep >> 1, (stones & ~TOWER_FLAG) | deep << 63};
}

static inline void tower_store(Board *b, int sq, Tower t) {
    b->stones[sq] = t.high ? t.low | TOWER_FLAG : t.low;
    b->deep[sq]   = t.high ? t.high << 1 | t.low >> 63 : 0;
}

/* k and d are at most 8, so every shift below is in range. */
static void tower_push(Board *b, int sq, u64 chunk, int d) {
    Tower t = tower_load(b, sq);
    t.high  = t.high << d | t.low >> (64 - d);
    t.low   = t.low << d | chunk;
    tower_store(b, sq, t);
}

static u64 tower_pop(Board *b, int sq, int k) {
    Tower t         = tower_load(b, sq);
    const u64 taken = t.low & ((1ULL << k) - 1);
    t.low           = t.low >> k | t.high << (64 - k);
    t.high >>= k;
    tower_store(b, sq, t);
    return taken;
}

static bool has_towers(const Board *b) {
    u64 words = 0;
    for (int sq = 0; sq < SQUARE_COUNT; sq++) {
        words |= b->stones[sq];
    }
    return (words >> TOWER_BITS) != 0;
}

void board_set_stack(Board *b, int sq, const u8 *colors, int height, StoneType top) {
    for (int i = 0; i < height; i++) {
        tower_push(b, sq, colors[i], 1);
    }
    b->walls |= (u64)(top == WALL) << sq;
    b->caps |= (u64)(top == CAP) << sq;
}

void board_refresh(Board *b) {
    int stones[2] = {0, 0}, capstones[2] = {0, 0};
    b->owned[WHITE] = b->owned[BLACK] = 0;
    b->tall                           = 0;
    b->towers                         = has_towers(b);
    u64 squares                       = b->mask;
    while (squares) {
        const int sq     = pop_lsb(&squares);
        const int height = board_height(b, sq);
        if (height == 0) {
            continue;
        }
        const int top = (int)(b->stones[sq] & 1);
        b->owned[top] |= 1ULL << sq;
        b->tall |= (u64)(height > 1) << sq;
        /* Less the 1 marking the height, in each word in use. */
        const int blacks =
            popcount(b->stones[sq]) - 1 + (b->deep[sq] ? popcount(b->deep[sq]) - 1 : 0);
        stones[BLACK] += blacks;
        stones[WHITE] += height - blacks;
        capstones[top] += (b->caps >> sq) & 1;
    }
    for (int color = 0; color < 2; color++) {
        const int flats     = StartReserves[b->size][0] - (stones[color] - capstones[color]);
        const int caps      = StartReserves[b->size][1] - capstones[color];
        b->flats[color]     = (u8)(flats < 0 ? 0 : flats);
        b->capstones[color] = (u8)(caps < 0 ? 0 : caps);
    }
    b->hash = board_compute_hash(b);
}

static inline void make_place(Board *b, Move m) {
    const int sq         = move_square(m);
    const StoneType type = move_stone(m);
    /* Each player's first stone is the opponent's. */
    const int color = board_stm(b) ^ (b->ply < 2);
    if (type == CAP) {
        b->capstones[color]--;
    } else {
        b->flats[color]--;
    }
    b->stones[sq] = 2 | (u64)color;
    b->owned[color] |= 1ULL << sq;
    if (type != FLAT) {
        b->walls |= (u64)(type == WALL) << sq;
        b->caps |= (u64)(type == CAP) << sq;
    }
    b->hash += PlaceKeys[sq][type][color];
    b->reversible = 0;
}

static inline void state_copy(PositionState *restrict to, const PositionState *restrict from) {
    const StateHalf *restrict in = (const StateHalf *)from;
    StateHalf *restrict out      = (StateHalf *)to;
    out[0]                       = in[0];
    out[1]                       = in[1];
}

/* Written once and built twice. A tower needs calls in the middle of the loop, and those
 * are what would have every other spread saving registers, so the common build never makes
 * one and the board tells it in advance when it would have to. Returns what unmake needs.
 * It takes no Undo, and neither does its counterpart: an Undo whose address never escapes
 * can live in registers across a whole loop over the moves of a node. */
static inline __attribute__((always_inline)) u64 spread(Board *b, Move m, bool towers) {
    const int src  = move_square(m);
    const int step = DirectionStep[move_direction(m)];
    int pattern    = move_pattern(m);
    int left       = move_carry(m);
    u64 hash       = b->hash;

    const u64 stones = b->stones[src];
    /* Only the low `left` bits of this are ever read below, so the fast build has no need
     * to mask off the stones that stay. */
    u64 carried = stones, rest;
    if (towers) {
        hash -= stack_key(b, src);
        carried = tower_pop(b, src, left);
        rest    = b->stones[src];
        hash += stack_key(b, src);
    } else {
        rest           = stones >> left;
        b->stones[src] = rest;
        hash += (rest - stones) * b->stack_keys[src];
    }

    /* Which side's squares change is as random as the stones, so the owners are worked out
     * in registers and written once. */
    const u64 srcbit = 1ULL << src;
    const u64 stays  = rest > 1 ? srcbit : 0;
    u64 black        = (rest & 1 ? srcbit : 0) & stays;
    u64 tall         = rest > 3 ? srcbit : 0;
    u64 landed       = 0;
    u64 highest      = 0;

    int sq = src;
    do {
        const int d = __builtin_ctz(pattern) + 1;
        pattern >>= d;
        sq += step;
        left -= d;

        /* The bottom of the carried group lands first. */
        const u64 chunk = (carried >> left) & ((1ULL << d) - 1);
        const u64 under = b->stones[sq];
        u64 grown;
        if (towers) {
            hash -= stack_key(b, sq);
            tower_push(b, sq, chunk, d);
            hash += stack_key(b, sq);
            grown = b->stones[sq];
        } else {
            grown         = under << d | chunk;
            b->stones[sq] = grown;
            hash += (grown - under) * b->stack_keys[sq];
        }

        const u64 bit = 1ULL << sq;
        landed |= bit;
        black |= (grown & 1) << sq;
        tall |= grown > 3 ? bit : 0;
        highest |= grown;
    } while (pattern);

    const u64 touched = landed | srcbit;
    b->owned[WHITE]   = (b->owned[WHITE] & ~touched) | ((landed | stays) & ~black);
    b->owned[BLACK]   = (b->owned[BLACK] & ~touched) | black;
    b->tall           = (b->tall & ~touched) | tall;
    b->reversible++;

    /* A flat on top changes neither set below, and can land on nothing that would. */
    const u64 walls = b->walls;
    const u64 caps  = b->caps;
    if (unlikely(((walls | caps) & srcbit) != 0)) {
        const u64 dest        = 1ULL << sq;
        const u64 is_wall     = (walls >> src) & 1;
        const u64 is_cap      = (caps >> src) & 1;
        const StoneType moved = is_wall ? WALL : CAP;
        hash += TypeKeys[sq][moved] - TypeKeys[src][moved];
        /* The generator only reaches a wall with a lone capstone, so this is the flattening
         * and nothing else. */
        if (walls & dest) {
            hash -= TypeKeys[sq][WALL];
            b->reversible = 0;
        }
        b->walls = (walls & ~(srcbit | dest)) | is_wall << sq;
        b->caps  = (caps & ~srcbit) | is_cap << sq;
    }
    b->hash = hash;

    if (towers) {
        b->towers = has_towers(b);
        return carried;
    }
    if (unlikely(highest >> TOWER_BITS)) {
        b->towers = 1;
    }
    return stones;
}

static __attribute__((noinline)) u64 make_tower_spread(Board *b, Move m) {
    return spread(b, m, true);
}

static inline void make_spread(Board *b, Move m, Undo *u) {
    u->lifted = unlikely(b->towers) ? make_tower_spread(b, m) : spread(b, m, false);
}

void board_save(const Board *b, Undo *u) { state_copy(&u->state, &b->state); }

void board_play_place(Board *b, Move m) {
    make_place(b, m);
    b->ply++;
}

void board_play_spread(Board *b, Move m, Undo *u) {
    make_spread(b, m, u);
    b->ply++;
}

void board_play_step(Board *b, Move m) {
    const int src   = move_square(m);
    const int sq    = src + DirectionStep[move_direction(m)];
    const int us    = board_stm(b);
    const u64 from  = 1ULL << src;
    const u64 to    = 1ULL << sq;
    const u64 under = b->stones[sq];
    const u64 grown = under << 1 | (u64)us;
    b->stones[src]  = 1;
    b->stones[sq]   = grown;
    b->hash += (grown - under) * b->stack_keys[sq] - (u64)(us + 1) * b->stack_keys[src];
    b->owned[us] = (b->owned[us] ^ from) | to;
    b->owned[us ^ 1] &= ~to;
    b->tall |= (u64)(under > 1) << sq;
    /* A wall can only land where a flat would, so nothing is ever flattened here. */
    if (unlikely(b->walls & from)) {
        b->hash += TypeKeys[sq][WALL] - TypeKeys[src][WALL];
        b->walls ^= from | to;
    }
    if (unlikely(grown >> TOWER_BITS)) {
        b->towers = 1;
    }
    b->reversible++;
    b->ply++;
}

void board_play(Board *b, Move m, Undo *u) {
    if (move_is_place(m)) {
        board_play_place(b, m);
    } else if (board_is_step(b, m)) {
        board_play_step(b, m);
    } else {
        board_play_spread(b, m, u);
    }
}

void board_make(Board *b, Move m, Undo *u) {
    board_save(b, u);
    board_play(b, m, u);
}

static inline __attribute__((always_inline)) void undo_spread(Board *b, Move m, u64 lifted,
                                                              bool towers) {
    const int src  = move_square(m);
    const int step = DirectionStep[move_direction(m)];
    int pattern    = move_pattern(m);
    int sq         = src;
    do {
        const int d = __builtin_ctz(pattern) + 1;
        pattern >>= d;
        sq += step;
        if (towers) {
            tower_pop(b, sq, d);
        } else {
            b->stones[sq] >>= d;
        }
    } while (pattern);

    if (towers) {
        tower_push(b, src, lifted, move_carry(m));
    } else {
        b->stones[src] = lifted;
    }
}

static __attribute__((noinline)) void unmake_tower_spread(Board *b, Move m, u64 lifted) {
    undo_spread(b, m, lifted, true);
}

static inline void unmake_spread(Board *b, Move m, const Undo *u) {
    /* Whether the move made a tower does not matter: it pops what it pushed. */
    if (unlikely(u->towers)) {
        unmake_tower_spread(b, m, u->lifted);
    } else {
        undo_spread(b, m, u->lifted, false);
    }
}

void board_unmake_place(Board *b, Move m, const Undo *u) {
    b->stones[move_square(m)] = 1;
    state_copy(&b->state, &u->state);
}

void board_unmake_spread(Board *b, Move m, const Undo *u) {
    unmake_spread(b, m, u);
    state_copy(&b->state, &u->state);
}

void board_unmake_step(Board *b, Move m, const Undo *u) {
    const int src = move_square(m);
    const int sq  = src + DirectionStep[move_direction(m)];
    b->stones[sq] >>= 1;
    b->stones[src] = 2 | (u64)(u->ply & 1);
    state_copy(&b->state, &u->state);
}

void board_unmake(Board *b, Move m, const Undo *u) {
    const int src = move_square(m);
    if (move_is_place(m)) {
        board_unmake_place(b, m, u);
    } else if (move_pattern(m) == 1 && b->stones[src] == 1 && !u->towers &&
               !((u->caps >> src) & 1)) {
        /* board_is_step(), asked of the position as it was. */
        board_unmake_step(b, m, u);
    } else {
        board_unmake_spread(b, m, u);
    }
}

void board_make_null(Board *b, Undo *u) {
    u->reversible = b->reversible;
    b->ply++;
    b->reversible = 0;
}

void board_unmake_null(Board *b, const Undo *u) {
    b->ply--;
    b->reversible = u->reversible;
}

static inline bool group_spans(const Board *b, u64 group) {
    return ((group & FILE_A) && (group & b->file_last)) ||
           ((group & RANK_1) && (group & b->rank_last));
}

/* A road has to cross from every rank to the next, or from every file to the next, along
 * a pair of adjacent stones. Testing that is a handful of operations and no branches, and
 * it rules out most positions long before a flood could. */
static inline bool road_conceivable(const Board *b, u64 area) {
    const u64 low = 0x7F7F7F7F7F7F7F7FULL;
    /* Bit 7 of a byte ends up set if any bit of that byte is. */
    const u64 north = area & area >> 8;
    const u64 ranks = ((north & low) + low) | north;

    u64 east = area & area >> 1 & ~FILE_H;
    east |= east >> 32;
    east |= east >> 16;
    east |= east >> 8;

    return ((ranks & b->rank_links) == b->rank_links) | ((east & b->file_links) == b->file_links);
}

/* Out of line: almost no position gets this far, and the rest should not pay for its
 * registers. */
static __attribute__((noinline)) bool road_flood(const Board *b, u64 area, u64 seeds) {
    while (seeds) {
        const u64 group = flood(seeds & -seeds, area);
        if (group_spans(b, group)) {
            return true;
        }
        seeds &= ~group;
    }
    return false;
}

/* Is any square of `seeds` part of a road through `area`? */
static inline bool road_through(const Board *b, u64 area, u64 seeds) {
    return seeds && road_conceivable(b, area) && road_flood(b, area, seeds);
}

static inline Outcome flat_outcome(const Board *b) {
    if (board_empty(b) != 0 && (b->flats[WHITE] | b->capstones[WHITE]) != 0 &&
        (b->flats[BLACK] | b->capstones[BLACK]) != 0) {
        return ONGOING;
    }
    const int lead = board_flat_lead(b, WHITE);
    return lead > 0 ? WHITE_FLATS : lead < 0 ? BLACK_FLATS : DRAW;
}

Outcome board_outcome_after(const Board *b, const Undo *u) {
    const u64 white       = board_road_stones(b, WHITE);
    const u64 black       = board_road_stones(b, BLACK);
    const u64 fresh_white = white & ~(u->owned[WHITE] & ~u->walls);
    const u64 fresh_black = black & ~(u->owned[BLACK] & ~u->walls);

    /* A move can complete both roads at once. The mover wins that, and stm is already the
     * other player here, so the mover's road is the one that must be asked about first. */
    const int mover = !board_stm(b);
    if (road_through(b, mover == WHITE ? white : black,
                     mover == WHITE ? fresh_white : fresh_black)) {
        return mover == WHITE ? WHITE_ROAD : BLACK_ROAD;
    }
    if (road_through(b, mover == WHITE ? black : white,
                     mover == WHITE ? fresh_black : fresh_white)) {
        return mover == WHITE ? BLACK_ROAD : WHITE_ROAD;
    }
    return flat_outcome(b);
}

Outcome board_outcome_after_place(const Board *b, Move m) {
    /* In the first two plies the stone is the other player's, and then `seed` is empty. No
     * road is two stones long, so that is the right answer for those too. */
    const int mover = !board_stm(b);
    const u64 mine  = board_road_stones(b, mover);
    const u64 seed  = mine & 1ULL << move_square(m);
    if (seed && road_conceivable(b, mine) && road_flood(b, mine, seed)) {
        return mover == WHITE ? WHITE_ROAD : BLACK_ROAD;
    }
    return flat_outcome(b);
}

Outcome board_outcome(const Board *b) {
    const u64 white  = board_road_stones(b, WHITE);
    const u64 black  = board_road_stones(b, BLACK);
    const int mover  = !board_stm(b);
    const u64 mine   = mover == WHITE ? white : black;
    const u64 theirs = mover == WHITE ? black : white;
    const u64 edges  = (FILE_A | RANK_1) & b->mask;
    if (road_through(b, mine, mine & edges)) {
        return mover == WHITE ? WHITE_ROAD : BLACK_ROAD;
    }
    if (road_through(b, theirs, theirs & edges)) {
        return mover == WHITE ? BLACK_ROAD : WHITE_ROAD;
    }
    return flat_outcome(b);
}

bool board_road_through(const Board *b, u64 area, u64 seeds) {
    return road_through(b, area, seeds);
}

void board_road_ends(const Board *b, int color, u64 ends[4]) {
    const u64 area  = board_road_stones(b, color);
    const u64 west  = FILE_A & b->mask;
    const u64 south = RANK_1 & b->mask;
    ends[0]         = neighbours(flood(west, area)) | west;
    ends[1]         = neighbours(flood(b->file_last, area)) | b->file_last;
    ends[2]         = neighbours(flood(south, area)) | south;
    ends[3]         = neighbours(flood(b->rank_last, area)) | b->rank_last;
}

u64 board_road_near(const u64 ends[4], u64 empty) {
    return ((ends[0] & neighbours(ends[1] & empty)) | (ends[1] & neighbours(ends[0] & empty)) |
            (ends[2] & neighbours(ends[3] & empty)) | (ends[3] & neighbours(ends[2] & empty))) &
           empty;
}

u64 board_road_placements(const Board *b, int color) {
    u64 ends[4];
    board_road_ends(b, color, ends);
    return board_road_gaps(ends) & board_empty(b);
}
