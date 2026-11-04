/* eval.h - the static evaluation: the network, and what goes around it. */
#ifndef EVAL_H
#define EVAL_H

#include "board.h"
#include "nn.h"

/* In hundredths of a flat, from the side to move's point of view. Must stay well inside
 * SCORE_WIN_BOUND. `threats` is board_road_placements() for the side not to move: every
 * caller needs it for its own reasons, and it is the most expensive input there is. */
int evaluate(const Board *b, u64 threats);

/* The network's score with its frame already up to date, for a search that carries frames
 * from node to node. Only for a size nn_ready() has. */
int evaluate_frame(const Board *b, u64 threats, const NNFrame *frame);

/* What the network is scored on besides the stacks, read the same way in play and in a fit. */
void eval_net_inputs(const Board *b, NNInputs *in);

#endif
