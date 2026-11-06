/* search.c - iterative deepening principal variation search. */
#include "search.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bitboard.h"
#include "eval.h"
#include "movegen.h"
#include "notation.h"
#include "thread.h"
#include "tt.h"

/* Enough for every ply to hold a worst-case move list, which never happens: this is
 * address space, and only the part a search reaches is ever touched. */
#define ARENA_MOVES (MAX_PLY * MAX_MOVES)

#define CHECK_EVERY 2048

/* The clock runs while a move travels back through a pipe, a runner or a network. */
#define MOVE_OVERHEAD_MS 20

/* How many plies of forced replies the leaf search will follow. */
#define MAX_QUIESCE 4

typedef struct Worker {
    Board board;
    u64 nodes;
    int seldepth;

    i64 start_ms;
    i64 soft_ms; /* do not begin another iteration past this */
    i64 hard_ms; /* abandon the search past this, 0 for no limit */
    u64 node_limit;

    Move root_best;
    int root_score;

    Move pv[MAX_PLY][MAX_PLY];
    int pv_length[MAX_PLY];

    Move killers[MAX_PLY][2];
    /* By side, then the whole move. Square plus kind put every spread from a square in one
     * direction in one slot, and a spread east in a flat placement's, whatever it carried. */
    i32 history[2][65536];
    /* What answered what, by side, then square plus kind of the move before this one and of
     * this one. */
    i32 replies[2][256][256];
    Move played[MAX_PLY + 1];
    /* The static evaluation of each node on the path. */
    int evals[MAX_PLY + 1];
    /* The move a node at this ply leaves out, while it asks whether its table move is the only
     * good one. MOVE_NONE otherwise. */
    Move excluded[MAX_PLY + 1];
    int root_depth;

    /* Positions on the way here, for repetition. */
    u64 keys[MAX_GAME_PLIES + MAX_PLY];
    int key_count;

    Move *arena;
    i32 *arena_scores;

    /* The network's accumulator for each node on the path, filled only when that node is
     * evaluated, and cleared by the make that reaches it. A table hit skips the evaluation, so
     * a frame comes from the nearest ancestor that has one, through the squares the moves since
     * then changed. The root's is built before the search starts. */
    NNFrame frames[MAX_PLY + 1];
} Worker;

static Worker Search;
static atomic_bool StopFlag;
static ThreadHandle SearchThread;
static bool SearchThreadRunning;
static int LmrTable[64][64];

static i64 now_ms(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (i64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void search_init(void) {
    for (int depth = 1; depth < 64; depth++) {
        for (int moves = 1; moves < 64; moves++) {
            LmrTable[depth][moves] = (int)(0.5 + log((double)depth) * log((double)moves) / 2.0);
        }
    }
    Search.arena        = malloc(ARENA_MOVES * sizeof(Move));
    Search.arena_scores = malloc(ARENA_MOVES * sizeof(i32));
    if (!Search.arena || !Search.arena_scores || !tt_resize(TT_DEFAULT_MB)) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
}

void search_clear(void) {
    tt_clear();
    memset(Search.killers, 0, sizeof(Search.killers));
    memset(Search.history, 0, sizeof(Search.history));
    memset(Search.replies, 0, sizeof(Search.replies));
}

static inline int score_to_tt(int score, int ply) {
    return score >= SCORE_WIN_BOUND ? score + ply : score <= -SCORE_WIN_BOUND ? score - ply : score;
}

static inline int score_from_tt(int score, int ply) {
    return score >= SCORE_WIN_BOUND ? score - ply : score <= -SCORE_WIN_BOUND ? score + ply : score;
}

/* The value for `us` of a game that ended at `ply`. */
static inline int outcome_score(Outcome outcome, int us, int ply) {
    if (outcome == DRAW) {
        return 0;
    }
    const int winner = outcome == WHITE_ROAD || outcome == WHITE_FLATS ? WHITE : BLACK;
    return winner == us ? SCORE_WIN - ply : -SCORE_WIN + ply;
}

/* Tak has no repetition rule, but match runners draw a threefold, and a line that
 * can be forced back to where it started is going nowhere under any rules. */
static inline bool is_repetition(const Worker *w) {
    const Board *b = &w->board;
    if (b->reversible < 4) {
        return false;
    }
    const int oldest = w->key_count - b->reversible;
    for (int i = w->key_count - 2; i >= 0 && i >= oldest; i -= 2) {
        if (w->keys[i] == b->hash) {
            return true;
        }
    }
    return false;
}

static inline bool out_of_time(Worker *w) {
    if (w->node_limit && w->nodes >= w->node_limit) {
        return true;
    }
    return w->hard_ms && now_ms() - w->start_ms >= w->hard_ms;
}

static inline bool should_stop(Worker *w) {
    if ((w->nodes & (CHECK_EVERY - 1)) == 0 && out_of_time(w)) {
        atomic_store_explicit(&StopFlag, true, memory_order_relaxed);
    }
    return atomic_load_explicit(&StopFlag, memory_order_relaxed);
}

static inline i32 *history_slot(Worker *w, int us, Move m) { return &w->history[us][m]; }

static inline void history_update(i32 *slot, int bonus) {
    *slot += bonus - *slot * abs(bonus) / 16384;
}

/* The evaluation of the node at `ply`. Make and unmake know nothing of the network: the moves on
 * the path are in `played`, and that is all a frame needs to catch up. */
static int node_eval(Worker *w, int ply, u64 threats) {
    const Board *b = &w->board;
    if (!nn_ready(b->size)) {
        return evaluate(b, threats);
    }
    NNFrame *frame = &w->frames[ply];
    if (!frame->computed) {
        u64 touched = 0;
        int base    = ply - 1;
        while (base >= 0) {
            touched |= nn_move_squares(w->played[base]);
            if (w->frames[base].computed) {
                break;
            }
            base--;
        }
        if (base >= 0) {
            nn_derive(b, frame, &w->frames[base], touched);
        } else {
            nn_refresh(b, frame);
        }
    }
    return evaluate_frame(b, threats, frame);
}

/* With the last stones going in, the flat count can end the game before the road comes, and
 * the reasoning in movegen_may_answer() no longer holds. */
static inline bool last_stones(const Board *b, int us) {
    return (b->flats[us] + b->capstones[us] <= 1) || (b->flats[!us] + b->capstones[!us] <= 1) ||
           (board_empty(b) & (board_empty(b) - 1)) == 0;
}

/* The evaluation trusts a quiet position, and a position with a road hanging is not one.
 * So a leaf only stands pat if there is nothing forcing; otherwise it plays out the
 * replies that stop the road. */
static int quiesce(Worker *w, int alpha, int beta, int ply, int qply, Move *moves) {
    Board *b     = &w->board;
    const int us = board_stm(b);

    w->nodes++;
    if (ply > w->seldepth) {
        w->seldepth = ply;
    }
    if (should_stop(w)) {
        return 0;
    }
    if (b->ply < 2) {
        return node_eval(w, ply, 0);
    }

    /* A win on the spot needs no search, and most of Tak's tactics hang off this test. */
    u64 ends[4];
    board_road_ends(b, us, ends);
    if (board_road_gaps(ends) & board_empty(b)) {
        return SCORE_WIN - ply - 1;
    }

    /* Looking for a spread is the dearest thing a node does, and a stand pat that already
     * fails high can only be confirmed by what it finds. */
    const u64 threats   = board_road_placements(b, !us);
    const int stand_pat = node_eval(w, ply, threats);
    if (!threats && stand_pat >= beta) {
        return stand_pat;
    }
    if (movegen_road_spread(b, us, ends)) {
        return SCORE_WIN - ply - 1;
    }

    if (!threats || last_stones(b, us) || qply >= MAX_QUIESCE || ply >= MAX_PLY - 1) {
        return stand_pat;
    }

    const int count = movegen_all(b, moves);
    int best        = -SCORE_WIN + ply + 2; /* the road lands unless a reply stops it */
    RoadAnswer answer;
    movegen_road_answer(b, !us, threats, &answer);
    Undo undo;
    board_save(b, &undo);
    for (int i = 0; i < count; i++) {
        const Move m = moves[i];
        if (!movegen_may_answer(&answer, m)) {
            continue;
        }
        board_play(b, m, &undo);
        w->played[ply]              = m;
        w->frames[ply + 1].computed = false;
        const Outcome outcome       = board_outcome_after(b, &undo);
        int score;
        if (outcome != ONGOING) {
            score = outcome_score(outcome, us, ply + 1);
        } else if (board_road_placements(b, board_stm(b))) {
            /* Still lost to a road. */
            score = -SCORE_WIN + ply + 2;
        } else {
            w->keys[w->key_count++] = undo.hash;
            score                   = -quiesce(w, -beta, -alpha, ply + 1, qply + 1, moves + count);
            w->key_count--;
        }
        board_unmake(b, m, &undo);
        if (atomic_load_explicit(&StopFlag, memory_order_relaxed)) {
            return 0;
        }
        if (score > best) {
            best = score;
            if (score > alpha) {
                alpha = score;
                if (score >= beta) {
                    break;
                }
            }
        }
    }
    return best;
}

/* Empty squares where a flat or capstone leaves a road to finish next move: touching a group
 * that runs to one edge and beside an empty square that touches one running to the other. A
 * square that does it through a group touching neither edge is missed. */
static inline u64 road_near(const u64 ends[4], u64 empty) {
    return ((ends[0] & neighbours(ends[1] & empty)) | (ends[1] & neighbours(ends[0] & empty)) |
            (ends[2] & neighbours(ends[3] & empty)) | (ends[3] & neighbours(ends[2] & empty))) &
           empty;
}

/* Tops held by `us` less those held by the other side, after the spread `m` against before it,
 * read off the stack's bits as spread_wins() does. */
static inline int spread_swing(const Board *b, Move m, int us) {
    const int sq    = move_square(m);
    const int step  = DirectionStep[move_direction(m)];
    const int carry = move_carry(m);
    const u64 word  = b->stones[sq];
    int swing       = -1;
    if (board_short_height(b, sq) > carry) {
        swing += ((word >> carry) & 1) == (u64)us ? 1 : -1;
    }
    int to = sq;
    for (int hops = move_pattern(m); hops; hops &= hops - 1) {
        to += step;
        const int before = (b->owned[us] >> to) & 1 ? 1 : (b->owned[!us] >> to) & 1 ? -1 : 0;
        const int after  = ((word >> (carry - 1 - __builtin_ctz(hops))) & 1) == (u64)us ? 1 : -1;
        swing += after - before;
    }
    return swing;
}

/* Besides the history, a static prior from the roads and the tops. A placement that leaves a road
 * to finish, or takes a square where the other side would leave one, is far more likely to cut
 * than the rest, and a spread is worth the tops it wins: E50. Each alone was too small to measure,
 * and together they passed. */
static void score_moves(Worker *w, const Move *moves, i32 *scores, int count, Move tt_move, int ply,
                        u64 threats, const u64 ends[4], const u64 their_ends[4]) {
    const Board *b        = &w->board;
    const int us          = board_stm(b);
    const Move killer_a   = w->killers[ply][0];
    const Move killer_b   = w->killers[ply][1];
    const u64 near_mine   = neighbours(board_road_stones(b, us));
    const i32 *replies    = w->replies[us][ply ? w->played[ply - 1] & 0xFF : 0];
    const u64 empty       = board_empty(b);
    const u64 near_ours   = b->ply >= 2 ? road_near(ends, empty) : 0;
    const u64 near_theirs = b->ply >= 2 ? road_near(their_ends, empty) : 0;

    for (int i = 0; i < count; i++) {
        const Move m = moves[i];
        i32 score    = w->history[us][m] + replies[m & 0xFF];
        if (m == tt_move) {
            score += 1 << 28;
        } else if (m == killer_a) {
            score += 1 << 24;
        } else if (m == killer_b) {
            score += 1 << 23;
        }
        if (move_is_place(m)) {
            const int sq         = move_square(m);
            const StoneType type = move_stone(m);
            if ((threats >> sq) & 1) {
                score += 1 << 26;
            }
            if (type == FLAT) {
                score += ((near_mine >> sq) & 1) ? 300 : 100;
            } else if (type == CAP) {
                score += 150;
            }
            if (type != WALL && ((near_ours >> sq) & 1)) {
                score += 8192;
            }
            if ((near_theirs >> sq) & 1) {
                score += 4096;
            }
        } else {
            score += 256 * spread_swing(b, m, us);
        }
        scores[i] = score;
    }
}

/* Two passes, because a compiler can vectorise finding the highest score and not a loop that
 * keeps its index. A tie must go to the earlier move: the generator lists placements first, and
 * an order among ties that loses that costs real nodes. */
static inline Move pick_next(Move *moves, i32 *scores, int from, int count) {
    i32 top = scores[from];
    for (int i = from + 1; i < count; i++) {
        top = scores[i] > top ? scores[i] : top;
    }
    int best = from;
    while (scores[best] != top) {
        best++;
    }
    const Move m = moves[best];
    const i32 s  = scores[best];
    moves[best]  = moves[from];
    scores[best] = scores[from];
    moves[from]  = m;
    scores[from] = s;
    return m;
}

/* `cut_node` is a null window expected to fail high: a late move there is very unlikely to be the
 * one that does, so it is worth less depth than at a node where every move must be refuted. */
static int search(Worker *w, int alpha, int beta, int depth, int ply, bool cut_node, Move *moves,
                  i32 *scores) {
    Board *b           = &w->board;
    const int us       = board_stm(b);
    const bool pv_node = beta - alpha > 1;
    const bool root    = ply == 0;

    if (depth <= 0) {
        return quiesce(w, alpha, beta, ply, 0, moves);
    }

    w->nodes++;
    w->pv_length[ply] = 0;
    if (ply > w->seldepth) {
        w->seldepth = ply;
    }
    if (should_stop(w)) {
        return 0;
    }

    if (!root) {
        if (is_repetition(w)) {
            return 0;
        }
        if (ply >= MAX_PLY - 1) {
            return node_eval(w, ply, b->ply >= 2 ? board_road_placements(b, !us) : 0);
        }
        alpha = alpha > -SCORE_WIN + ply ? alpha : -SCORE_WIN + ply;
        beta  = beta < SCORE_WIN - ply - 1 ? beta : SCORE_WIN - ply - 1;
        if (alpha >= beta) {
            return alpha;
        }
    }

    const Move excluded = w->excluded[ply];
    TTEntry entry       = {0};
    const u64 key       = board_key(b);
    const bool hit      = tt_probe(key, &entry);
    Move tt_move        = hit && !excluded ? entry.move : MOVE_NONE;
    if (hit && !pv_node && !excluded && entry.depth >= depth) {
        const int score   = score_from_tt(entry.score, ply);
        const Bound bound = tt_bound(&entry);
        if (bound == BOUND_EXACT || (bound == BOUND_LOWER && score >= beta) ||
            (bound == BOUND_UPPER && score <= alpha)) {
            return score;
        }
    }

    /* Both sides' ends, which the ordering reads as well. */
    u64 ends[4]       = {0};
    u64 their_ends[4] = {0};
    u64 threats       = 0;
    if (b->ply >= 2) {
        board_road_ends(b, us, ends);
        if (!root && (board_road_gaps(ends) & board_empty(b))) {
            return SCORE_WIN - ply - 1;
        }
        board_road_ends(b, !us, their_ends);
        threats = board_road_gaps(their_ends) & board_empty(b);
    }
    const int eval = hit ? entry.eval : node_eval(w, ply, threats);

    /* A side whose position has got better since its last move is more likely to find something
     * late in its list. */
    w->evals[ply]        = eval;
    const bool improving = ply >= 2 && eval > w->evals[ply - 2];

    if (!pv_node && !threats && !root && !excluded && depth <= 6 && eval - 90 * depth >= beta &&
        abs(beta) < SCORE_WIN_BOUND) {
        return eval;
    }

    if (!root && b->ply >= 2 && movegen_road_spread(b, us, ends)) {
        return SCORE_WIN - ply - 1;
    }

    if (!pv_node && !threats && !root && !excluded) {
        if (depth >= 3 && eval >= beta && b->ply >= 4 && abs(beta) < SCORE_WIN_BOUND) {
            const int reduction = 3 + depth / 4;
            Undo undo;
            board_make_null(b, &undo);
            w->played[ply]              = MOVE_NONE;
            w->frames[ply + 1].computed = false;
            w->keys[w->key_count++]     = b->hash;
            const int score = -search(w, -beta, -beta + 1, depth - 1 - reduction, ply + 1,
                                      !cut_node, moves, scores);
            w->key_count--;
            board_unmake_null(b, &undo);
            if (atomic_load_explicit(&StopFlag, memory_order_relaxed)) {
                return 0;
            }
            if (score >= beta && abs(score) < SCORE_WIN_BOUND) {
                return score;
            }
        }
    }

    if (tt_move != MOVE_NONE && !movegen_is_legal(b, tt_move)) {
        tt_move = MOVE_NONE;
    }

    /* A table move that beats every other by a margin is searched a ply deeper: a line that
     * hangs on one move is where a short horizon misjudges. This node's list does not exist
     * yet, so the search without that move can have its space. If even the others reach beta,
     * more than one move cuts and the node is done. */
    int extension = 0;
    if (!root && depth >= 6 && tt_move != MOVE_NONE && entry.depth >= depth - 3 &&
        tt_bound(&entry) != BOUND_UPPER && abs(entry.score) < SCORE_WIN_BOUND &&
        ply < 2 * w->root_depth) {
        const int singular_beta = score_from_tt(entry.score, ply) - 2 * depth;
        w->excluded[ply]        = tt_move;
        const int value         = search(w, singular_beta - 1, singular_beta, (depth - 1) / 2, ply,
                                         cut_node, moves, scores);
        w->excluded[ply]        = MOVE_NONE;
        if (atomic_load_explicit(&StopFlag, memory_order_relaxed)) {
            return 0;
        }
        if (value < singular_beta) {
            extension = 1;
        } else if (singular_beta >= beta) {
            return singular_beta;
        }
    }

    /* Most nodes cut on their first move, and that is nearly always the table's or a killer, so
     * these are tried before there is a list at all, which is far from free to make and sort. A
     * road to answer outranks a killer, so a node with one does not try them early. */
    Move first[3];
    int first_count = 0;
    if (tt_move != MOVE_NONE) {
        first[first_count++] = tt_move;
    }
    if (!threats) {
        for (int k = 0; k < 2; k++) {
            const Move killer = w->killers[ply][k];
            if (killer != MOVE_NONE && killer != tt_move && movegen_is_legal(b, killer)) {
                first[first_count++] = killer;
            }
        }
    }

    /* The other side's road lands unless a reply stops it. */
    const bool must_answer = threats && !root && !last_stones(b, us);
    RoadAnswer answer;
    if (must_answer) {
        movegen_road_answer(b, !us, threats, &answer);
    }
    const int original_alpha = alpha;
    int best_score           = must_answer ? -SCORE_WIN + ply + 2 : -SCORE_WIN;
    Move best_move           = MOVE_NONE;
    int searched             = 0;
    Undo undo;
    board_save(b, &undo);

    int count = 0;
    for (int i = 0;; i++) {
        if (i == first_count) {
            count = movegen_all(b, moves);
            score_moves(w, moves, scores, count, tt_move, ply, threats, ends, their_ends);
            for (int j = 0; j < first_count; j++) {
                pick_next(moves, scores, j, count);
            }
        }
        if (i >= first_count && i >= count) {
            break;
        }
        const Move m = i < first_count ? first[i] : pick_next(moves, scores, i, count);
        if ((must_answer && !movegen_may_answer(&answer, m)) || m == excluded) {
            continue;
        }

        /* Late move pruning: a shallow node has seen enough once this far down the list, and
         * sooner when its position is getting worse. */
        if (!root && !pv_node && !threats && depth <= 6 &&
            searched >= (improving ? 6 + depth * depth * 3 : 3 + depth * depth * 3 / 2) &&
            best_score > -SCORE_WIN_BOUND) {
            break;
        }

        board_play(b, m, &undo);
        if (depth > 1) {
            tt_prefetch(board_key(b));
        }
        const Outcome outcome = board_outcome_after(b, &undo);
        searched++;
        w->played[ply]              = m;
        w->frames[ply + 1].computed = false;
        w->pv_length[ply + 1]       = 0;

        int score;
        if (outcome != ONGOING) {
            score = outcome_score(outcome, us, ply + 1);
        } else {
            w->keys[w->key_count++] = undo.hash;
            int reduction           = 0;
            if (depth >= 3 && searched >= 4 && !threats) {
                reduction = LmrTable[depth < 63 ? depth : 63][searched < 63 ? searched : 63];
                reduction += cut_node - pv_node;
                reduction = reduction < 0 ? 0 : reduction > depth - 2 ? depth - 2 : reduction;
            }

            if (searched == 1) {
                score = -search(w, -beta, -alpha, depth - 1 + (m == tt_move) * extension, ply + 1,
                                !pv_node && !cut_node, moves + count, scores + count);
            } else {
                score = -search(w, -alpha - 1, -alpha, depth - 1 - reduction, ply + 1, true,
                                moves + count, scores + count);
                if (score > alpha && reduction) {
                    score = -search(w, -alpha - 1, -alpha, depth - 1, ply + 1, !cut_node,
                                    moves + count, scores + count);
                }
                if (score > alpha && pv_node) {
                    score = -search(w, -beta, -alpha, depth - 1, ply + 1, false, moves + count,
                                    scores + count);
                }
            }
            w->key_count--;
        }
        board_unmake(b, m, &undo);

        if (atomic_load_explicit(&StopFlag, memory_order_relaxed)) {
            return 0;
        }

        if (score > best_score) {
            best_score = score;
            if (score > alpha) {
                best_move = m;
                alpha     = score;
                if (pv_node) {
                    w->pv[ply][0] = m;
                    memcpy(&w->pv[ply][1], w->pv[ply + 1], sizeof(Move) * w->pv_length[ply + 1]);
                    w->pv_length[ply] = w->pv_length[ply + 1] + 1;
                }
                if (root) {
                    w->root_best  = m;
                    w->root_score = score;
                }
                if (score >= beta) {
                    if (w->killers[ply][0] != m) {
                        w->killers[ply][1] = w->killers[ply][0];
                        w->killers[ply][0] = m;
                    }
                    const int bonus = depth * depth * 16 < 4000 ? depth * depth * 16 : 4000;
                    i32 *replies    = w->replies[us][ply ? w->played[ply - 1] & 0xFF : 0];
                    history_update(history_slot(w, us, m), bonus);
                    history_update(&replies[m & 0xFF], bonus);
                    const Move *tried = i < first_count ? first : moves;
                    for (int j = 0; j < i; j++) {
                        if (tried[j] == excluded) {
                            continue;
                        }
                        history_update(history_slot(w, us, tried[j]), -bonus);
                        history_update(&replies[tried[j] & 0xFF], -bonus);
                    }
                    break;
                }
            }
        }
    }

    /* Without its best move, a node's score says nothing about the position. */
    if (excluded) {
        return best_score;
    }
    const Bound bound = best_score >= beta            ? BOUND_LOWER
                        : best_score > original_alpha ? BOUND_EXACT
                                                      : BOUND_UPPER;
    tt_store(key, best_move != MOVE_NONE ? best_move : tt_move, score_to_tt(best_score, ply), eval,
             depth, bound);
    return best_score;
}

static void print_info(Worker *w, int depth, int score) {
    const i64 elapsed = now_ms() - w->start_ms;
    printf("info depth %d seldepth %d ", depth, w->seldepth);
    if (abs(score) >= SCORE_WIN_BOUND) {
        /* In plies, which is how PTN Ninja reads it. racetrack only understands cp and
         * skips the line with a warning. */
        const int plies = SCORE_WIN - abs(score);
        printf("score mate %d ", score > 0 ? plies : -plies);
    } else {
        printf("score cp %d ", score);
    }
    printf("nodes %llu nps %llu hashfull %d time %lld pv", (unsigned long long)w->nodes,
           (unsigned long long)(w->nodes * 1000 / (u64)(elapsed > 0 ? elapsed : 1)), tt_hashfull(),
           (long long)elapsed);
    for (int i = 0; i < w->pv_length[0]; i++) {
        char text[MOVE_BUFFER];
        printf(" %s", move_format(w->pv[0][i], text));
    }
    printf("\n");
    fflush(stdout);
}

static void set_time_budget(Worker *w, const SearchLimits *limits) {
    w->soft_ms = 0;
    w->hard_ms = 0;
    if (limits->movetime) {
        w->hard_ms = limits->movetime > 10 ? limits->movetime - 5 : limits->movetime;
        w->soft_ms = w->hard_ms;
    } else if (limits->clock) {
        /* Never staking more than half of what is left lets a low clock settle at the
         * increment, where spending it all flags on the first slow reply. */
        const i64 left   = limits->time[board_stm(&w->board)] - MOVE_OVERHEAD_MS;
        const i64 usable = left > 0 ? left : 0;
        const i64 inc    = limits->inc[board_stm(&w->board)];

        /* A game lasts about as long as the shorter reserve, at three moves for every two
         * stones, so that says how far the clock has to go. */
        const Board *b  = &w->board;
        const int white = b->flats[WHITE] + b->capstones[WHITE];
        const int black = b->flats[BLACK] + b->capstones[BLACK];
        const int moves = 4 + (white < black ? white : black) * 3 / 2;
        w->soft_ms      = usable / (moves > 10 ? moves : 10) + inc * 3 / 4;
        w->hard_ms      = w->soft_ms * 3;
        if (w->hard_ms > usable / 2) {
            w->hard_ms = usable / 2;
        }
        if (w->hard_ms < 1) {
            w->hard_ms = 1;
        }
        if (w->soft_ms > w->hard_ms) {
            w->soft_ms = w->hard_ms;
        }
    }
}

static SearchReport run_search(const Board *root, const u64 *history, int history_count,
                               const SearchLimits *limits, bool verbose) {
    Worker *w     = &Search;
    w->board      = *root;
    w->nodes      = 0;
    w->seldepth   = 0;
    w->start_ms   = now_ms();
    w->node_limit = limits->nodes;
    w->root_best  = MOVE_NONE;
    w->root_score = 0;
    w->key_count  = history_count < MAX_GAME_PLIES ? history_count : MAX_GAME_PLIES;
    memcpy(w->keys, history, sizeof(u64) * (size_t)w->key_count);
    set_time_budget(w, limits);
    tt_new_search();

    /* The root never changes during a search, and every frame is carried from it at worst. A
     * root left to its own evaluation would go unbuilt whenever the table answers for it. */
    w->frames[0].computed = false;
    if (nn_ready(w->board.size)) {
        nn_refresh(&w->board, &w->frames[0]);
    }

    SearchReport report = {MOVE_NONE, 0, 0, 0};

    /* A move to fall back on, however early the stop comes. */
    if (board_outcome(root) == ONGOING) {
        Move fallback[MAX_MOVES];
        movegen_all(root, fallback);
        report.best = fallback[0];
    } else {
        return report;
    }

    const int depth_limit = limits->depth ? limits->depth : MAX_PLY - 1;
    int score             = 0;
    for (int depth = 1; depth <= depth_limit; depth++) {
        w->root_depth = depth;
        int window    = 40;
        int alpha     = depth >= 4 ? score - window : -SCORE_WIN;
        int beta      = depth >= 4 ? score + window : SCORE_WIN;
        int value;
        for (;;) {
            value = search(w, alpha, beta, depth, 0, false, w->arena, w->arena_scores);
            if (atomic_load_explicit(&StopFlag, memory_order_relaxed)) {
                break;
            }
            if (value <= alpha) {
                alpha = alpha - window > -SCORE_WIN ? alpha - window : -SCORE_WIN;
            } else if (value >= beta) {
                beta = beta + window < SCORE_WIN ? beta + window : SCORE_WIN;
            } else {
                break;
            }
            window *= 2;
            if (window > 500) {
                alpha = -SCORE_WIN;
                beta  = SCORE_WIN;
            }
        }

        /* A partial iteration still counts for the root moves it finished, because the
         * previous best is searched first and only a better one replaces it. */
        if (w->root_best != MOVE_NONE) {
            report.best  = w->root_best;
            report.score = w->root_score;
        }
        if (atomic_load_explicit(&StopFlag, memory_order_relaxed)) {
            break;
        }

        score        = value;
        report.depth = depth;
        report.score = value;
        if (verbose) {
            print_info(w, depth, value);
        }
        if (abs(value) >= SCORE_WIN_BOUND && !limits->infinite && SCORE_WIN - abs(value) <= depth) {
            break;
        }
        /* The next iteration takes several times this one, so starting it late only
         * spends the clock on a search that will not finish. */
        if (w->soft_ms && (now_ms() - w->start_ms) * 2 >= w->soft_ms) {
            break;
        }
        if (limits->soft_nodes && w->nodes >= limits->soft_nodes) {
            break;
        }
    }

    report.nodes = w->nodes;
    return report;
}

/* Arguments of the running search, owned here so the caller's may go away. */
static Board StartBoard;
static u64 StartHistory[MAX_GAME_PLIES];
static int StartHistoryCount;
static SearchLimits StartLimits;

static void search_thread_main(void *unused) {
    (void)unused;
    const SearchReport report =
        run_search(&StartBoard, StartHistory, StartHistoryCount, &StartLimits, true);

    /* `go infinite` owes its bestmove to the `stop` that ends it, never to itself. */
    while (StartLimits.infinite && !atomic_load_explicit(&StopFlag, memory_order_relaxed)) {
        thread_sleep_ms(1);
    }

    char text[MOVE_BUFFER];
    printf("bestmove %s\n", report.best ? move_format(report.best, text) : "(none)");
    fflush(stdout);
}

SearchReport search_run(const Board *root, const u64 *history, int history_count,
                        const SearchLimits *limits, bool verbose) {
    atomic_store_explicit(&StopFlag, false, memory_order_relaxed);
    return run_search(root, history, history_count, limits, verbose);
}

void search_wait(void) {
    if (SearchThreadRunning) {
        thread_join(SearchThread);
        SearchThreadRunning = false;
    }
}

void search_stop(void) { atomic_store_explicit(&StopFlag, true, memory_order_relaxed); }

void search_start(const Board *root, const u64 *history, int history_count,
                  const SearchLimits *limits) {
    search_wait();
    atomic_store_explicit(&StopFlag, false, memory_order_relaxed);
    StartBoard        = *root;
    StartHistoryCount = history_count < MAX_GAME_PLIES ? history_count : MAX_GAME_PLIES;
    memcpy(StartHistory, history, sizeof(u64) * (size_t)StartHistoryCount);
    StartLimits = *limits;
    if (!thread_create(&SearchThread, search_thread_main, NULL)) {
        fprintf(stderr, "cannot start the search thread\n");
        exit(1);
    }
    SearchThreadRunning = true;
}
