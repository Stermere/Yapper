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

/* Everything make changes apart from the stacks. It is one 64-byte block because that is
 * what unmake gets to restore with a single copy, whatever the move was, which is also
 * what recovers a flattened wall. Keep it free of padding: selftest memcmps the board. */
#define POSITION_STATE                                                                     \
    struct {                                                                               \
        u64 owned[2]; /* squares by the colour of their top stone */                       \
        u64 walls;    /* squares topped by a wall */                                       \
        u64 caps;     /* squares topped by a capstone */                                   \
        u64 tall;     /* squares of two stones or more */                                  \
        u64 hash;                                                                          \
        int reversible;  /* plies since a placement or a flattened wall */                 \
        int ply;         /* half moves played; the first two place the opponent's stone */ \
        u8 towers;       /* some stack has at least TOWER_BITS stones */                   \
        u8 flats[2];     /* stones in reserve */                                           \
        u8 capstones[2]; /* capstones in reserve */                                        \
        u8 padding[3];                                                                     \
    }

/* A word this high is a tower: with none on the board, no stack is within the eight stones
 * of a spread of running out of word, which is what lets a spread skip the question. */
#define TOWER_BITS 47

typedef POSITION_STATE PositionState;

/* Half of it, for a copy that is two moves on anything with 256-bit vectors. Unaligned on
 * purpose: Windows does not keep the stack 32-byte aligned. */
typedef u64 StateHalf __attribute__((vector_size(32), aligned(8), may_alias));

_Static_assert(sizeof(PositionState) == 64, "padding would break the memcmp in selftest");

typedef struct Board {
    /* One bit per stone, bit 0 the top of the stack, 1 for black, with a 1 above the bottom
     * stone so that the word alone says how tall the stack is: an empty square holds 1. A
     * stack is only ever pushed and popped at the top, so this is a shift register. */
    u64 stones[SQUARE_COUNT];

    /* On a line of its own, so the copy of it never straddles two. */
    _Alignas(64) union {
        POSITION_STATE;
        PositionState state;
    };

    u64 mask;       /* squares on the board */
    u64 file_last;  /* the east edge */
    u64 rank_last;  /* the north edge */
    u64 rank_links; /* ranks that a road between north and south must climb out of */
    u64 file_links; /* files that a road between west and east must step out of */

    int size;
    int halfkomi; /* in black's favour */

    /* A copy of the hash's key per square. A spread is short of registers, and this way the
     * table is an offset from the board rather than an address of its own to hold. */
    u64 stack_keys[SQUARE_COUNT];

    /* Stones 64 and up of a stack, with a 1 above them, while `stones` keeps its top bit set
     * in place of its own. 62 stones is all 6x6 has, so this is only ever touched by a
     * contrived 7x7 or 8x8 tower, and it sits last to stay off the hot cache lines. */
    u64 deep[SQUARE_COUNT];
} Board;

/* What board_unmake() cannot work out. */
typedef struct Undo {
    union {
        POSITION_STATE;
        PositionState state;
    };
    u64 lifted; /* a spread's source before the move; for a tower, the stones it lifted */
} Undo;

/* Must run once before anything else here. */
void board_init(void);

void board_clear(Board *b, int size, int halfkomi);

/* The stones and capstones each player starts with on a board of `size`. */
int board_start_flats(int size);
int board_start_capstones(int size);

void board_make(Board *b, Move m, Undo *u);
void board_unmake(Board *b, Move m, const Undo *u);

/* board_make() in two halves, for a loop over the moves of one position: save once, then
 * play and unmake each. Saving per move would make each sibling wait for the last, because
 * every copy reads what the copy before it wrote. An unmake puts back what was saved, so
 * the same Undo holds for all of them. */
void board_save(const Board *b, Undo *u);
void board_play(Board *b, Move m, Undo *u);

/* The same with the kind of move known, for a loop over a list that has them in runs. */
void board_play_place(Board *b, Move m);
void board_play_spread(Board *b, Move m, Undo *u);
void board_unmake_place(Board *b, Move m, const Undo *u);
void board_unmake_spread(Board *b, Move m, const Undo *u);

/* A lone flat or wall stepping one square has no hops to walk and no owners to work out,
 * and the generator lists them all together. These leave the Undo's `lifted` alone, so
 * only board_is_step() may send a move here. */
void board_play_step(Board *b, Move m);
void board_unmake_step(Board *b, Move m, const Undo *u);

/* A pass, for null move pruning. Never legal in the game itself. */
void board_make_null(Board *b, Undo *u);
void board_unmake_null(Board *b, const Undo *u);

/* The result after `u` was filled in by the make that reached this position, assuming the
 * position before it was not over. Roads are only searched for through squares the move
 * changed, which is what makes this cheap enough to run at every node. */
Outcome board_outcome_after(const Board *b, const Undo *u);

/* The same for a position reached by the placement `m`, which is simpler still: only that
 * one stone can have made a road, and only for the player who placed it. */
Outcome board_outcome_after_place(const Board *b, Move m);

/* The same answer from nothing but the position, for roots and for checking the above. */
Outcome board_outcome(const Board *b);

/* Hash computed from scratch. board_make() must always agree with it. */
u64 board_compute_hash(const Board *b);

/* The hash with the side to move in it. Repetition only ever compares positions with the
 * same side to move, so this is for the transposition table alone, and make does not have
 * to keep it up to date at every leaf. */
u64 board_key(const Board *b);

/* Empty squares where placing a flat or capstone completes a road for `color`. */
u64 board_road_placements(const Board *b, int color);

/* Is any square of `seeds` part of a road through `area`? */
bool board_road_through(const Board *b, u64 area, u64 seeds);

/* What it takes to finish a road for `color`: west, east, south, north, each the side of the
 * board itself and everything touching a group of `color` that runs to it. A road's first new
 * square is in one of a pair and its last in the other, however it was made. */
void board_road_ends(const Board *b, int color, u64 ends[4]);

/* The squares in both of a pair, where a single stone of `color` finishes a road. */
static inline u64 board_road_gaps(const u64 ends[4]) {
    return (ends[0] & ends[1]) | (ends[2] & ends[3]);
}

/* Rebuilds everything derived from the stacks after they are written directly. Parsing a
 * position is the only thing that does. */
void board_refresh(Board *b);

/* White moves on even plies, so the side to move is not stored, and neither make nor the
 * hash has to spend anything on turning it over. */
static inline int board_stm(const Board *b) { return b->ply & 1; }

/* A tower reads as 63 here, which is all that anything limited by the carry needs. */
static inline int board_short_height(const Board *b, int sq) {
    return 63 - __builtin_clzll(b->stones[sq]);
}
static inline int board_height(const Board *b, int sq) {
    const u64 deep = b->deep[sq];
    return board_short_height(b, sq) + (deep ? 63 - __builtin_clzll(deep) : 0);
}
/* The colour of a stone, counting down from the top of the stack. */
static inline int board_stone(const Board *b, int sq, int depth) {
    const u64 word = depth < 63 ? b->stones[sq] : b->deep[sq] >> (depth - 63);
    return (int)((word >> (depth < 63 ? depth : 0)) & 1);
}

static inline bool board_is_step(const Board *b, Move m) {
    const int src = move_square(m);
    return move_pattern(m) == 1 && b->stones[src] < 4 && !b->towers && !((b->caps >> src) & 1);
}

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
