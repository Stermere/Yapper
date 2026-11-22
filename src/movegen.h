/* movegen.h - legal move generation. Every generated move is legal: Tak has no check. */
#ifndef MOVEGEN_H
#define MOVEGEN_H

#include "board.h"
#include "types.h"

/* [square][direction][hops]: the squares a spread lands on, none for no hops. */
extern u64 MoveLine[SQUARE_COUNT][4][MAX_SIZE + 1];

/* Must run once before generating. */
void movegen_init(void);

/* move_squares() from that table, for where a node asks, with no loop over the hops. */
static inline u64 movegen_squares(Move m) {
    if (m == MOVE_NONE) {
        return 0;
    }
    const int sq = move_square(m);
    return 1ULL << sq |
           MoveLine[sq][move_direction(m)][__builtin_popcount((unsigned)move_pattern(m))];
}

/* Writes every legal move and returns how many. `list` needs MAX_MOVES entries. The
 * position must not be over. */
int movegen_all(const Board *b, Move *list);

/* What movegen_all() would return, without writing the moves. */
int movegen_count(const Board *b);

/* Could `color` finish a road with a spread, were it their move? `ends` is board_road_ends()
 * for that side. Cheap enough for a leaf: most stacks are ruled out by their lines, and nothing
 * is played. */
bool movegen_road_spread(const Board *b, int color, const u64 ends[4]);

/* Tops held by `color` less those held by the other side, after the spread `m` against before
 * it, read off the stack's bits. */
int movegen_spread_swing(const Board *b, Move m, int color);

/* What a move has to touch to stop the roads `color` could finish by placing on `threats`: for
 * each of those squares, the square itself or a stone of a group it would join. A move that
 * misses one leaves that road to be placed, and with two such squares no placement can stop
 * both. */
#define ANSWER_NEEDS 8
typedef struct RoadAnswer {
    u64 threats;
    u64 needs[ANSWER_NEEDS];
    int count;
} RoadAnswer;

void movegen_road_answer(const Board *b, int color, u64 threats, RoadAnswer *a);

/* False for a move that leaves one of those roads to be placed. It knows nothing of the mover's
 * own wins, so the side to move must have no road of its own to finish, and stones and squares
 * enough that a placement cannot end the game on flats. */
bool movegen_may_answer(const RoadAnswer *a, Move m);

/* Is `m` legal here? For moves that did not come from the generator: a transposition
 * table entry can be stale or collided, and making one unchecked corrupts the board. */
bool movegen_is_legal(const Board *b, Move m);

#endif
