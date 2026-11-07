/* eval.c - the evaluation: a network scores the stacks, and the little a fit cannot read is
 * added around it. */
#include "eval.h"

#include "bitboard.h"
#include "tune.h"

/* A game ends when a side runs out of stones or the board fills, whichever is first. */
static inline int stones_left(const Board *b) {
    const int us         = board_stm(b);
    const int mine_left  = b->flats[us] + b->capstones[us];
    const int their_left = b->flats[!us] + b->capstones[!us];
    const int left       = mine_left < their_left ? mine_left : their_left;
    const int spaces     = popcount(board_empty(b));
    return spaces < left ? spaces : left;
}

/* The output bucket for this many stones left, finer near the end, where what a flat is worth
 * climbs too steeply for a few buckets to follow. */
static inline int output_bucket(int left) {
    static const u8 Buckets[16] = {0, 0, 1, 2, 3, 4, 4, 5, 5, 5, 6, 6, 6, 6, 6, 7};
    return Buckets[left < 15 ? left : 15];
}

void eval_net_inputs(const Board *b, NNInputs *in) {
    const int us   = board_stm(b);
    const int komi = us == WHITE ? -b->halfkomi : b->halfkomi;
    in->stm        = us;
    in->bucket     = output_bucket(stones_left(b));
    in->lead       = (popcount(board_flats(b, us)) - popcount(board_flats(b, !us))) * 2 + komi;
    in->reserve    = b->flats[us] + b->capstones[us] - b->flats[!us] - b->capstones[!us];
    in->komi       = komi;
}

/* Far inside the win bound, which a stand pat must never reach. */
#define NET_SCORE_LIMIT 12000

int evaluate_frame(const Board *b, u64 threats, const NNFrame *frame) {
    NNInputs in;
    eval_net_inputs(b, &in);
    i64 score = nn_score(b->size, frame, &in) >> 6;
    /* No position a fit reads has a road hanging, so what one is worth is set by hand. The side to
     * move has already had its chance to win on the spot, so a threat by the other side is one
     * this side still has to answer. */
    if (threats) {
        score -= (threats & (threats - 1)) ? ThreatMany : ThreatOne;
    }
    return (int)(score > NET_SCORE_LIMIT    ? NET_SCORE_LIMIT
                 : score < -NET_SCORE_LIMIT ? -NET_SCORE_LIMIT
                                            : score);
}

int evaluate(const Board *b, u64 threats) {
    if (nn_ready(b->size)) {
        NNFrame frame;
        nn_refresh(b, &frame);
        return evaluate_frame(b, threats, &frame);
    }
    /* Only a build with no network for this size gets here, as after a change to the network's
     * shape: counting flats is enough to play, and to build the trainer that fits a new one. */
    NNInputs in;
    eval_net_inputs(b, &in);
    return in.lead * 50;
}
