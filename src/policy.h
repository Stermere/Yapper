/* policy.h - a prior over the moves of a position, from what each move does in Tak's own terms:
 * what a placement touches and leaves for the roads, what a spread takes and gives up. It is a
 * weight per feature, fitted on the games' own moves by `yapper policyfit`. */
#ifndef POLICY_H
#define POLICY_H

#include "board.h"
#include "types.h"

/* What a node works out once for all of its moves. */
typedef struct PolicyContext {
    u64 near_ours;   /* board_road_near() for the side to move */
    u64 near_theirs; /* and for the other side */
    u64 threats;     /* where the other side can place a road */
    u64 ends_ours;   /* each side's four ends together */
    u64 ends_theirs;
    int us;
    int phase; /* by the smaller reserve, 0 at the start to 3 at the end */
    int race; /* with the end in sight, 0 behind on flats, 1 level, 2 ahead, komi counted; else 3 */
} PolicyContext;

/* `ends` and `their_ends` are board_road_ends() for the side to move and the other side. */
void policy_context(const Board *b, const u64 ends[4], const u64 their_ends[4], PolicyContext *ctx);

#define POLICY_MAX_ACTIVE 16

/* The features of `m`, as indices below policy_feature_count(). Returns how many. */
int policy_features(const Board *b, const PolicyContext *ctx, Move m, u16 *out);

int policy_feature_count(void);

/* The prior of `m` in 256ths of a nat: its features' weights summed. */
int policy_logit(const Board *b, const PolicyContext *ctx, Move m);

/* Weights in place of the compiled ones, in 256ths of a nat, or NULL for the compiled ones. */
void policy_install(const i16 *weights);

#endif
