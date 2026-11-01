/* board.h - the position, make/unmake, and the game-end rules. */
#ifndef BOARD_H
#define BOARD_H

#include "types.h"

typedef enum {
    ONGOING,
    WHITE_ROAD,
    BLACK_ROAD,
    WHITE_FLATS,
    BLACK_FLATS,
    DRAW,
} Outcome;

typedef struct Board {
    /* One bit per stone, bit 0 the top of the stack, 1 for black. A stack is only ever
     * pushed and popped at the top, so this is a shift register. */
    u64 stones[SQUARE_COUNT];
    u8 height[SQUARE_COUNT];

    u64 owned[2]; /* squares by the colour of their top stone */
    u64 walls;    /* squares topped by a wall */
    u64 caps;     /* squares topped by a capstone */
    u64 hash;

    u64 mask;      /* squares on the board */
    u64 file_last; /* the east edge */
    u64 rank_last; /* the north edge */

    int size;
    int halfkomi; /* in black's favour */
    int ply;      /* half moves played; the first two place the opponent's stone */
    int stm;
    int reversible; /* plies since a placement or a flattened wall */

    u8 flats[2];     /* stones in reserve */
    u8 capstones[2]; /* capstones in reserve */

    /* Stones 65 and up of a stack. 62 stones is all 6x6 has, so this is only ever touched
     * by a contrived 7x7 or 8x8 tower, and it sits last to stay off the hot cache lines. */
    u64 deep[SQUARE_COUNT];
} Board;

/* What board_unmake() cannot work out. The bitboards are restored rather than undone,
 * which is also what recovers a flattened wall. */
typedef struct Undo {
    u64 owned[2];
    u64 walls;
    u64 caps;
    u64 hash;
    int reversible;
} Undo;

/* Must run once before anything else here. */
void board_init(void);

void board_clear(Board *b, int size, int halfkomi);

/* The stones and capstones each player starts with on a board of `size`. */
int board_start_flats(int size);
int board_start_capstones(int size);

void board_make(Board *b, Move m, Undo *u);
void board_unmake(Board *b, Move m, const Undo *u);

/* A pass, for null move pruning. Never legal in the game itself. */
void board_make_null(Board *b, Undo *u);
void board_unmake_null(Board *b, const Undo *u);

/* The result after `u` was filled in by the make that reached this position, assuming the
 * position before it was not over. Roads are only searched for through squares the move
 * changed, which is what makes this cheap enough to run at every node. */
Outcome board_outcome_after(const Board *b, const Undo *u);

/* The same answer from nothing but the position, for roots and for checking the above. */
Outcome board_outcome(const Board *b);

/* Hash computed from scratch. board_make() must always agree with it. */
u64 board_compute_hash(const Board *b);

/* Empty squares where placing a flat or capstone completes a road for `color`. */
u64 board_road_placements(const Board *b, int color);

/* Rebuilds everything derived from the stacks after they are written directly. Parsing a
 * position is the only thing that does. */
void board_refresh(Board *b);

static inline u64 board_empty(const Board *b) { return b->mask & ~(b->owned[0] | b->owned[1]); }
static inline u64 board_road_stones(const Board *b, int color) {
    return b->owned[color] & ~b->walls;
}
static inline u64 board_flats(const Board *b, int color) {
    return b->owned[color] & ~(b->walls | b->caps);
}
static inline StoneType board_top(const Board *b, int sq) {
    return (b->walls >> sq) & 1 ? WALL : (b->caps >> sq) & 1 ? CAP : FLAT;
}

/* For a position being written by hand, then finished with board_refresh(). The stones
 * are given bottom first. */
void board_set_stack(Board *b, int sq, const u8 *colors, int height, StoneType top);

#endif
