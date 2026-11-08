/* policy.h - a prior over the moves of a position, from what each move does in Tak's own terms:
 * what a placement touches and leaves for the roads, what a spread takes and gives up, and how
 * either answers the last two moves. A small network reads each move's features, fitted on the
 * games' own moves by `yapper policyfit`. */
#ifndef POLICY_H
#define POLICY_H

#include "board.h"
#include "types.h"

/* How many placements from each edge a side's roads are followed. */
#define POLICY_LEVELS 3

/* What a node works out once for all of its moves. Side 0 is the side to move. */
typedef struct PolicyContext {
    u64 near_road[2];    /* board_road_near() */
    u64 threats;         /* where the other side can place a road */
    u64 ends[2];         /* each side's four ends together */
    u64 rel_empty[2][3]; /* empty squares on a road within 0, 1 and 2 placements of the best */
    u64 rel_own[2][3];   /* the same for the side's own road stones */
    u64 prev[3];         /* the squares the last move touched, those beside them, two away */
    u64 prev2[3];        /* the same for this side's own last move */
    int dist[2];         /* fewest placements to a road, up to 2 * POLICY_LEVELS */
    int us;
    int phase; /* by the smaller reserve, 0 at the start to 3 at the end */
    int race; /* with the end in sight, 0 behind on flats, 1 level, 2 ahead, komi counted; else 3 */
    int prev_kind; /* the last move: flat, wall, capstone or spread, or 4 for none */
} PolicyContext;

/* `ends` and `their_ends` are board_road_ends() for the side to move and the other side; `prev`
 * is the move that reached the position and `prev2` the one before it, either MOVE_NONE. */
void policy_context(const Board *b, const u64 ends[4], const u64 their_ends[4], Move prev,
                    Move prev2, PolicyContext *ctx);

#define POLICY_MAX_ACTIVE 40

/* The features of `m`, as indices below policy_feature_count(). Returns how many. */
int policy_features(const Board *b, const PolicyContext *ctx, Move m, u16 *out);

int policy_feature_count(void);

/* The network: a row of 16 lanes for each feature, 15 clipped units and a linear lane. */
#define POLICY_LANES  16
#define POLICY_HIDDEN 15
#define POLICY_QA     128 /* an activation of 1, and a nat in the linear lane */
#define POLICY_QB     256 /* an output weight of 1 */
#define POLICY_LIMIT                                                            \
    512 /* the largest a row's lane may be, which keeps a move's sum in 16 bits \
         */

/* The prior of `m` in 256ths of a nat. */
int policy_logit(const Board *b, const PolicyContext *ctx, Move m);

/* Every move's prior at once, the same as policy_logit() for each: what moves share is summed
 * once. */
void policy_logits(const Board *b, const PolicyContext *ctx, const Move *moves, int count,
                   i32 *out);

/* Weights in place of the compiled ones, or NULL for those. A table of the wrong shape is no
 * table at all, and every move's prior is 0. */
void policy_install(const i16 *rows, const i16 *bias, const i16 *out);

#endif
