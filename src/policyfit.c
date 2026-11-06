/* policyfit.c - fits the move prior to the moves the search chose in self-play games.
 *
 * The prior is a softmax over a position's legal moves, its logits the weights of each move's
 * features summed, aimed at the move the search played. Features come from the engine's own
 * policy_features(), and the held-out games are scored at the end with the engine's own
 * policy_logit() on the rounded weights, so what is fitted is what plays. */
#include "policyfit.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bitboard.h"
#include "datagen.h"
#include "movegen.h"
#include "policy.h"

#define HELD_OUT 16

typedef struct Score {
    u64 positions;
    double loss, uniform;
    u64 top1, top3, top8;
    double rank;
} Score;

typedef struct Fit {
    int keep;
    bool train;     /* a pass that fits, or one that only scores the held-out games */
    bool engine;    /* score with policy_logit() and the hand-set prior beside it */
    float *weights; /* in nats */
    float *squares; /* Adagrad's running sum of squared steps */
    double *gradient;
    double rate;
    Score fitted, prior;
} Fit;

static Move Moves[MAX_MOVES];
static u16 Features[MAX_MOVES][POLICY_MAX_ACTIVE];
static u8 Active[MAX_MOVES];
static double Logits[MAX_MOVES];

/* What the search ordered by before the prior, without its history: the road squares, the near
 * squares and the tops a spread wins. */
static i32 hand_prior(const Board *b, const PolicyContext *ctx, Move m) {
    if (!move_is_place(m)) {
        return 256 * movegen_spread_swing(b, m, ctx->us);
    }
    const int sq         = move_square(m);
    const StoneType type = move_stone(m);
    const u64 near_mine  = neighbours(board_road_stones(b, ctx->us));
    i32 score            = 0;
    if ((ctx->threats >> sq) & 1) {
        score += 1 << 26;
    }
    if (type == FLAT) {
        score += ((near_mine >> sq) & 1) ? 300 : 100;
    } else if (type == CAP) {
        score += 150;
    }
    if (type != WALL && ((ctx->near_ours >> sq) & 1)) {
        score += 8192;
    }
    if ((ctx->near_theirs >> sq) & 1) {
        score += 4096;
    }
    return score;
}

/* The target's rank by `values`, ties going to the earlier move as the search's ordering does. */
static int rank_of(const double *values, int count, int target) {
    int rank = 1;
    for (int j = 0; j < count; j++) {
        rank += values[j] > values[target] || (values[j] == values[target] && j < target);
    }
    return rank;
}

static void tally(Score *s, int rank, double loss, int count) {
    s->positions++;
    s->loss += loss;
    s->uniform += log((double)count);
    s->top1 += rank == 1;
    s->top3 += rank <= 3;
    s->top8 += rank <= 8;
    s->rank += rank;
}

static bool visit(const Board *b, const DataGame *game, int ply, u64 index, void *context) {
    Fit *fit = context;
    if (game->plies[ply].score == DATA_NO_SCORE || ply < game->header.random_plies || b->ply < 2) {
        return true;
    }
    const bool held = index % HELD_OUT == HELD_OUT - 1;
    if (fit->train ? held || index % (u64)fit->keep != 0 : !held) {
        return true;
    }

    const int count = movegen_all(b, Moves);
    int target      = -1;
    for (int j = 0; j < count; j++) {
        target = Moves[j] == game->plies[ply].move ? j : target;
    }
    if (target < 0 || count < 2) {
        return true;
    }

    u64 ends[4], their_ends[4];
    board_road_ends(b, board_stm(b), ends);
    board_road_ends(b, !board_stm(b), their_ends);
    PolicyContext ctx;
    policy_context(b, ends, their_ends, &ctx);

    double top = -1e30;
    for (int j = 0; j < count; j++) {
        Active[j] = (u8)policy_features(b, &ctx, Moves[j], Features[j]);
        if (fit->engine) {
            Logits[j] = policy_logit(b, &ctx, Moves[j]) / 256.0;
        } else {
            double z = 0.0;
            for (int k = 0; k < Active[j]; k++) {
                z += fit->weights[Features[j][k]];
            }
            Logits[j] = z;
        }
        top = Logits[j] > top ? Logits[j] : top;
    }
    double total = 0.0;
    for (int j = 0; j < count; j++) {
        total += exp(Logits[j] - top);
    }
    const double loss = log(total) - (Logits[target] - top);

    if (!fit->train) {
        tally(&fit->fitted, rank_of(Logits, count, target), loss, count);
        if (fit->engine) {
            for (int j = 0; j < count; j++) {
                Logits[j] = hand_prior(b, &ctx, Moves[j]);
            }
            tally(&fit->prior, rank_of(Logits, count, target), 0.0, count);
        }
        return true;
    }

    const int features = policy_feature_count();
    memset(fit->gradient, 0, sizeof(double) * (size_t)features);
    for (int j = 0; j < count; j++) {
        const double step = exp(Logits[j] - top) / total - (j == target);
        for (int k = 0; k < Active[j]; k++) {
            fit->gradient[Features[j][k]] += step;
        }
    }
    for (int f = 0; f < features; f++) {
        const double g = fit->gradient[f] + 1e-4 * fit->weights[f];
        if (g != 0.0) {
            fit->squares[f] += (float)(g * g);
            fit->weights[f] -= (float)(fit->rate * g / (sqrt(fit->squares[f]) + 1e-8));
        }
    }
    fit->fitted.positions++;
    fit->fitted.loss += loss;
    return true;
}

static bool pass(Fit *fit, char **paths, int path_count) {
    for (int i = 0; i < path_count; i++) {
        if (!data_replay(paths[i], visit, fit)) {
            fprintf(stderr, "cannot read %s\n", paths[i]);
            return false;
        }
    }
    return true;
}

static void report(const char *name, const Score *s, bool losses) {
    const double n = s->positions ? (double)s->positions : 1.0;
    printf("  %-18s %9llu positions  top1 %5.1f%%  top3 %5.1f%%  top8 %5.1f%%  mean rank %6.2f",
           name, (unsigned long long)s->positions, 100.0 * s->top1 / n, 100.0 * s->top3 / n,
           100.0 * s->top8 / n, s->rank / n);
    if (losses) {
        printf("  loss %.4f (uniform %.4f)", s->loss / n, s->uniform / n);
    }
    printf("\n");
}

int policyfit_run(const char *out, int keep, int epochs, char **paths, int path_count) {
    const int features = policy_feature_count();
    Fit fit            = {.keep = keep > 0 ? keep : 1, .rate = 0.05};
    fit.weights        = calloc((size_t)features, sizeof(float));
    fit.squares        = calloc((size_t)features, sizeof(float));
    fit.gradient       = calloc((size_t)features, sizeof(double));
    i16 *rounded       = calloc((size_t)features, sizeof(i16));
    if (!fit.weights || !fit.squares || !fit.gradient || !rounded) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    for (int epoch = 1; epoch <= epochs; epoch++) {
        memset(&fit.fitted, 0, sizeof(fit.fitted));
        fit.train = true;
        if (!pass(&fit, paths, path_count)) {
            return 1;
        }
        const u64 fitted        = fit.fitted.positions;
        const double train_loss = fit.fitted.loss / (fitted ? (double)fitted : 1.0);
        memset(&fit.fitted, 0, sizeof(fit.fitted));
        fit.train = false;
        if (!pass(&fit, paths, path_count)) {
            return 1;
        }
        printf("epoch %d: %llu positions fitted at loss %.4f\n", epoch, (unsigned long long)fitted,
               train_loss);
        report("held out", &fit.fitted, true);
        fflush(stdout);
        fit.rate *= 0.5;
    }

    for (int f = 0; f < features; f++) {
        const double q = floor(fit.weights[f] * 256.0 + 0.5);
        rounded[f]     = (i16)(q > 32767 ? 32767 : q < -32767 ? -32767 : q);
    }
    policy_install(rounded);
    memset(&fit.fitted, 0, sizeof(fit.fitted));
    memset(&fit.prior, 0, sizeof(fit.prior));
    fit.train  = false;
    fit.engine = true;
    if (!pass(&fit, paths, path_count)) {
        return 1;
    }
    policy_install(NULL);
    printf("held out, as the engine plays it:\n");
    report("fitted prior", &fit.fitted, true);
    report("hand-set prior", &fit.prior, false);

    FILE *file = fopen(out, "wb");
    if (!file) {
        fprintf(stderr, "cannot write %s\n", out);
        return 1;
    }
    fprintf(file,
            "/* policy_net.h - written by `yapper policyfit`. A weight per feature of policy.c, "
            "in\n * 256ths of a nat. */\n#ifndef POLICY_NET_H\n#define POLICY_NET_H\n\n"
            "#define POLICY_NET_FEATURES %d\n\nstatic const i16 PolicyNet[%d] = {",
            features, features);
    for (int f = 0; f < features; f++) {
        fprintf(file, "%s%d,", f % 12 ? " " : "\n    ", rounded[f]);
    }
    fprintf(file, "\n};\n\n#endif\n");
    fclose(file);
    free(fit.weights);
    free(fit.squares);
    free(fit.gradient);
    free(rounded);
    return 0;
}
