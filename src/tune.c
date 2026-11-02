/* tune.c - fits the weights to self-play games, by logistic regression of each position's trace
 * against how its game ended. The score is linear in the weights, so the fit has one answer, and
 * selftest holds a trace equal to evaluate(), so what is fitted is what plays. It lives in the
 * binary because a fit reads millions of positions through the board and the evaluation. */
#include "tune.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "datagen.h"
#include "eval.h"
#include "search.h"
#include "thread.h"

#define TUNE_THREADS       8
#define TUNE_PASSES        800
#define TUNE_RATE          1.5
#define TUNE_RIDGE         1e-8
#define TUNE_HOLD_OUT      16 /* one game in this many is kept out of the fit */
#define SCALE_LOWEST       50.0
#define SCALE_HIGHEST      4000.0
#define TUNE_BETA_MOMENTUM 0.9
#define TUNE_BETA_SQUARES  0.999

typedef struct TuneEntry {
    u16 term;
    i16 count;
} TuneEntry;

typedef struct TuneSample {
    u32 first; /* into `entries` */
    u16 entry_count;
    u16 phase;
    float target; /* 1 for a game the side to move went on to win, 0 for a loss */
} TuneSample;

typedef struct TuneSet {
    TuneSample *samples;
    TuneEntry *entries;
    u64 count, capacity;
    u64 entry_total, entry_capacity;
} TuneSet;

typedef struct TuneLoader {
    TuneSet train, held_out;
    u64 rng;
    int keep;
    u64 games_before; /* games in the files already read */
    u64 last_index;
    bool term_seen[TERM_COUNT]; /* a term no position has keeps the value it came with */
} TuneLoader;

typedef struct TuneJob {
    const TuneSet *set;
    u64 from, to;
    const double *opening, *ending;
    double scale;
    double loss;
    double gradient_opening[TERM_COUNT], gradient_ending[TERM_COUNT];
} TuneJob;

static void sample_add(TuneSet *set, const EvalTrace *trace, float target) {
    if (set->count == set->capacity) {
        set->capacity = set->capacity ? set->capacity * 2 : 1 << 16;
        set->samples  = realloc(set->samples, set->capacity * sizeof(TuneSample));
    }
    if (set->entry_total + TERM_COUNT > set->entry_capacity) {
        set->entry_capacity = set->entry_capacity ? set->entry_capacity * 2 : 1 << 20;
        set->entries        = realloc(set->entries, set->entry_capacity * sizeof(TuneEntry));
    }
    if (!set->samples || !set->entries) {
        printf("out of memory\n");
        exit(1);
    }
    TuneSample *sample = &set->samples[set->count++];
    *sample            = (TuneSample){(u32)set->entry_total, 0, (u16)trace->phase, target};
    for (int term = 0; term < TERM_COUNT; term++) {
        if (trace->counts[term]) {
            set->entries[set->entry_total++] = (TuneEntry){(u16)term, (i16)trace->counts[term]};
            sample->entry_count++;
        }
    }
}

static inline float game_target(Outcome outcome, int us) {
    if (outcome == DRAW) {
        return 0.5f;
    }
    const int winner = outcome == WHITE_ROAD || outcome == WHITE_FLATS ? WHITE : BLACK;
    return winner == us ? 1.0f : 0.0f;
}

/* Only positions the evaluation is ever asked about: a side that can win on the spot, a road
 * being answered and a forced result all go to the search, and never reach a stand pat. */
static bool load_visit(const Board *b, const DataGame *game, int ply, u64 index, void *context) {
    TuneLoader *loader = context;
    const int score    = game->plies[ply].score;
    if (ply < game->header.random_plies || score == DATA_NO_SCORE ||
        abs(score) >= SCORE_WIN_BOUND) {
        return true;
    }
    loader->rng = loader->rng * 0xD1342543DE82EF95ULL + 1;
    if ((loader->rng >> 32) % (u64)loader->keep) {
        return true;
    }
    const int us = board_stm(b);
    if (board_road_placements(b, us) || board_road_placements(b, !us)) {
        return true;
    }
    EvalTrace trace;
    evaluate_trace(b, 0, &trace);
    for (int term = 0; term < TERM_COUNT; term++) {
        loader->term_seen[term] |= trace.counts[term] != 0;
    }
    loader->last_index = index + 1;
    TuneSet *set =
        (loader->games_before + index) % TUNE_HOLD_OUT ? &loader->train : &loader->held_out;
    sample_add(set, &trace, game_target((Outcome)game->header.outcome, us));
    return true;
}

static void tune_job(void *argument) {
    TuneJob *job = argument;
    double loss  = 0.0;
    memset(job->gradient_opening, 0, sizeof(job->gradient_opening));
    memset(job->gradient_ending, 0, sizeof(job->gradient_ending));

    for (u64 i = job->from; i < job->to; i++) {
        const TuneSample *sample = &job->set->samples[i];
        const TuneEntry *entries = &job->set->entries[sample->first];
        const double ending      = (double)sample->phase / PHASE_END;
        const double opening     = 1.0 - ending;
        double score_opening = 0.0, score_ending = 0.0;
        for (int e = 0; e < sample->entry_count; e++) {
            score_opening += job->opening[entries[e].term] * entries[e].count;
            score_ending += job->ending[entries[e].term] * entries[e].count;
        }
        const double score    = score_opening * opening + score_ending * ending;
        const double expected = 1.0 / (1.0 + exp(-score / job->scale));
        const double target   = sample->target;
        loss -= target * log(expected + 1e-12) + (1.0 - target) * log(1.0 - expected + 1e-12);

        const double error = (expected - target) / job->scale;
        for (int e = 0; e < sample->entry_count; e++) {
            const double slope = error * entries[e].count;
            job->gradient_opening[entries[e].term] += slope * opening;
            job->gradient_ending[entries[e].term] += slope * ending;
        }
    }
    job->loss = loss;
}

/* Mean loss over the set, leaving the mean gradient in the two arrays if they are given. */
static double run_pass(const TuneSet *set, const double *opening, const double *ending,
                       double scale, double *gradient_opening, double *gradient_ending) {
    static TuneJob jobs[TUNE_THREADS];
    ThreadHandle handles[TUNE_THREADS];
    for (int i = 0; i < TUNE_THREADS; i++) {
        TuneJob *job = &jobs[i];
        job->set     = set;
        job->from    = set->count * (u64)i / TUNE_THREADS;
        job->to      = set->count * (u64)(i + 1) / TUNE_THREADS;
        job->opening = opening;
        job->ending  = ending;
        job->scale   = scale;
        if (!thread_create(&handles[i], tune_job, job)) {
            printf("cannot start a thread\n");
            exit(1);
        }
    }
    double loss = 0.0;
    for (int i = 0; i < TUNE_THREADS; i++) {
        thread_join(handles[i]);
        loss += jobs[i].loss;
    }
    if (gradient_opening) {
        for (int term = 0; term < TERM_COUNT; term++) {
            gradient_opening[term] = gradient_ending[term] = 0.0;
            for (int i = 0; i < TUNE_THREADS; i++) {
                gradient_opening[term] += jobs[i].gradient_opening[term] / (double)set->count;
                gradient_ending[term] += jobs[i].gradient_ending[term] / (double)set->count;
            }
        }
    }
    return loss / (double)set->count;
}

static double best_scale(const TuneSet *set, const double *opening, const double *ending) {
    const double ratio = 0.618033988749895;
    double low = SCALE_LOWEST, high = SCALE_HIGHEST;
    double left       = high - ratio * (high - low);
    double right      = low + ratio * (high - low);
    double loss_left  = run_pass(set, opening, ending, left, NULL, NULL);
    double loss_right = run_pass(set, opening, ending, right, NULL, NULL);
    for (int i = 0; i < 24; i++) {
        if (loss_left < loss_right) {
            high       = right;
            right      = left;
            loss_right = loss_left;
            left       = high - ratio * (high - low);
            loss_left  = run_pass(set, opening, ending, left, NULL, NULL);
        } else {
            low        = left;
            left       = right;
            loss_left  = loss_right;
            right      = low + ratio * (high - low);
            loss_right = run_pass(set, opening, ending, right, NULL, NULL);
        }
    }
    return (low + high) / 2.0;
}

static bool write_weights(const char *path, const double *opening, const double *ending) {
    FILE *out = fopen(path, "wb");
    if (!out) {
        printf("cannot write %s\n", path);
        return false;
    }
    fprintf(out, "/* weights.h - what each term of the evaluation is worth, in hundredths of a "
                 "flat. Written by\n * `yapper tune`: docs/DATA.md says how. */\n");
    fprintf(out, "#ifndef WEIGHTS_H\n#define WEIGHTS_H\n\n#include \"eval.h\"\n\n");
    fprintf(out, "/* A term's value in the opening and at the end of the game, in one word, so a "
                 "term costs one\n * add. A sum of these stays one as long as each half fits in "
                 "16 bits. */\n");
    fprintf(out, "#define S(opening, ending) ((i32)((u32)(ending) << 16) + (opening))\n\n");
    fprintf(out, "static const i32 Weights[TERM_COUNT] = {\n");
    for (int term = 0; term < TERM_COUNT; term++) {
        fprintf(out, "    [T_%s] = S(%d, %d),\n", EvalTermNames[term], (int)lround(opening[term]),
                (int)lround(ending[term]));
    }
    fprintf(out, "};\n\n#undef S\n\n#endif\n");
    return fclose(out) == 0;
}

int tune_run(const char *out, int keep, double scale, char **paths, int path_count) {
    static TuneLoader loader;
    loader.keep = keep;
    loader.rng  = 0x2545F4914F6CDD1DULL;
    for (int i = 0; i < path_count; i++) {
        loader.last_index = 0;
        if (!data_replay(paths[i], load_visit, &loader)) {
            return 1;
        }
        loader.games_before += loader.last_index;
    }
    const TuneSet *train = &loader.train;
    printf("%llu positions to fit, %llu held out, %.1f terms a position\n",
           (unsigned long long)train->count, (unsigned long long)loader.held_out.count,
           train->count ? (double)train->entry_total / (double)train->count : 0.0);
    if (!train->count || !loader.held_out.count) {
        printf("not enough positions\n");
        return 1;
    }

    static double opening[TERM_COUNT], ending[TERM_COUNT];
    eval_weights(opening, ending);

    if (scale == 0.0) {
        scale = best_scale(train, opening, ending);
        printf("scale %.1f, loss %.6f\n", scale,
               run_pass(train, opening, ending, scale, NULL, NULL));
        return 0;
    }

    const double held_before = run_pass(&loader.held_out, opening, ending, scale, NULL, NULL);
    printf("as built: fit %.6f, held out %.6f\n",
           run_pass(train, opening, ending, scale, NULL, NULL), held_before);

    static double gradient_opening[TERM_COUNT], gradient_ending[TERM_COUNT];
    static double momentum[2][TERM_COUNT], squares[2][TERM_COUNT];
    for (int pass = 1; pass <= TUNE_PASSES; pass++) {
        const double loss =
            run_pass(train, opening, ending, scale, gradient_opening, gradient_ending);
        const double rate = TUNE_RATE * (1.0 - 0.95 * (double)pass / TUNE_PASSES);
        const double kept = 1.0 - pow(TUNE_BETA_MOMENTUM, pass);
        const double seen = 1.0 - pow(TUNE_BETA_SQUARES, pass);
        for (int term = 0; term < TERM_COUNT; term++) {
            if (!loader.term_seen[term]) {
                continue;
            }
            double *weights[2]        = {&opening[term], &ending[term]};
            const double gradients[2] = {gradient_opening[term], gradient_ending[term]};
            for (int half = 0; half < 2; half++) {
                const double gradient = gradients[half] + TUNE_RIDGE * *weights[half];
                double *m             = &momentum[half][term];
                double *v             = &squares[half][term];
                *m = TUNE_BETA_MOMENTUM * *m + (1.0 - TUNE_BETA_MOMENTUM) * gradient;
                *v = TUNE_BETA_SQUARES * *v + (1.0 - TUNE_BETA_SQUARES) * gradient * gradient;
                *weights[half] -= rate * (*m / kept) / (sqrt(*v / seen) + 1e-12);
            }
        }
        if (pass % 100 == 0) {
            printf("pass %4d: fit %.6f\n", pass, loss);
            fflush(stdout);
        }
    }
    printf("fitted: fit %.6f, held out %.6f (was %.6f)\n",
           run_pass(train, opening, ending, scale, NULL, NULL),
           run_pass(&loader.held_out, opening, ending, scale, NULL, NULL), held_before);
    return write_weights(out, opening, ending) ? 0 : 1;
}
