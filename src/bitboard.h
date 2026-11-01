/* bitboard.h - 64-bit square sets in the shared 8x8 layout. */
#ifndef BITBOARD_H
#define BITBOARD_H

#include "types.h"

#define FILE_A 0x0101010101010101ULL
#define FILE_H 0x8080808080808080ULL
#define RANK_1 0x00000000000000FFULL

static inline int lsb(u64 x) { return __builtin_ctzll(x); }
static inline int popcount(u64 x) { return __builtin_popcountll(x); }

static inline int pop_lsb(u64 *x) {
    const int sq = lsb(*x);
    *x &= *x - 1;
    return sq;
}

/* Shifting past file H would wrap onto the next rank; off the board sides is the
 * caller's mask. Ranks fall off the ends of the word by themselves on 8x8 and are masked
 * elsewhere. */
static inline u64 shift_east(u64 x) { return (x & ~FILE_H) << 1; }
static inline u64 shift_west(u64 x) { return (x & ~FILE_A) >> 1; }

static inline u64 neighbours(u64 x) { return x << 8 | x >> 8 | shift_east(x) | shift_west(x); }

/* Everything connected to `seed` inside `area`. Expanding only the frontier means a small
 * group costs a couple of rounds however large the area is. */
static inline u64 flood(u64 seed, u64 area) {
    u64 group    = seed & area;
    u64 frontier = group;
    while (frontier) {
        frontier = neighbours(frontier) & area & ~group;
        group |= frontier;
    }
    return group;
}

#endif
