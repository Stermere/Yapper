/* types.h - fundamental types and the move encoding. */
#ifndef TYPES_H
#define TYPES_H

#include <stdbool.h>
#include <stdint.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
typedef int64_t i64;
typedef int32_t i32;
typedef int16_t i16;
typedef int8_t i8;

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define MIN_SIZE 3
#define MAX_SIZE 8

/* Every size shares the 8x8 layout: sq = rank * 8 + file. Shifts stay constants, so one
 * code path serves all boards and a size is only a mask and a carry limit. */
#define SQUARE_COUNT 64

/* 50 + 2 stones a side on 8x8. A stack of 8 at any square has at most 508 spreads, the
 * best moves-per-stone ratio there is, so 104 stones reach 6604, plus 192 placements. */
#define MAX_MOVES 8192

#define MAX_STONES 104

typedef enum { WHITE, BLACK } Color;

/* Stones below the top of a stack are always flats: nothing lands on a wall or capstone
 * without flattening it, so only the top stone has a type. */
typedef enum { FLAT, WALL, CAP } StoneType;

/* PTN: + north, > east, - south, < west. */
typedef enum { NORTH, EAST, SOUTH, WEST } Direction;

static const int DirectionStep[4]  = {8, 1, -8, -1};
static const char DirectionChar[4] = {'+', '>', '-', '<'};

/*
 * bits 0-5   square (placement target, or spread source)
 * bits 6-7   spread direction, or 1 + StoneType for a placement
 * bits 8-15  spread pattern, zero for a placement
 *
 * Pattern: stones count up from the bottom of the carried group, and bit i ends a hop
 * after stone i. The highest bit is therefore the carry count minus one, the popcount
 * is the number of hops, and there are exactly 255 patterns for a carry limit of 8.
 *
 * A placement's kind is at least 1, so zero is never a legal move.
 */
typedef u16 Move;

#define MOVE_NONE ((Move)0)

static inline Move move_place(int sq, StoneType type) { return (Move)(sq | (type + 1) << 6); }
static inline Move move_spread(int sq, Direction dir, int pattern) {
    return (Move)(sq | dir << 6 | pattern << 8);
}
static inline int move_square(Move m) { return m & 63; }
static inline int move_kind(Move m) { return (m >> 6) & 3; }
static inline int move_pattern(Move m) { return m >> 8; }
static inline bool move_is_place(Move m) { return move_pattern(m) == 0; }
static inline StoneType move_stone(Move m) { return (StoneType)(move_kind(m) - 1); }
static inline Direction move_direction(Move m) { return (Direction)move_kind(m); }
static inline int move_carry(Move m) { return 32 - __builtin_clz(move_pattern(m)); }

/* The squares a move changes: a spread's source and every square it drops on. A pass changes
 * none. */
static inline u64 move_squares(Move m) {
    if (m == MOVE_NONE) {
        return 0;
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
    return bits;
}

#endif
