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

static u64 SquareKeys[SQUARE_COUNT][3];
static u64 HeightKeys[MAX_STONES + 1];
static u64 EmptyKeys[SQUARE_COUNT];
static u64 SideKey;

/* Fixed seed. Keys taken from any entropy source would make bench unrepeatable. */
static u64 splitmix64(u64 *state) {
    u64 z = (*state += 0x9E3779B97F4A7C15ULL);
    z     = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z     = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* A square's contribution is a function of its whole stack, not a sum over its stones, so
 * a spread costs per square touched however many stones it moves, and no table has to
 * span every height a stack can reach. */
static inline u64 key_start(int sq, StoneType top, u64 stones) {
    const u64 x = (stones ^ SquareKeys[sq][top]) * 0x9E3779B97F4A7C15ULL;
    return x ^ (x >> 32);
}

static inline u64 key_finish(u64 x, int height) {
    x = (x ^ HeightKeys[height]) * 0xD6E8FEB86659FD93ULL;
    return x ^ (x >> 32);
}

static inline u64 square_key(int sq, StoneType top, u64 stones, int height) {
    return key_finish(key_start(sq, top, stones), height);
}

static inline u64 square_key_of(const Board *b, int sq, StoneType top) {
    const int height = b->height[sq];
    u64 x            = key_start(sq, top, b->stones[sq]);
    if (unlikely(height > 64)) {
        x ^= b->deep[sq] * 0xFF51AFD7ED558CCDULL;
    }
    return key_finish(x, height);
}

void board_init(void) {
    u64 state = 0x59415050455221ULL; /* "YAPPER!" */
    for (int sq = 0; sq < SQUARE_COUNT; sq++) {
        for (int top = 0; top < 3; top++) {
            SquareKeys[sq][top] = splitmix64(&state);
        }
    }
    for (int h = 0; h <= MAX_STONES; h++) {
        HeightKeys[h] = splitmix64(&state);
    }
    for (int sq = 0; sq < SQUARE_COUNT; sq++) {
        EmptyKeys[sq] = square_key(sq, FLAT, 0, 0);
    }
    SideKey = splitmix64(&state);
}

int board_start_flats(int size) { return StartReserves[size][0]; }
int board_start_capstones(int size) { return StartReserves[size][1]; }

u64 board_compute_hash(const Board *b) {
    u64 hash    = b->stm == BLACK ? SideKey : 0;
    u64 squares = b->mask;
    while (squares) {
        const int sq = pop_lsb(&squares);
        hash ^= square_key_of(b, sq, board_top(b, sq));
    }
    return hash;
}

void board_clear(Board *b, int size, int halfkomi) {
    assert(size >= MIN_SIZE && size <= MAX_SIZE);
    memset(b, 0, sizeof(*b));
    b->size        = size;
    b->halfkomi    = halfkomi;
    const u64 rank = size == 8 ? RANK_1 : (1ULL << size) - 1;
    b->mask        = 0;
    for (int r = 0; r < size; r++) {
        b->mask |= rank << (r * 8);
    }
    b->file_last = FILE_A << (size - 1);
    b->rank_last = RANK_1 << ((size - 1) * 8);
    for (int color = 0; color < 2; color++) {
        b->flats[color]     = StartReserves[size][0];
        b->capstones[color] = StartReserves[size][1];
    }
    b->hash = board_compute_hash(b);
}

void board_set_stack(Board *b, int sq, const u8 *colors, int height, StoneType top) {
    u64 stones = 0, deep = 0;
    for (int i = 0; i < height; i++) {
        deep   = deep << 1 | stones >> 63;
        stones = stones << 1 | colors[i];
    }
    b->stones[sq] = stones;
    b->deep[sq]   = deep;
    b->height[sq] = (u8)height;
    b->walls |= (u64)(top == WALL) << sq;
    b->caps |= (u64)(top == CAP) << sq;
}

void board_refresh(Board *b) {
    int stones[2] = {0, 0}, capstones[2] = {0, 0};
    b->owned[WHITE] = b->owned[BLACK] = 0;
    u64 squares                       = b->mask;
    while (squares) {
        const int sq     = pop_lsb(&squares);
        const int height = b->height[sq];
        if (height == 0) {
            continue;
        }
        const int top = (int)(b->stones[sq] & 1);
        b->owned[top] |= 1ULL << sq;
        const int blacks = popcount(b->stones[sq]) + popcount(b->deep[sq]);
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

/* k and d are at most 8, so every shift below is in range. */
static inline void stack_push(Board *b, int sq, u64 chunk, int d) {
    const int height = b->height[sq];
    if (unlikely(height + d > 64)) {
        b->deep[sq] = b->deep[sq] << d | b->stones[sq] >> (64 - d);
    }
    b->stones[sq] = b->stones[sq] << d | chunk;
    b->height[sq] = (u8)(height + d);
}

static inline u64 stack_pop(Board *b, int sq, int k) {
    const u64 taken = b->stones[sq] & ((1ULL << k) - 1);
    b->stones[sq] >>= k;
    if (unlikely(b->height[sq] > 64)) {
        b->stones[sq] |= b->deep[sq] << (64 - k);
        b->deep[sq] >>= k;
    }
    b->height[sq] = (u8)(b->height[sq] - k);
    return taken;
}

static inline void make_place(Board *b, Move m) {
    const int sq         = move_square(m);
    const StoneType type = move_stone(m);
    /* Each player's first stone is the opponent's. */
    const int color = b->stm ^ (b->ply < 2);
    if (type == CAP) {
        b->capstones[color]--;
    } else {
        b->flats[color]--;
    }
    b->stones[sq] = (u64)color;
    b->height[sq] = 1;
    b->owned[color] |= 1ULL << sq;
    b->walls |= (u64)(type == WALL) << sq;
    b->caps |= (u64)(type == CAP) << sq;
    b->hash ^= EmptyKeys[sq] ^ square_key(sq, type, (u64)color, 1);
    b->reversible = 0;
}

static inline void make_spread(Board *b, Move m) {
    const int src         = move_square(m);
    const int step        = DirectionStep[move_direction(m)];
    int pattern           = move_pattern(m);
    int left              = move_carry(m);
    const u64 srcbit      = 1ULL << src;
    const StoneType moved = board_top(b, src);
    u64 hash              = b->hash;

    hash ^= square_key_of(b, src, moved);
    u64 carried = stack_pop(b, src, left);
    hash ^= square_key_of(b, src, FLAT);

    b->walls &= ~srcbit;
    b->caps &= ~srcbit;
    b->owned[WHITE] &= ~srcbit;
    b->owned[BLACK] &= ~srcbit;
    b->owned[b->stones[src] & 1] |= (u64)(b->height[src] != 0) << src;

    int sq  = src;
    u64 bit = 0;
    do {
        const int d = __builtin_ctz(pattern) + 1;
        pattern >>= d;
        sq += step;
        left -= d;
        bit = 1ULL << sq;

        /* The bottom of the carried group lands first. */
        const u64 chunk = carried >> left;
        carried &= (1ULL << left) - 1;

        hash ^= square_key_of(b, sq, (b->walls >> sq) & 1 ? WALL : FLAT);
        stack_push(b, sq, chunk, d);
        hash ^= square_key_of(b, sq, pattern ? FLAT : moved);

        const int top = (int)(chunk & 1);
        b->owned[top] |= bit;
        b->owned[!top] &= ~bit;
    } while (pattern);

    /* The generator only reaches a wall with a lone capstone, so this is the flattening
     * and nothing else. */
    b->reversible = (b->walls & bit) ? 0 : b->reversible + 1;
    b->walls &= ~bit;
    b->walls |= (u64)(moved == WALL) << sq;
    b->caps |= (u64)(moved == CAP) << sq;
    b->hash = hash;
}

void board_make(Board *b, Move m, Undo *u) {
    u->owned[WHITE] = b->owned[WHITE];
    u->owned[BLACK] = b->owned[BLACK];
    u->walls        = b->walls;
    u->caps         = b->caps;
    u->hash         = b->hash;
    u->reversible   = b->reversible;

    if (move_is_place(m)) {
        make_place(b, m);
    } else {
        make_spread(b, m);
    }
    b->ply++;
    b->stm ^= 1;
    b->hash ^= SideKey;
}

void board_unmake(Board *b, Move m, const Undo *u) {
    b->ply--;
    b->stm ^= 1;

    if (move_is_place(m)) {
        const int sq    = move_square(m);
        const int color = b->stm ^ (b->ply < 2);
        if (move_stone(m) == CAP) {
            b->capstones[color]++;
        } else {
            b->flats[color]++;
        }
        b->stones[sq] = 0;
        b->height[sq] = 0;
    } else {
        const int src   = move_square(m);
        const int step  = DirectionStep[move_direction(m)];
        int pattern     = move_pattern(m);
        const int carry = move_carry(m);
        int left        = carry;
        int sq          = src;
        u64 carried     = 0;
        do {
            const int d = __builtin_ctz(pattern) + 1;
            pattern >>= d;
            sq += step;
            left -= d;
            carried |= stack_pop(b, sq, d) << left;
        } while (pattern);
        stack_push(b, src, carried, carry);
    }

    b->owned[WHITE] = u->owned[WHITE];
    b->owned[BLACK] = u->owned[BLACK];
    b->walls        = u->walls;
    b->caps         = u->caps;
    b->hash         = u->hash;
    b->reversible   = u->reversible;
}

void board_make_null(Board *b, Undo *u) {
    u->hash       = b->hash;
    u->reversible = b->reversible;
    b->ply++;
    b->stm ^= 1;
    b->hash ^= SideKey;
    b->reversible = 0;
}

void board_unmake_null(Board *b, const Undo *u) {
    b->ply--;
    b->stm ^= 1;
    b->hash       = u->hash;
    b->reversible = u->reversible;
}

static inline bool group_spans(const Board *b, u64 group) {
    return ((group & FILE_A) && (group & b->file_last)) ||
           ((group & RANK_1) && (group & b->rank_last));
}

/* Is any square of `seeds` part of a road through `area`? */
static inline bool road_through(const Board *b, u64 area, u64 seeds) {
    while (seeds) {
        const u64 group = flood(seeds & -seeds, area);
        if (group_spans(b, group)) {
            return true;
        }
        seeds &= ~group;
    }
    return false;
}

static inline Outcome flat_outcome(const Board *b) {
    if (board_empty(b) != 0 && (b->flats[WHITE] | b->capstones[WHITE]) != 0 &&
        (b->flats[BLACK] | b->capstones[BLACK]) != 0) {
        return ONGOING;
    }
    const int lead =
        2 * (popcount(board_flats(b, WHITE)) - popcount(board_flats(b, BLACK))) - b->halfkomi;
    return lead > 0 ? WHITE_FLATS : lead < 0 ? BLACK_FLATS : DRAW;
}

Outcome board_outcome_after(const Board *b, const Undo *u) {
    const u64 white       = board_road_stones(b, WHITE);
    const u64 black       = board_road_stones(b, BLACK);
    const u64 fresh_white = white & ~(u->owned[WHITE] & ~u->walls);
    const u64 fresh_black = black & ~(u->owned[BLACK] & ~u->walls);

    /* A move can complete both roads at once. The mover wins that, and stm is already the
     * other player here, so the mover's road is the one that must be asked about first. */
    const int mover = !b->stm;
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

Outcome board_outcome(const Board *b) {
    const u64 white  = board_road_stones(b, WHITE);
    const u64 black  = board_road_stones(b, BLACK);
    const int mover  = !b->stm;
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

u64 board_road_placements(const Board *b, int color) {
    const u64 area  = board_road_stones(b, color);
    const u64 west  = FILE_A & b->mask;
    const u64 south = RANK_1 & b->mask;

    /* A stone completes a road when it touches, or is, both ends of one. */
    const u64 east_west = (neighbours(flood(west, area)) | west) &
                          (neighbours(flood(b->file_last, area)) | b->file_last);
    const u64 south_north = (neighbours(flood(south, area)) | south) &
                            (neighbours(flood(b->rank_last, area)) | b->rank_last);
    return (east_west | south_north) & board_empty(b);
}
