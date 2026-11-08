/* policyfit.c - fits the move prior to the moves the search chose in self-play games.
 *
 * The prior is a softmax over a position's legal moves, aimed at the move the search played. Each
 * move's features come from the engine's own policy_features(), are read once into memory by
 * every thread, and are fitted in minibatches whose gradients the threads sum, by Adam. The
 * held-out games are scored at the end with the engine's own policy_logits() on the rounded
 * weights, so what is fitted is what plays. Files go to threads by their place in the list, so a
 * fit repeats for the same seed and thread count. */
#include "policyfit.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bitboard.h"
#include "datagen.h"
#include "movegen.h"
#include "policy.h"
#include "thread.h"

#define FIT_THREADS 64
#define HELD_OUT    16   /* one game in this many is held out */
#define HELD_SCORED 4    /* and one in this many of those is scored */
#define BATCH       1024 /* positions a step */
#define FIT_LIMIT   ((float)POLICY_LIMIT / POLICY_QA)

/* A position: its moves' feature counts and features. */
typedef struct FitPos {
    u64 feat, move;
    u16 count, target;
    u8 spread;
} FitPos;

typedef struct FitSet {
    FitPos *pos;
    u64 npos, cpos;
    u8 *counts;
    u64 nmoves, cmoves;
    u16 *feats;
    u64 nfeats, cfeats;
} FitSet;

typedef struct FitScore {
    double loss, rank;
    u64 n, top1, top3, top8;
} FitScore;

typedef struct FitWorker {
    int id;
    FitSet train, held;
    Move moves[MAX_MOVES];
    float *pre;  /* [MAX_MOVES][POLICY_HIDDEN] before clipping */
    float *grad; /* this thread's share of the step's gradient */
    double logits[MAX_MOVES];
    double loss;
    u64 positions;
    FitScore score;
} FitWorker;

static int FitThreads, FitKeep, FitFeatures;
static char **FitPaths;
static int FitPathCount;
static FitWorker *FitWorkers;

/* wlin[F], emb[F][H], b1[H], w2[H], in nats and activations of 1. */
#define H POLICY_HIDDEN
static u64 FitParams;
static float *FitParam, *FitAdamM, *FitAdamV;
#define WLIN(p) (p)
#define EMB(p)  ((p) + FitFeatures)
#define B1(p)   ((p) + FitFeatures + (u64)FitFeatures * H)
#define W2(p)   ((p) + FitFeatures + (u64)FitFeatures * H + H)

static void *fit_grow(void *p, u64 *cap, u64 need, size_t item) {
    if (need <= *cap) {
        return p;
    }
    u64 c = *cap ? *cap : 4096;
    while (c < need) {
        c *= 2;
    }
    void *q = realloc(p, c * item);
    if (!q) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    *cap = c;
    return q;
}

static inline Move fit_prev_move(const DataGame *game, int ply, int back) {
    return ply >= back ? game->plies[ply - back].move : MOVE_NONE;
}

static bool fit_read_position(const Board *b, const DataGame *game, int ply, u64 index,
                              void *context) {
    FitWorker *w = context;
    if (game->plies[ply].score == DATA_NO_SCORE || ply < game->header.random_plies || b->ply < 2) {
        return true;
    }
    FitSet *set;
    if (index % HELD_OUT == HELD_OUT - 1) {
        if ((index / HELD_OUT) % HELD_SCORED != 0) {
            return true;
        }
        set = &w->held;
    } else {
        if (index % (u64)FitKeep != 0) {
            return true;
        }
        set = &w->train;
    }
    const int count = movegen_all(b, w->moves);
    int target      = -1;
    for (int j = 0; j < count; j++) {
        target = w->moves[j] == game->plies[ply].move ? j : target;
    }
    if (target < 0 || count < 2) {
        return true;
    }
    u64 ends[4], their_ends[4];
    board_road_ends(b, board_stm(b), ends);
    board_road_ends(b, !board_stm(b), their_ends);
    PolicyContext ctx;
    policy_context(b, ends, their_ends, fit_prev_move(game, ply, 1), fit_prev_move(game, ply, 2),
                   &ctx);

    set->pos    = fit_grow(set->pos, &set->cpos, set->npos + 1, sizeof(FitPos));
    set->counts = fit_grow(set->counts, &set->cmoves, set->nmoves + (u64)count, 1);
    set->feats =
        fit_grow(set->feats, &set->cfeats, set->nfeats + (u64)count * POLICY_MAX_ACTIVE, 2);
    FitPos *p = &set->pos[set->npos++];
    p->feat   = set->nfeats;
    p->move   = set->nmoves;
    p->count  = (u16)count;
    p->target = (u16)target;
    p->spread = !move_is_place(w->moves[target]);
    for (int j = 0; j < count; j++) {
        const int n = policy_features(b, &ctx, w->moves[j], set->feats + set->nfeats);
        set->counts[set->nmoves++] = (u8)n;
        set->nfeats += (u64)n;
    }
    return true;
}

static atomic_bool FitReadFailed;

static void fit_reader_main(void *arg) {
    FitWorker *w = arg;
    for (int i = w->id; i < FitPathCount; i += FitThreads) {
        if (!data_replay(FitPaths[i], fit_read_position, w)) {
            fprintf(stderr, "cannot read %s\n", FitPaths[i]);
            atomic_store(&FitReadFailed, true);
        }
    }
}

static inline float fit_clip(float x) { return x < 0.f ? 0.f : x > 1.f ? 1.f : x; }

/* One position's loss, its gradient added to `grad` unless that is NULL. */
static double fit_position(FitWorker *w, const FitSet *set, const FitPos *p, float *grad,
                           FitScore *score) {
    const u16 *feats  = set->feats + p->feat;
    const u8 *counts  = set->counts + p->move;
    const float *wlin = WLIN(FitParam), *emb = EMB(FitParam), *w2 = W2(FitParam);
    double top   = -1e30;
    const u16 *f = feats;
    for (int j = 0; j < p->count; j++) {
        float *a = w->pre + (u64)j * H;
        memcpy(a, B1(FitParam), H * sizeof(float));
        double z = 0.0;
        for (int k = 0; k < counts[j]; k++) {
            z += wlin[f[k]];
            const float *row = emb + (u64)f[k] * H;
            for (int h = 0; h < H; h++) {
                a[h] += row[h];
            }
        }
        for (int h = 0; h < H; h++) {
            z += w2[h] * fit_clip(a[h]);
        }
        w->logits[j] = z;
        top          = z > top ? z : top;
        f += counts[j];
    }
    double total = 0.0;
    for (int j = 0; j < p->count; j++) {
        total += exp(w->logits[j] - top);
    }
    const double loss = log(total) - (w->logits[p->target] - top);
    if (score) {
        int rank = 1;
        for (int j = 0; j < p->count; j++) {
            rank += w->logits[j] > w->logits[p->target] ||
                    (w->logits[j] == w->logits[p->target] && j < p->target);
        }
        score->n++;
        score->loss += loss;
        score->rank += rank;
        score->top1 += rank == 1;
        score->top3 += rank <= 3;
        score->top8 += rank <= 8;
    }
    if (!grad) {
        return loss;
    }
    float *gw = WLIN(grad), *ge = EMB(grad), *gb = B1(grad), *g2 = W2(grad);
    float dbias[H] = {0};
    f              = feats;
    for (int j = 0; j < p->count; j++) {
        const float step = (float)(exp(w->logits[j] - top) / total - (j == p->target));
        const float *a   = w->pre + (u64)j * H;
        float da[H];
        for (int h = 0; h < H; h++) {
            g2[h] += step * fit_clip(a[h]);
            da[h] = a[h] > 0.f && a[h] < 1.f ? step * w2[h] : 0.f;
            dbias[h] += da[h];
        }
        for (int k = 0; k < counts[j]; k++) {
            gw[f[k]] += step;
            float *row = ge + (u64)f[k] * H;
            for (int h = 0; h < H; h++) {
                row[h] += da[h];
            }
        }
        f += counts[j];
    }
    for (int h = 0; h < H; h++) {
        gb[h] += dbias[h];
    }
    return loss;
}

typedef struct FitRef {
    u32 thread, pos;
} FitRef;

static FitRef *FitOrder;
static u64 FitOrderCount, FitStepsDone, FitStepsAll;
static double FitRate;
static atomic_int FitBarrierCount, FitBarrierGen;
static bool FitScoring;

static void fit_barrier(void) {
    const int gen = atomic_load(&FitBarrierGen);
    if (atomic_fetch_add(&FitBarrierCount, 1) == FitThreads - 1) {
        atomic_store(&FitBarrierCount, 0);
        atomic_fetch_add(&FitBarrierGen, 1);
    } else {
        while (atomic_load(&FitBarrierGen) == gen) {
        }
    }
}

/* Each step every thread fits its share of the batch, then sums and applies its own slice of the
 * parameters, so no two threads ever write the same weight. */
static void fit_epoch(FitWorker *w) {
    const u64 steps = FitOrderCount / BATCH;
    for (u64 s = 0; s < steps; s++) {
        const u64 from = s * BATCH;
        for (u64 i = from + (u64)BATCH * (u64)w->id / (u64)FitThreads;
             i < from + (u64)BATCH * (u64)(w->id + 1) / (u64)FitThreads; i++) {
            const FitSet *set = &FitWorkers[FitOrder[i].thread].train;
            w->loss += fit_position(w, set, &set->pos[FitOrder[i].pos], w->grad, NULL);
            w->positions++;
        }
        fit_barrier();
        const u64 t     = FitStepsDone + s;
        const double lr = FitRate * (1.0 - 0.95 * (double)t / (double)FitStepsAll);
        const double c1 = 1.0 - pow(0.9, (double)(t + 1)), c2 = 1.0 - pow(0.999, (double)(t + 1));
        const u64 end = FitParams * (u64)(w->id + 1) / (u64)FitThreads;
        for (u64 k = FitParams * (u64)w->id / (u64)FitThreads; k < end; k++) {
            float g = 0.f;
            for (int j = 0; j < FitThreads; j++) {
                g += FitWorkers[j].grad[k];
                FitWorkers[j].grad[k] = 0.f;
            }
            g /= (float)BATCH;
            FitAdamM[k] = 0.9f * FitAdamM[k] + 0.1f * g;
            FitAdamV[k] = 0.999f * FitAdamV[k] + 0.001f * g * g;
            FitParam[k] -= (float)(lr * (FitAdamM[k] / c1) / (sqrt(FitAdamV[k] / c2) + 1e-8));
            /* The rows are what a move sums in 16 bits. */
            if (k < (u64)FitFeatures + (u64)FitFeatures * H) {
                FitParam[k] = FitParam[k] > FIT_LIMIT    ? FIT_LIMIT
                              : FitParam[k] < -FIT_LIMIT ? -FIT_LIMIT
                                                         : FitParam[k];
            }
        }
        fit_barrier();
    }
}

static void fit_score_held(FitWorker *w) {
    memset(&w->score, 0, sizeof(w->score));
    for (int t = 0; t < FitThreads; t++) {
        const FitSet *set = &FitWorkers[t].held;
        for (u64 i = (u64)w->id; i < set->npos; i += (u64)FitThreads) {
            fit_position(w, set, &set->pos[i], NULL, &w->score);
        }
    }
}

static void fit_worker_main(void *arg) {
    FitWorker *w = arg;
    if (FitScoring) {
        fit_score_held(w);
    } else {
        fit_epoch(w);
    }
}

static void fit_run_all(void (*fn)(void *)) {
    ThreadHandle handles[FIT_THREADS];
    for (int t = 0; t < FitThreads; t++) {
        if (!thread_create(&handles[t], fn, &FitWorkers[t])) {
            fprintf(stderr, "cannot start a thread\n");
            exit(1);
        }
    }
    for (int t = 0; t < FitThreads; t++) {
        thread_join(handles[t]);
    }
}

static u64 FitRngState;
static u64 fit_rng(void) {
    FitRngState ^= FitRngState << 13;
    FitRngState ^= FitRngState >> 7;
    FitRngState ^= FitRngState << 17;
    return FitRngState;
}

static double fit_now(void) {
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void fit_report(const char *name, const FitScore *s) {
    const double n = s->n ? (double)s->n : 1.0;
    printf(
        "  %-14s %8llu positions  loss %.4f  top1 %5.2f%%  top3 %5.2f%%  top8 %5.2f%%  rank %.3f\n",
        name, (unsigned long long)s->n, s->loss / n, 100.0 * s->top1 / n, 100.0 * s->top3 / n,
        100.0 * s->top8 / n, s->rank / n);
}

/* ---- the engine's own prior on the rounded weights ---- */

static i16 FitRows[4096 * POLICY_LANES], FitBias[POLICY_LANES], FitOut[POLICY_LANES];

typedef struct FitEngineScore {
    FitScore score;
    Move moves[MAX_MOVES];
    i32 logits[MAX_MOVES];
} FitEngineScore;

static bool fit_score_position(const Board *b, const DataGame *game, int ply, u64 index,
                               void *context) {
    FitEngineScore *e = context;
    if (game->plies[ply].score == DATA_NO_SCORE || ply < game->header.random_plies || b->ply < 2 ||
        index % HELD_OUT != HELD_OUT - 1 || (index / HELD_OUT) % HELD_SCORED != 0) {
        return true;
    }
    const int count = movegen_all(b, e->moves);
    int target      = -1;
    for (int j = 0; j < count; j++) {
        target = e->moves[j] == game->plies[ply].move ? j : target;
    }
    if (target < 0 || count < 2) {
        return true;
    }
    u64 ends[4], their_ends[4];
    board_road_ends(b, board_stm(b), ends);
    board_road_ends(b, !board_stm(b), their_ends);
    PolicyContext ctx;
    policy_context(b, ends, their_ends, fit_prev_move(game, ply, 1), fit_prev_move(game, ply, 2),
                   &ctx);
    policy_logits(b, &ctx, e->moves, count, e->logits);
    double top = -1e30, total = 0.0;
    for (int j = 0; j < count; j++) {
        top = e->logits[j] / 256.0 > top ? e->logits[j] / 256.0 : top;
    }
    int rank = 1;
    for (int j = 0; j < count; j++) {
        total += exp(e->logits[j] / 256.0 - top);
        rank +=
            e->logits[j] > e->logits[target] || (e->logits[j] == e->logits[target] && j < target);
    }
    FitScore *s = &e->score;
    s->n++;
    s->loss += log(total) - (e->logits[target] / 256.0 - top);
    s->rank += rank;
    s->top1 += rank == 1;
    s->top3 += rank <= 3;
    s->top8 += rank <= 8;
    return true;
}

static i16 fit_round(double x) {
    const double q = floor(x + 0.5);
    return (i16)(q > 32767 ? 32767 : q < -32767 ? -32767 : q);
}

static bool fit_write_header(const char *out) {
    FILE *file = fopen(out, "wb");
    if (!file) {
        fprintf(stderr, "cannot write %s\n", out);
        return false;
    }
    fprintf(file,
            "/* policy_net.h - written by `yapper policyfit`. A row of 16 lanes for each feature "
            "of\n * policy.c, its 15 units' biases and output weights. */\n// clang-format off\n"
            "#ifndef POLICY_NET_H\n#define POLICY_NET_H\n\n#define POLICY_NET_FEATURES %d\n"
            "#define POLICY_NET_LANES %d\n\nstatic const i16 PolicyNetBias[%d] = {",
            FitFeatures, POLICY_LANES, POLICY_LANES);
    for (int h = 0; h < POLICY_LANES; h++) {
        fprintf(file, "%s%d", h ? "," : "", FitBias[h]);
    }
    fprintf(file, "};\nstatic const i16 PolicyNetOut[%d] = {", POLICY_LANES);
    for (int h = 0; h < POLICY_LANES; h++) {
        fprintf(file, "%s%d", h ? "," : "", FitOut[h]);
    }
    fprintf(file, "};\nstatic const i16 PolicyNetRows[%d] = {", FitFeatures * POLICY_LANES);
    for (int k = 0; k < FitFeatures * POLICY_LANES; k++) {
        fprintf(file, "%s%d,", k % 16 ? "" : "\n    ", FitRows[k]);
    }
    fprintf(file, "\n};\n\n#endif\n");
    fclose(file);
    return true;
}

int policyfit_run(const char *out, int keep, int epochs, int threads, double rate,
                  unsigned long long seed, char **paths, int path_count) {
    FitThreads   = threads < 1 ? 1 : threads > FIT_THREADS ? FIT_THREADS : threads;
    FitKeep      = keep > 0 ? keep : 1;
    FitPaths     = paths;
    FitPathCount = path_count;
    FitFeatures  = policy_feature_count();
    FitRate      = rate > 0.0 ? rate : 0.003;
    if (FitFeatures > 4096) {
        fprintf(stderr, "too many features\n");
        return 1;
    }
    FitParams  = (u64)FitFeatures + (u64)FitFeatures * H + 2u * H;
    FitWorkers = calloc((size_t)FitThreads, sizeof(FitWorker));
    FitParam   = calloc(FitParams, sizeof(float));
    FitAdamM   = calloc(FitParams, sizeof(float));
    FitAdamV   = calloc(FitParams, sizeof(float));
    if (!FitWorkers || !FitParam || !FitAdamM || !FitAdamV) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    for (int t = 0; t < FitThreads; t++) {
        FitWorkers[t].id   = t;
        FitWorkers[t].pre  = malloc(sizeof(float) * MAX_MOVES * H);
        FitWorkers[t].grad = calloc(FitParams, sizeof(float));
        if (!FitWorkers[t].pre || !FitWorkers[t].grad) {
            fprintf(stderr, "out of memory\n");
            return 1;
        }
    }

    const double started = fit_now();
    fit_run_all(fit_reader_main);
    if (atomic_load(&FitReadFailed)) {
        return 1;
    }
    u64 held = 0, feats = 0;
    FitOrderCount = 0;
    for (int t = 0; t < FitThreads; t++) {
        FitOrderCount += FitWorkers[t].train.npos;
        held += FitWorkers[t].held.npos;
        feats += FitWorkers[t].train.nfeats + FitWorkers[t].held.nfeats;
    }
    printf("%llu positions to fit, %llu held out, %.2f GB of features, read in %.0f s\n",
           (unsigned long long)FitOrderCount, (unsigned long long)held, (double)feats * 2 / 1e9,
           fit_now() - started);
    if (FitOrderCount < BATCH) {
        fprintf(stderr, "too few positions\n");
        return 1;
    }
    FitOrder = malloc(sizeof(FitRef) * FitOrderCount);
    if (!FitOrder) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    u64 at = 0;
    for (int t = 0; t < FitThreads; t++) {
        for (u64 k = 0; k < FitWorkers[t].train.npos; k++) {
            FitOrder[at++] = (FitRef){(u32)t, (u32)k};
        }
    }

    FitRngState = (seed + 1) * 0x9E3779B97F4A7C15ULL;
    for (u64 k = 0; k < (u64)FitFeatures * H; k++) {
        EMB(FitParam)[k] = (float)((double)(fit_rng() % 20001) / 10000.0 - 1.0) * 0.1f;
    }
    for (int h = 0; h < H; h++) {
        /* Units start in the middle of their range, where they pass a gradient. */
        B1(FitParam)[h] = 0.5f;
        W2(FitParam)[h] = (float)((double)(fit_rng() % 20001) / 10000.0 - 1.0) * 0.1f;
    }
    FitStepsAll  = FitOrderCount / BATCH * (u64)epochs;
    FitStepsDone = 0;
    for (int epoch = 1; epoch <= epochs; epoch++) {
        const double begun = fit_now();
        for (u64 k = FitOrderCount - 1; k > 0; k--) {
            const u64 j    = fit_rng() % (k + 1);
            const FitRef x = FitOrder[k];
            FitOrder[k]    = FitOrder[j];
            FitOrder[j]    = x;
        }
        double loss = 0.0;
        u64 fitted  = 0;
        for (int t = 0; t < FitThreads; t++) {
            FitWorkers[t].loss      = 0.0;
            FitWorkers[t].positions = 0;
        }
        FitScoring = false;
        fit_run_all(fit_worker_main);
        FitStepsDone += FitOrderCount / BATCH;
        FitScoring = true;
        fit_run_all(fit_worker_main);
        FitScore s = {0};
        for (int t = 0; t < FitThreads; t++) {
            loss += FitWorkers[t].loss;
            fitted += FitWorkers[t].positions;
            const FitScore *x = &FitWorkers[t].score;
            s.n += x->n;
            s.loss += x->loss;
            s.rank += x->rank;
            s.top1 += x->top1;
            s.top3 += x->top3;
            s.top8 += x->top8;
        }
        printf("epoch %d: fitted at loss %.4f in %.0f s\n", epoch, loss / (double)fitted,
               fit_now() - begun);
        fit_report("held out", &s);
        fflush(stdout);
    }

    for (int f = 0; f < FitFeatures; f++) {
        for (int h = 0; h < H; h++) {
            FitRows[f * POLICY_LANES + h] = fit_round(EMB(FitParam)[(u64)f * H + h] * POLICY_QA);
        }
        FitRows[f * POLICY_LANES + H] = fit_round(WLIN(FitParam)[f] * POLICY_QA);
    }
    for (int h = 0; h < H; h++) {
        FitBias[h] = fit_round(B1(FitParam)[h] * POLICY_QA);
        FitOut[h]  = fit_round(W2(FitParam)[h] * POLICY_QB);
    }
    policy_install(FitRows, FitBias, FitOut);
    FitEngineScore *e = calloc(1, sizeof(FitEngineScore));
    if (!e) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    for (int i = 0; i < path_count; i++) {
        data_replay(paths[i], fit_score_position, e);
    }
    printf("held out, as the engine plays it:\n");
    fit_report("the prior", &e->score);
    free(e);
    policy_install(NULL, NULL, NULL);
    return fit_write_header(out) ? 0 : 1;
}
