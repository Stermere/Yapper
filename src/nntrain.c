/* nntrain.c - fits a network to self-play games. It lives in the binary for the reason tune.c
 * does, and it reads a position the way play does, through nn_feature() and eval_net_inputs(),
 * so the two cannot drift apart. It trains in floating point with each filter stored once, as
 * the engine is given it, and writes the fixed point the engine runs. */
#include "nntrain.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bitboard.h"
#include "board.h"
#include "datagen.h"
#include "eval.h"
#include "nn.h"
#include "search.h"
#include "thread.h"

#define NT_MAX_THREADS 64
#define NT_BATCH       16384
#define NT_HOLD_OUT    16 /* one game in this many is kept out of the fit, as in tune.c */
#define NT_RATE        0.002
#define NT_RATE_END    0.02 /* of NT_RATE, reached on a cosine by the last batch */
#define NT_BETA1       0.9
#define NT_BETA2       0.999
#define NT_PI          3.14159265358979323846

/* Weights stay inside what the fixed point can hold: 64 stones of the largest filter weight
 * still fit 16 bits with the bias added, and an output weight is at most 127 / NN_QB. */
#define NT_FILTER_LIMIT 1.5f
#define NT_BIAS_LIMIT   1.98f
#define NT_OUTPUT_LIMIT 1.98f

/* The skip inputs are scaled to about one for the fit, and back for the engine. */
#define NT_LEAD_UNIT    16.0f
#define NT_RESERVE_UNIT 16.0f
#define NT_KOMI_UNIT    4.0f

/* Held-out positions kept whole, to score through the engine against the fit. */
#define NT_CHECK_BOARDS 4096

typedef struct NTSample {
    u64 first; /* into `features` */
    u8 count;
    u8 stm;
    u8 bucket;
    u8 result; /* 0 lost, 1 drawn, 2 won, for the side to move */
    i8 lead, reserve, komi;
    i16 score; /* the search's */
    i16 hce;   /* the hand-written evaluation's, to compare against */
} NTSample;

typedef struct NTSet {
    NTSample *samples;
    u16 *features; /* square * NN_TOKENS + token, with the top bit for a black top */
    u64 count, capacity;
    u64 feature_count, feature_capacity;
} NTSet;

typedef struct NTLoader {
    NTSet train, held;
    int size, keep;
    double lambda;
    u64 rng;
    u64 games_before, last_index;
    Board *check;
    int check_count;
} NTLoader;

/* Filters, then bias, output and skip, in one array so the optimiser can walk it as one. */
typedef struct NTModel {
    int size, squares;
    u64 bias_at, output_at, skip_at, count;
    float *p, *m, *v;
    /* `p` rounded to the engine's fixed point. The fit reads only this, so what is fitted is
     * what plays, to within the engine's last rounding, while `p` keeps the small steps. */
    float *q;
    int image[NN_ORIENTATIONS][MAX_SIZE * MAX_SIZE];
} NTModel;

typedef struct NTJob {
    const NTModel *model;
    const NTSet *set;
    const u32 *order;
    u64 from, to;
    double lambda;
    float *gradient; /* NULL to score only */
    double loss, result_loss;
    /* For the update. */
    float **gradients;
    int gradient_count;
    float *p, *m, *v, *q;
    double rate, kept, seen;
    float scale;
} NTJob;

static void set_add(NTSet *set, const NTSample *sample, const u16 *features) {
    if (set->count == set->capacity) {
        set->capacity = set->capacity ? set->capacity * 2 : 1 << 16;
        set->samples  = realloc(set->samples, set->capacity * sizeof(NTSample));
    }
    if (set->feature_count + SQUARE_COUNT > set->feature_capacity) {
        set->feature_capacity = set->feature_capacity ? set->feature_capacity * 2 : 1 << 20;
        set->features         = realloc(set->features, set->feature_capacity * sizeof(u16));
    }
    if (!set->samples || !set->features) {
        printf("out of memory\n");
        exit(1);
    }
    NTSample *added = &set->samples[set->count++];
    *added          = *sample;
    added->first    = set->feature_count;
    memcpy(set->features + set->feature_count, features, sample->count * sizeof(u16));
    set->feature_count += sample->count;
}

static int board_features(const Board *b, u16 *out) {
    int count    = 0;
    u64 occupied = b->owned[WHITE] | b->owned[BLACK];
    while (occupied) {
        const int sq      = pop_lsb(&occupied);
        const int feature = nn_feature(b, sq);
        const int square  = (sq >> 3) * b->size + (sq & 7);
        const int token   = (feature < 0 ? -feature : feature) - 1;
        out[count++]      = (u16)((square * NN_TOKENS + token) | (feature < 0 ? 0x8000 : 0));
    }
    return count;
}

static void sample_of(const Board *b, NTSample *sample, u16 *features) {
    NNInputs in;
    eval_net_inputs(b, &in);
    *sample = (NTSample){.count   = (u8)board_features(b, features),
                         .stm     = (u8)in.stm,
                         .bucket  = (u8)in.bucket,
                         .lead    = (i8)in.lead,
                         .reserve = (i8)in.reserve,
                         .komi    = (i8)in.komi,
                         .hce     = (i16)evaluate_hce(b, 0)};
}

/* The positions tune.c fits on: the evaluation is never asked about the rest. */
static bool nt_visit(const Board *b, const DataGame *game, int ply, u64 index, void *context) {
    NTLoader *loader   = context;
    loader->last_index = index + 1;
    const int score    = game->plies[ply].score;
    if (b->size != loader->size || ply < game->header.random_plies || score == DATA_NO_SCORE ||
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
    NTSample sample;
    u16 features[SQUARE_COUNT];
    sample_of(b, &sample, features);
    sample.score = (i16)score;

    const Outcome outcome = (Outcome)game->header.outcome;
    const int winner      = outcome == WHITE_ROAD || outcome == WHITE_FLATS ? WHITE : BLACK;
    sample.result         = (u8)(outcome == DRAW ? 1 : winner == us ? 2 : 0);

    const bool held = (loader->games_before + index) % NT_HOLD_OUT == 0;
    set_add(held ? &loader->held : &loader->train, &sample, features);
    if (held && loader->check_count < NT_CHECK_BOARDS) {
        loader->check[loader->check_count++] = *b;
    }
    return true;
}

/* How many steps of the engine's fixed point make one of weight `i`. */
static inline double unit_of(const NTModel *model, u64 i) {
    static const double skip_units[4] = {NT_LEAD_UNIT, NT_RESERVE_UNIT, NT_KOMI_UNIT, 1.0};
    if (i < model->output_at) {
        return NN_QA;
    }
    if (i < model->skip_at) {
        return NN_QB;
    }
    return NN_SCALE * 64.0 / skip_units[(i - model->skip_at) % 4];
}

/* Weight `i` as the engine stores it. */
static inline long grid(const NTModel *model, u64 i, float x) {
    return lround((double)x * unit_of(model, i));
}

static inline void snap(NTModel *model, u64 i) {
    model->q[i] = (float)((double)grid(model, i, model->p[i]) / unit_of(model, i));
}

static inline float sigmoid(float x) { return 1.0f / (1.0f + expf(-x)); }

static inline float target_of(const NTSample *s, double lambda) {
    return (float)(lambda * sigmoid((float)s->score / NN_SCALE) + (1.0 - lambda) * s->result / 2.0);
}

/* The model's output for one position, in units of NN_SCALE, keeping what the gradient needs. */
static float forward(const NTModel *model, const NTSample *s, const u16 *features,
                     float z[NN_ORIENTATIONS][NN_FILTERS], float sums[NN_FILTERS]) {
    float acc[NN_ORIENTATIONS][NN_FILTERS];
    memset(acc, 0, sizeof(acc));
    for (int i = 0; i < s->count; i++) {
        const int code   = features[i] & 0x7FFF;
        const float sign = features[i] >> 15 ? -1.0f : 1.0f;
        const int square = code / NN_TOKENS, token = code % NN_TOKENS;
        for (int g = 0; g < NN_ORIENTATIONS; g++) {
            const float *row =
                model->q + ((u64)model->image[g][square] * NN_TOKENS + token) * NN_FILTERS;
            for (int f = 0; f < NN_FILTERS; f++) {
                acc[g][f] += sign * row[f];
            }
        }
    }
    const float side  = s->stm ? -1.0f : 1.0f;
    const float *bias = model->q + model->bias_at;
    const float *wout = model->q + model->output_at + s->bucket * NN_FILTERS;
    const float *skip = model->q + model->skip_at + s->bucket * 4;
    memset(sums, 0, sizeof(float) * NN_FILTERS);
    for (int g = 0; g < NN_ORIENTATIONS; g++) {
        for (int f = 0; f < NN_FILTERS; f++) {
            const float pre = side * acc[g][f] + bias[f];
            const float x   = pre < 0.0f ? 0.0f : pre > 1.0f ? 1.0f : pre;
            z[g][f]         = pre;
            sums[f] += x * x;
        }
    }
    float out = skip[0] * (float)s->lead / NT_LEAD_UNIT +
                skip[1] * (float)s->reserve / NT_RESERVE_UNIT +
                skip[2] * (float)s->komi / NT_KOMI_UNIT + skip[3];
    for (int f = 0; f < NN_FILTERS; f++) {
        out += wout[f] * sums[f];
    }
    return out;
}

static void backward(const NTModel *model, const NTSample *s, const u16 *features,
                     const float z[NN_ORIENTATIONS][NN_FILTERS], const float sums[NN_FILTERS],
                     float slope, float *gradient) {
    const float side  = s->stm ? -1.0f : 1.0f;
    const float *wout = model->q + model->output_at + s->bucket * NN_FILTERS;
    float *gbias      = gradient + model->bias_at;
    float *gout       = gradient + model->output_at + s->bucket * NN_FILTERS;
    float *gskip      = gradient + model->skip_at + s->bucket * 4;
    gskip[0] += slope * (float)s->lead / NT_LEAD_UNIT;
    gskip[1] += slope * (float)s->reserve / NT_RESERVE_UNIT;
    gskip[2] += slope * (float)s->komi / NT_KOMI_UNIT;
    gskip[3] += slope;

    float dacc[NN_ORIENTATIONS][NN_FILTERS];
    for (int f = 0; f < NN_FILTERS; f++) {
        gout[f] += slope * sums[f];
    }
    for (int g = 0; g < NN_ORIENTATIONS; g++) {
        for (int f = 0; f < NN_FILTERS; f++) {
            const float pre = z[g][f];
            const float d   = pre > 0.0f && pre < 1.0f ? slope * wout[f] * 2.0f * pre : 0.0f;
            gbias[f] += d;
            dacc[g][f] = side * d;
        }
    }
    for (int i = 0; i < s->count; i++) {
        const int code   = features[i] & 0x7FFF;
        const float sign = features[i] >> 15 ? -1.0f : 1.0f;
        const int square = code / NN_TOKENS, token = code % NN_TOKENS;
        for (int g = 0; g < NN_ORIENTATIONS; g++) {
            float *row = gradient + ((u64)model->image[g][square] * NN_TOKENS + token) * NN_FILTERS;
            for (int f = 0; f < NN_FILTERS; f++) {
                row[f] += sign * dacc[g][f];
            }
        }
    }
}

static inline double cross_entropy(float target, float predicted) {
    const double p = predicted < 1e-7f ? 1e-7 : predicted > 1.0f - 1e-7f ? 1.0 - 1e-7 : predicted;
    return -(target * log(p) + (1.0 - target) * log(1.0 - p));
}

static void pass_job(void *argument) {
    NTJob *job = argument;
    float z[NN_ORIENTATIONS][NN_FILTERS], sums[NN_FILTERS];
    double loss = 0.0, result_loss = 0.0;
    for (u64 i = job->from; i < job->to; i++) {
        const NTSample *s = &job->set->samples[job->order ? job->order[i] : i];
        const u16 *f      = job->set->features + s->first;
        const float out   = forward(job->model, s, f, z, sums);
        const float p     = sigmoid(out);
        const float t     = target_of(s, job->lambda);
        loss += cross_entropy(t, p);
        result_loss += cross_entropy((float)s->result / 2.0f, p);
        if (job->gradient) {
            backward(job->model, s, f, z, sums, (p - t) * job->scale, job->gradient);
        }
    }
    job->loss        = loss;
    job->result_loss = result_loss;
}

/* Adds up a slice of every thread's gradient, clears them, and takes an Adam step on it. */
static void update_job(void *argument) {
    NTJob *job           = argument;
    const NTModel *model = job->model;
    for (u64 i = job->from; i < job->to; i++) {
        float g = 0.0f;
        for (int t = 0; t < job->gradient_count; t++) {
            g += job->gradients[t][i];
            job->gradients[t][i] = 0.0f;
        }
        job->m[i]         = (float)(NT_BETA1 * job->m[i] + (1.0 - NT_BETA1) * g);
        job->v[i]         = (float)(NT_BETA2 * job->v[i] + (1.0 - NT_BETA2) * g * g);
        float w           = job->p[i] - (float)(job->rate * (job->m[i] / job->kept) /
                                      (sqrt(job->v[i] / job->seen) + 1e-8));
        const float limit = i < model->bias_at     ? NT_FILTER_LIMIT
                            : i < model->output_at ? NT_BIAS_LIMIT
                            : i < model->skip_at   ? NT_OUTPUT_LIMIT
                                                   : 1e9f;
        job->p[i]         = w > limit ? limit : w < -limit ? -limit : w;
        job->q[i]         = (float)((double)grid(model, i, job->p[i]) / unit_of(model, i));
    }
}

static void run_jobs(NTJob *jobs, int threads, ThreadEntry entry) {
    ThreadHandle handles[NT_MAX_THREADS];
    for (int t = 0; t < threads; t++) {
        if (!thread_create(&handles[t], entry, &jobs[t])) {
            printf("cannot start a thread\n");
            exit(1);
        }
    }
    for (int t = 0; t < threads; t++) {
        thread_join(handles[t]);
    }
}

/* Mean loss against the blended target and against the result alone. */
static void score_set(const NTModel *model, const NTSet *set, double lambda, int threads,
                      double *loss, double *result_loss) {
    NTJob jobs[NT_MAX_THREADS];
    for (int t = 0; t < threads; t++) {
        jobs[t] = (NTJob){.model  = model,
                          .set    = set,
                          .from   = set->count * (u64)t / (u64)threads,
                          .to     = set->count * (u64)(t + 1) / (u64)threads,
                          .lambda = lambda};
    }
    run_jobs(jobs, threads, pass_job);
    *loss = *result_loss = 0.0;
    for (int t = 0; t < threads; t++) {
        *loss += jobs[t].loss;
        *result_loss += jobs[t].result_loss;
    }
    *loss /= (double)set->count;
    *result_loss /= (double)set->count;
}

static u64 ModelRng = 0x9E3779B97F4A7C15ULL;

static float model_random(float width) {
    ModelRng = ModelRng * 0xD1342543DE82EF95ULL + 1;
    return ((float)(ModelRng >> 40) / (float)(1 << 24) * 2.0f - 1.0f) * width;
}

static bool model_init(NTModel *model, int size) {
    model->size      = size;
    model->squares   = size * size;
    model->bias_at   = (u64)model->squares * NN_TOKENS * NN_FILTERS;
    model->output_at = model->bias_at + NN_FILTERS;
    model->skip_at   = model->output_at + NN_BUCKETS * NN_FILTERS;
    model->count     = model->skip_at + NN_BUCKETS * 4;
    model->p         = calloc(model->count, sizeof(float));
    model->m         = calloc(model->count, sizeof(float));
    model->v         = calloc(model->count, sizeof(float));
    model->q         = calloc(model->count, sizeof(float));
    if (!model->p || !model->m || !model->v || !model->q) {
        return false;
    }
    /* The same eight transforms the engine spreads the filters over. */
    for (int g = 0; g < NN_ORIENTATIONS; g++) {
        for (int sq = 0; sq < model->squares; sq++) {
            int rank = sq / size, file = sq % size;
            rank                = g & 1 ? size - 1 - rank : rank;
            file                = g & 2 ? size - 1 - file : file;
            model->image[g][sq] = g & 4 ? file * size + rank : rank * size + file;
        }
    }
    for (u64 i = 0; i < model->bias_at; i++) {
        model->p[i] = model_random(0.1f);
    }
    for (int f = 0; f < NN_FILTERS; f++) {
        model->p[model->bias_at + f] = 0.5f;
    }
    for (int i = 0; i < NN_BUCKETS * NN_FILTERS; i++) {
        model->p[model->output_at + i] = model_random(0.05f);
    }
    for (u64 i = 0; i < model->count; i++) {
        snap(model, i);
    }
    return true;
}

typedef struct NTQuantised {
    i16 *filters;
    i16 bias[NN_FILTERS];
    i16 output[NN_BUCKETS * NN_FILTERS];
    i32 skip[NN_BUCKETS * 4];
} NTQuantised;

/* The same integers the fit read. The limits on `p` keep every one inside its type. */
static void quantise_model(const NTModel *model, NTQuantised *q) {
    for (u64 i = 0; i < model->bias_at; i++) {
        q->filters[i] = (i16)grid(model, i, model->p[i]);
    }
    for (int f = 0; f < NN_FILTERS; f++) {
        q->bias[f] = (i16)grid(model, model->bias_at + f, model->p[model->bias_at + f]);
    }
    for (int i = 0; i < NN_BUCKETS * NN_FILTERS; i++) {
        q->output[i] = (i16)grid(model, model->output_at + i, model->p[model->output_at + i]);
    }
    for (int i = 0; i < NN_BUCKETS * 4; i++) {
        q->skip[i] = (i32)grid(model, model->skip_at + i, model->p[model->skip_at + i]);
    }
}

static bool write_array(FILE *out, const char *type, const char *name, const char *length,
                        const void *values, u64 count, bool wide) {
    fprintf(out, "static const %s %s[%s] = {\n", type, name, length);
    for (u64 i = 0; i < count; i++) {
        const long x = wide ? (long)((const i32 *)values)[i] : (long)((const i16 *)values)[i];
        fprintf(out, "%s%ld,%s", i % 24 == 0 ? "    " : "", x,
                i % 24 == 23 || i + 1 == count ? "\n" : "");
    }
    fprintf(out, "};\n\n");
    return !ferror(out);
}

static bool write_header(const char *path, int size, const NTQuantised *q, u64 filter_count) {
    FILE *out = fopen(path, "wb");
    if (!out) {
        printf("cannot write %s\n", path);
        return false;
    }
    char name[32], length[64];
    fprintf(out,
            "/* net%d.h - the network for %dx%d. Written by `yapper nntrain`: docs/DATA.md says "
            "how. */\n",
            size, size, size);
    fprintf(out, "// clang-format off\n#ifndef NET%d_H\n#define NET%d_H\n\n#include \"nn.h\"\n\n",
            size, size);
    snprintf(name, sizeof(name), "Net%dFilters", size);
    snprintf(length, sizeof(length), "%d * NN_TOKENS * NN_FILTERS", size * size);
    bool ok = write_array(out, "i16", name, length, q->filters, filter_count, false);
    snprintf(name, sizeof(name), "Net%dBias", size);
    ok = ok && write_array(out, "i16", name, "NN_FILTERS", q->bias, NN_FILTERS, false);
    snprintf(name, sizeof(name), "Net%dOutput", size);
    ok = ok && write_array(out, "i16", name, "NN_BUCKETS * NN_FILTERS", q->output,
                           NN_BUCKETS * NN_FILTERS, false);
    snprintf(name, sizeof(name), "Net%dSkip", size);
    ok = ok && write_array(out, "i32", name, "NN_BUCKETS * 4", q->skip, NN_BUCKETS * 4, true);
    fprintf(out, "#endif\n");
    return fclose(out) == 0 && ok;
}

/* The fixed point the engine runs, through evaluate() itself, against the fit it came from. */
static void check_engine(const NTModel *model, const NTLoader *loader) {
    double total = 0.0;
    int worst    = 0;
    for (int i = 0; i < loader->check_count; i++) {
        const Board *b = &loader->check[i];
        NTSample s;
        u16 features[SQUARE_COUNT];
        sample_of(b, &s, features);
        float z[NN_ORIENTATIONS][NN_FILTERS], sums[NN_FILTERS];
        const int fitted = (int)lroundf(forward(model, &s, features, z, sums) * NN_SCALE);
        const int played = evaluate(b, 0);
        const int miss   = abs(played - fitted);
        total += miss;
        worst = miss > worst ? miss : worst;
    }
    printf("engine against the fit on %d held-out positions: mean %.2f, worst %d hundredths\n",
           loader->check_count, total / (loader->check_count ? loader->check_count : 1), worst);
}

int nntrain_run(int size, const char *out, int keep, double lambda, int epochs, int threads,
                char **paths, int path_count) {
    if (size < MIN_SIZE || size > MAX_SIZE || keep < 1 || epochs < 1 || threads < 1 ||
        threads > NT_MAX_THREADS) {
        printf("bad arguments\n");
        return 2;
    }
    static NTLoader loader;
    loader.size   = size;
    loader.keep   = keep;
    loader.lambda = lambda;
    loader.rng    = 0x2545F4914F6CDD1DULL;
    loader.check  = malloc(sizeof(Board) * NT_CHECK_BOARDS);
    if (!loader.check) {
        printf("out of memory\n");
        return 1;
    }
    for (int i = 0; i < path_count; i++) {
        loader.last_index = 0;
        if (!data_replay(paths[i], nt_visit, &loader)) {
            return 1;
        }
        loader.games_before += loader.last_index;
    }
    const NTSet *train = &loader.train, *held = &loader.held;
    printf("%llu positions to fit, %llu held out, %.1f stacks a position\n",
           (unsigned long long)train->count, (unsigned long long)held->count,
           train->count ? (double)train->feature_count / (double)train->count : 0.0);
    if (!train->count || !held->count) {
        printf("not enough positions on %dx%d\n", size, size);
        return 1;
    }

    double hce_loss = 0.0;
    for (u64 i = 0; i < held->count; i++) {
        const NTSample *s = &held->samples[i];
        hce_loss += cross_entropy((float)s->result / 2.0f, sigmoid((float)s->hce / NN_SCALE));
    }
    printf("hand-written evaluation, held out against results: %.6f\n",
           hce_loss / (double)held->count);

    static NTModel model;
    if (!model_init(&model, size)) {
        printf("out of memory\n");
        return 1;
    }
    float *gradients[NT_MAX_THREADS];
    for (int t = 0; t < threads; t++) {
        gradients[t] = calloc(model.count, sizeof(float));
        if (!gradients[t]) {
            printf("out of memory\n");
            return 1;
        }
    }
    u32 *order = malloc(train->count * sizeof(u32));
    if (!order) {
        printf("out of memory\n");
        return 1;
    }
    for (u64 i = 0; i < train->count; i++) {
        order[i] = (u32)i;
    }

    const u64 batches = (train->count + NT_BATCH - 1) / NT_BATCH;
    const u64 steps   = batches * (u64)epochs;
    u64 step          = 0;
    u64 shuffle       = 0x853C49E6748FEA9BULL;
    NTJob jobs[NT_MAX_THREADS];
    for (int epoch = 1; epoch <= epochs; epoch++) {
        for (u64 i = train->count - 1; i > 0; i--) {
            shuffle     = shuffle * 0xD1342543DE82EF95ULL + 1;
            const u64 j = (shuffle >> 16) % (i + 1);
            const u32 t = order[i];
            order[i]    = order[j];
            order[j]    = t;
        }
        double loss = 0.0;
        for (u64 batch = 0; batch < batches; batch++, step++) {
            const u64 from = batch * NT_BATCH;
            const u64 to   = from + NT_BATCH < train->count ? from + NT_BATCH : train->count;
            for (int t = 0; t < threads; t++) {
                jobs[t] = (NTJob){.model    = &model,
                                  .set      = train,
                                  .order    = order,
                                  .from     = from + (to - from) * (u64)t / (u64)threads,
                                  .to       = from + (to - from) * (u64)(t + 1) / (u64)threads,
                                  .lambda   = lambda,
                                  .gradient = gradients[t],
                                  .scale    = 1.0f / (float)(to - from)};
            }
            run_jobs(jobs, threads, pass_job);
            for (int t = 0; t < threads; t++) {
                loss += jobs[t].loss;
            }

            const double progress = (double)step / (double)steps;
            const double rate =
                NT_RATE * (NT_RATE_END + (1.0 - NT_RATE_END) * 0.5 * (1.0 + cos(NT_PI * progress)));
            for (int t = 0; t < threads; t++) {
                jobs[t] = (NTJob){.model          = &model,
                                  .from           = model.count * (u64)t / (u64)threads,
                                  .to             = model.count * (u64)(t + 1) / (u64)threads,
                                  .gradients      = gradients,
                                  .gradient_count = threads,
                                  .p              = model.p,
                                  .m              = model.m,
                                  .v              = model.v,
                                  .q              = model.q,
                                  .rate           = rate,
                                  .kept           = 1.0 - pow(NT_BETA1, (double)(step + 1)),
                                  .seen           = 1.0 - pow(NT_BETA2, (double)(step + 1))};
            }
            run_jobs(jobs, threads, update_job);
        }
        double held_loss, held_result;
        score_set(&model, held, lambda, threads, &held_loss, &held_result);
        printf("epoch %3d: fit %.6f, held out %.6f, against results %.6f\n", epoch,
               loss / (double)train->count, held_loss, held_result);
        fflush(stdout);
    }

    NTQuantised q;
    q.filters = malloc(model.bias_at * sizeof(i16));
    if (!q.filters) {
        printf("out of memory\n");
        return 1;
    }
    quantise_model(&model, &q);
    if (!write_header(out, size, &q, model.bias_at)) {
        return 1;
    }
    const NNWeights weights = {q.filters, q.bias, q.output, q.skip};
    if (!nn_install(size, &weights)) {
        printf("out of memory\n");
        return 1;
    }
    check_engine(&model, &loader);
    printf("wrote %s\n", out);
    return 0;
}
