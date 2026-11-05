/* nntrain.c - fits the network to self-play games, every size in them at once. It lives in the
 * binary because it reads millions of positions through the board, which would take Python
 * hours, and it reads a position the way play does, through nn_feature() and eval_net_inputs(),
 * so the two cannot drift apart. It trains in floating point with each filter stored once, as
 * the engine is given it, and writes the fixed point the engine runs.
 *
 * A GPU, if the machine has one, does the passes and the steps. Its kernels are forward(),
 * backward() and update_job() again, and must stay them: the CPU still reads the positions,
 * scores the held-out ones and checks the engine against the fit, so a kernel that drifted
 * would show there. */
#include "nntrain.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bitboard.h"
#include "board.h"
#include "datagen.h"
#include "eval.h"
#include "gpu.h"
#include "nn.h"
#include "search.h"
#include "thread.h"

#define NT_MAX_THREADS 64
#define NT_BATCH       16384
#define NT_GROUP       16 /* batches the GPU is given at once, so threads start once a group */
#define NT_HOLD_OUT    16 /* one game in this many is kept out of the fit */
#define NT_RATE        0.002
#define NT_RATE_END    0.02 /* of NT_RATE, reached on a cosine by the last batch */
#define NT_BETA1       0.9
#define NT_BETA2       0.999
#define NT_PI          3.14159265358979323846

/* The network written is the mean of the weights over this last share of the steps. Even this
 * late, the steps move a held-out loss by up to 0.003 from one epoch to the next, so where a fit
 * happened to stop would otherwise decide much of how good it is. */
#define NT_AVERAGE 0.1

/* Games of one size, generation and budget make a group, and the report scores each. */
#define NT_GROUPS 32

/* Weights stay inside what the fixed point can hold: 64 stones of the largest filter weight
 * still fit 16 bits with the bias added, and an output weight is at most 127 / NN_QB. */
#define NT_FILTER_LIMIT    1.5f
#define NT_BIAS_LIMIT      1.98f
#define NT_OUTPUT_LIMIT    1.98f
#define NT_L2_LIMIT        ((float)NN_L2_LIMIT / NN_Q2)
#define NT_L3_LIMIT        ((float)NN_L3_LIMIT / NN_Q3)
#define NT_L4_LIMIT        ((float)NN_L4_LIMIT / NN_Q4)
#define NT_HEAD_BIAS_LIMIT 4.0f

/* The skip inputs are scaled to about one for the fit, and back for the engine. */
#define NT_LEAD_UNIT    ((float)NN_LEAD_UNIT)
#define NT_RESERVE_UNIT ((float)NN_RESERVE_UNIT)
#define NT_KOMI_UNIT    ((float)NN_KOMI_UNIT)

/* A pooled filter as the engine has it: the sum of its squares over QA * QA, times QA * QA / 2^16.
 */
#define NT_POOL ((float)((double)NN_QA * NN_QA / 65536.0))

/* Held-out positions kept whole, to score through the engine against the fit. */
#define NT_CHECK_PER_SIZE 4096
#define NT_CHECK_BOARDS   (NT_CHECK_PER_SIZE * (MAX_SIZE - MIN_SIZE + 1))

/* Bias, output and skip come a set per size, indexed by the size. */
#define NT_SLOTS (MAX_SIZE + 1)

/* A position's stacks, as the kernels are given them, have a slot this long. */
#define NT_SLOT_SQUARES (MAX_SIZE * MAX_SIZE)

typedef struct NTSample {
    u64 first; /* into `features` */
    u8 count;
    u8 stm;
    u8 bucket;
    u8 result; /* 0 lost, 1 drawn, 2 won, for the side to move */
    i8 lead, reserve, komi;
    u8 group;
    i16 score; /* the search's */
    u8 size;
} NTSample;

typedef struct NTSet {
    NTSample *samples;
    u16 *features; /* square * NN_TOKENS + token, with the top bit for a black top */
    u64 count, capacity;
    u64 feature_count, feature_capacity;
} NTSet;

typedef struct NTGroup {
    int size, generation;
    u32 nodes;
} NTGroup;

typedef struct NTLoader {
    NTSet train, held;
    int keep;
    u64 rng;
    NTGroup groups[NT_GROUPS];
    u64 held_counts[NT_GROUPS];
    int group_count;
    bool groups_full; /* the last group then holds the rest too */
    Board *check;
    int check_count, check_sizes[NT_SLOTS];
    double share[NT_SLOTS]; /* of a size's positions in each epoch, for a size with few games */
    double lambda[NT_SLOTS];
} NTLoader;

/* Filters, then bias, output and skip, in one array so the optimiser can walk it as one. */
typedef struct NTModel {
    u64 bias_at, output_at, skip_at, l2_at, l2_skip_at, l2_bias_at, l3_at, l3_bias_at, l4_at, count;
    float *p, *m, *v;
    /* `p` rounded to the engine's fixed point. The fit reads only this, so what is fitted is
     * what plays, to within the engine's last rounding, while `p` keeps the small steps. */
    float *q;
    float *a; /* the mean of `p` over the steps averaged so far */
    int image[NT_SLOTS][NN_ORIENTATIONS][MAX_SIZE * MAX_SIZE];
} NTModel;

typedef struct NTJob {
    const NTModel *model;
    const NTSet *set;
    const u32 *order;
    u64 from, to;
    const double *lambda;
    float *gradient; /* NULL to score only */
    double loss, result_loss;
    double group_loss[NT_GROUPS]; /* against results */
    u64 group_count[NT_GROUPS];
    /* For the update. */
    float **gradients;
    int gradient_count;
    float *p, *m, *v, *q, *a;
    double rate, kept, seen;
    double average; /* the weight of this step in the mean, or 0 before it starts */
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
                         .size    = (u8)b->size,
                         .lead    = (i8)in.lead,
                         .reserve = (i8)in.reserve,
                         .komi    = (i8)in.komi};
}

static int group_of(NTLoader *loader, const DataHeader *h) {
    for (int i = 0; i < loader->group_count; i++) {
        const NTGroup *g = &loader->groups[i];
        if (g->size == h->size && g->generation == h->generation && g->nodes == h->nodes) {
            return i;
        }
    }
    if (loader->group_count == NT_GROUPS) {
        loader->groups_full = true;
        return NT_GROUPS - 1;
    }
    loader->groups[loader->group_count] = (NTGroup){h->size, h->generation, h->nodes};
    return loader->group_count++;
}

/* A side able to win on the spot, a road being answered, a forced result and the random plies
 * all go to the search rather than to a stand pat, so the evaluation is never asked about
 * them. */
static bool nt_visit(const Board *b, const DataGame *game, int ply, u64 index, void *context) {
    NTLoader *loader = context;
    const int score  = game->plies[ply].score;
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
    NTSample sample;
    u16 features[SQUARE_COUNT];
    sample_of(b, &sample, features);
    sample.score = (i16)score;
    sample.group = (u8)group_of(loader, &game->header);

    const Outcome outcome = (Outcome)game->header.outcome;
    const int winner      = outcome == WHITE_ROAD || outcome == WHITE_FLATS ? WHITE : BLACK;
    sample.result         = (u8)(outcome == DRAW ? 1 : winner == us ? 2 : 0);

    /* By the game's place in its own file, so a game is held out whatever else a fit reads, and
     * two fits that share a file are scored on the same games of it. */
    const bool held = index % NT_HOLD_OUT == 0;
    set_add(held ? &loader->held : &loader->train, &sample, features);
    loader->held_counts[sample.group] += held;
    if (held && loader->check_sizes[b->size] < NT_CHECK_PER_SIZE) {
        loader->check_sizes[b->size]++;
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
    if (i < model->l2_at) {
        return NN_SCALE * 64.0 / skip_units[(i - model->skip_at) % 4];
    }
    return i < model->l2_bias_at   ? NN_Q2
           : i < model->l3_at      ? (double)NN_QU * NN_Q2
           : i < model->l3_bias_at ? NN_Q3
           : i < model->l4_at      ? (double)NN_QY * NN_Q3
                                   : NN_Q4;
}

static inline float limit_of(const NTModel *model, u64 i) {
    return i < model->bias_at      ? NT_FILTER_LIMIT
           : i < model->output_at  ? NT_BIAS_LIMIT
           : i < model->skip_at    ? NT_OUTPUT_LIMIT
           : i < model->l2_at      ? 1e9f
           : i < model->l2_bias_at ? NT_L2_LIMIT
           : i < model->l3_at      ? NT_HEAD_BIAS_LIMIT
           : i < model->l3_bias_at ? NT_L3_LIMIT
           : i < model->l4_at      ? NT_HEAD_BIAS_LIMIT
                                   : NT_L4_LIMIT;
}

/* Weight `i` as the engine stores it. */
static inline long grid(const NTModel *model, u64 i, float x) {
    return lround((double)x * unit_of(model, i));
}

static inline void snap(NTModel *model, u64 i) {
    model->q[i] = (float)((double)grid(model, i, model->p[i]) / unit_of(model, i));
}

static inline float sigmoid(float x) { return 1.0f / (1.0f + expf(-x)); }

/* `lambda` is by size: how far to trust the search's score over the result varies with the board.
 */
static inline float target_of(const NTSample *s, const double *lambda) {
    const double l = lambda[s->size];
    return (float)(l * sigmoid((float)s->score / NN_SCALE) + (1.0 - l) * s->result / 2.0);
}

/* `whose` is 0 for a stack of the view's own side and 1 for the other side's. */
static inline u64 row_at(const NTModel *model, int size, int g, int code, int whose) {
    const int square = code / NN_TOKENS, token = code % NN_TOKENS;
    return ((u64)model->image[size][g][square] * NN_INPUTS + (u64)whose * NN_TOKENS + token) *
           NN_FILTERS;
}

/* Whether a stack is the other side's, from the side to move's view; the other view reverses it. */
static inline int theirs_of(const NTSample *s, u16 feature) { return (feature >> 15) != s->stm; }

static inline u64 bias_of(const NTModel *model, int size) {
    return model->bias_at + (u64)size * NN_FILTERS;
}

/* A bucket's output weights, the side to move's view first. */
/* A size and bucket's set of output, skip and head weights. */
static inline u64 set_of(int size, int bucket) { return (u64)size * NN_BUCKETS + (u64)bucket; }

static inline u64 output_of(const NTModel *model, int size, int bucket) {
    return model->output_at + set_of(size, bucket) * NN_POOLS;
}

static inline u64 skip_of(const NTModel *model, int size, int bucket) {
    return model->skip_at + set_of(size, bucket) * 4;
}

static inline u64 l2_of(const NTModel *model, int size, int bucket) {
    return model->l2_at + set_of(size, bucket) * NN_L2 * NN_POOLS;
}

static inline u64 l2_skip_of(const NTModel *model, int size, int bucket) {
    return model->l2_skip_at + set_of(size, bucket) * NN_L2 * 3;
}

static inline u64 l2_bias_of(const NTModel *model, int size, int bucket) {
    return model->l2_bias_at + set_of(size, bucket) * NN_L2;
}

static inline u64 l3_of(const NTModel *model, int size, int bucket) {
    return model->l3_at + set_of(size, bucket) * NN_L3 * NN_L2;
}

static inline u64 l3_bias_of(const NTModel *model, int size, int bucket) {
    return model->l3_bias_at + set_of(size, bucket) * NN_L3;
}

static inline u64 l4_of(const NTModel *model, int size, int bucket) {
    return model->l4_at + set_of(size, bucket) * NN_L3;
}

static inline float clipped(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }

/* What a pass keeps for the gradient: every unit before its clip, the pools, and the head's units
 * before theirs. */
typedef struct NTActs {
    float z[NN_SIDES][NN_ORIENTATIONS][NN_FILTERS];
    float pooled[NN_POOLS];
    float y[NN_L2];
    float h[NN_L3];
} NTActs;

static inline void skip_inputs(const NTSample *s, float in[3]) {
    in[0] = (float)s->lead / NT_LEAD_UNIT;
    in[1] = (float)s->reserve / NT_RESERVE_UNIT;
    in[2] = (float)s->komi / NT_KOMI_UNIT;
}

/* The model's output for one position, in units of NN_SCALE. View 0 is the side to move's. */
static float forward(const NTModel *model, const NTSample *s, const u16 *features, NTActs *t) {
    float acc[NN_SIDES][NN_ORIENTATIONS][NN_FILTERS];
    memset(acc, 0, sizeof(acc));
    for (int i = 0; i < s->count; i++) {
        const int code   = features[i] & 0x7FFF;
        const int theirs = theirs_of(s, features[i]);
        for (int view = 0; view < NN_SIDES; view++) {
            for (int g = 0; g < NN_ORIENTATIONS; g++) {
                const float *row = model->q + row_at(model, s->size, g, code, theirs ^ view);
                for (int f = 0; f < NN_FILTERS; f++) {
                    acc[view][g][f] += row[f];
                }
            }
        }
    }
    const float *bias   = model->q + bias_of(model, s->size);
    const float *wout   = model->q + output_of(model, s->size, s->bucket);
    const float *skip   = model->q + skip_of(model, s->size, s->bucket);
    const float *l2     = model->q + l2_of(model, s->size, s->bucket);
    const float *l2skip = model->q + l2_skip_of(model, s->size, s->bucket);
    const float *l2bias = model->q + l2_bias_of(model, s->size, s->bucket);
    const float *l3     = model->q + l3_of(model, s->size, s->bucket);
    const float *l3bias = model->q + l3_bias_of(model, s->size, s->bucket);
    const float *l4     = model->q + l4_of(model, s->size, s->bucket);
    float sums[NN_POOLS];
    memset(sums, 0, sizeof(sums));
    for (int view = 0; view < NN_SIDES; view++) {
        for (int g = 0; g < NN_ORIENTATIONS; g++) {
            for (int f = 0; f < NN_FILTERS; f++) {
                const float pre  = acc[view][g][f] + bias[f];
                const float x    = clipped(pre);
                t->z[view][g][f] = pre;
                sums[view * NN_FILTERS + f] += x * x;
            }
        }
    }
    float in[3];
    skip_inputs(s, in);
    float out = skip[0] * in[0] + skip[1] * in[1] + skip[2] * in[2] + skip[3];
    for (int k = 0; k < NN_POOLS; k++) {
        t->pooled[k] = sums[k] * NT_POOL;
        out += wout[k] * t->pooled[k];
    }
    for (int j = 0; j < NN_L2; j++) {
        float y = l2bias[j] + l2skip[j * 3] * in[0] + l2skip[j * 3 + 1] * in[1] +
                  l2skip[j * 3 + 2] * in[2];
        for (int k = 0; k < NN_POOLS; k++) {
            y += l2[j * NN_POOLS + k] * t->pooled[k];
        }
        t->y[j] = y;
    }
    for (int k = 0; k < NN_L3; k++) {
        float h = l3bias[k];
        for (int j = 0; j < NN_L2; j++) {
            h += l3[k * NN_L2 + j] * clipped(t->y[j]);
        }
        t->h[k] = h;
        out += l4[k] * clipped(h);
    }
    return out;
}

static void backward(const NTModel *model, const NTSample *s, const u16 *features, const NTActs *t,
                     float slope, float *gradient) {
    const float *wout = model->q + output_of(model, s->size, s->bucket);
    const float *l2   = model->q + l2_of(model, s->size, s->bucket);
    const float *l3   = model->q + l3_of(model, s->size, s->bucket);
    const float *l4   = model->q + l4_of(model, s->size, s->bucket);
    float *gbias      = gradient + bias_of(model, s->size);
    float *gout       = gradient + output_of(model, s->size, s->bucket);
    float *gskip      = gradient + skip_of(model, s->size, s->bucket);
    float *gl2        = gradient + l2_of(model, s->size, s->bucket);
    float *gl2skip    = gradient + l2_skip_of(model, s->size, s->bucket);
    float *gl2bias    = gradient + l2_bias_of(model, s->size, s->bucket);
    float *gl3        = gradient + l3_of(model, s->size, s->bucket);
    float *gl3bias    = gradient + l3_bias_of(model, s->size, s->bucket);
    float *gl4        = gradient + l4_of(model, s->size, s->bucket);
    float in[3];
    skip_inputs(s, in);
    gskip[0] += slope * in[0];
    gskip[1] += slope * in[1];
    gskip[2] += slope * in[2];
    gskip[3] += slope;

    float dy[NN_L2];
    memset(dy, 0, sizeof(dy));
    for (int k = 0; k < NN_L3; k++) {
        const float h = t->h[k];
        gl4[k] += slope * clipped(h);
        const float dh = h > 0.0f && h < 1.0f ? slope * l4[k] : 0.0f;
        if (dh != 0.0f) {
            gl3bias[k] += dh;
            for (int j = 0; j < NN_L2; j++) {
                gl3[k * NN_L2 + j] += dh * clipped(t->y[j]);
                dy[j] += dh * l3[k * NN_L2 + j];
            }
        }
    }
    for (int j = 0; j < NN_L2; j++) {
        dy[j] = t->y[j] > 0.0f && t->y[j] < 1.0f ? dy[j] : 0.0f;
        if (dy[j] != 0.0f) {
            gl2bias[j] += dy[j];
            gl2skip[j * 3] += dy[j] * in[0];
            gl2skip[j * 3 + 1] += dy[j] * in[1];
            gl2skip[j * 3 + 2] += dy[j] * in[2];
            for (int k = 0; k < NN_POOLS; k++) {
                gl2[j * NN_POOLS + k] += dy[j] * t->pooled[k];
            }
        }
    }
    float dpool[NN_POOLS];
    for (int k = 0; k < NN_POOLS; k++) {
        gout[k] += slope * t->pooled[k];
        float d = slope * wout[k];
        for (int j = 0; j < NN_L2; j++) {
            d += dy[j] * l2[j * NN_POOLS + k];
        }
        dpool[k] = d * NT_POOL;
    }

    float dacc[NN_SIDES][NN_ORIENTATIONS][NN_FILTERS];
    for (int view = 0; view < NN_SIDES; view++) {
        for (int g = 0; g < NN_ORIENTATIONS; g++) {
            for (int f = 0; f < NN_FILTERS; f++) {
                const float pre = t->z[view][g][f];
                const float d =
                    pre > 0.0f && pre < 1.0f ? dpool[view * NN_FILTERS + f] * 2.0f * pre : 0.0f;
                gbias[f] += d;
                dacc[view][g][f] = d;
            }
        }
    }
    for (int i = 0; i < s->count; i++) {
        const int code   = features[i] & 0x7FFF;
        const int theirs = theirs_of(s, features[i]);
        for (int view = 0; view < NN_SIDES; view++) {
            for (int g = 0; g < NN_ORIENTATIONS; g++) {
                float *row = gradient + row_at(model, s->size, g, code, theirs ^ view);
                for (int f = 0; f < NN_FILTERS; f++) {
                    row[f] += dacc[view][g][f];
                }
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
    NTActs acts;
    double loss = 0.0, result_loss = 0.0;
    for (u64 i = job->from; i < job->to; i++) {
        const NTSample *s   = &job->set->samples[job->order ? job->order[i] : i];
        const u16 *f        = job->set->features + s->first;
        const float out     = forward(job->model, s, f, &acts);
        const float p       = sigmoid(out);
        const float t       = target_of(s, job->lambda);
        const double result = cross_entropy((float)s->result / 2.0f, p);
        loss += cross_entropy(t, p);
        result_loss += result;
        job->group_loss[s->group] += result;
        job->group_count[s->group]++;
        if (job->gradient) {
            backward(job->model, s, f, &acts, (p - t) * job->scale, job->gradient);
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
        const float limit = limit_of(model, i);
        job->p[i]         = w > limit ? limit : w < -limit ? -limit : w;
        job->q[i]         = (float)((double)grid(model, i, job->p[i]) / unit_of(model, i));
        if (job->average > 0.0) {
            job->a[i] += (float)((job->p[i] - job->a[i]) * job->average);
        }
    }
}

/* `jobs` is an array of `threads` jobs of `size` bytes each. */
static void run_jobs(void *jobs, size_t size, int threads, ThreadEntry entry) {
    ThreadHandle handles[NT_MAX_THREADS];
    for (int t = 0; t < threads; t++) {
        if (!thread_create(&handles[t], entry, (char *)jobs + size * (size_t)t)) {
            printf("cannot start a thread\n");
            exit(1);
        }
    }
    for (int t = 0; t < threads; t++) {
        thread_join(handles[t]);
    }
}

/* Mean loss against the blended target and against the result alone, and the latter for each
 * group if `by_group` is given. */
static void score_set(const NTModel *model, const NTSet *set, const double *lambda, int threads,
                      double *loss, double *result_loss, double by_group[NT_GROUPS]) {
    NTJob jobs[NT_MAX_THREADS];
    for (int t = 0; t < threads; t++) {
        jobs[t] = (NTJob){.model  = model,
                          .set    = set,
                          .from   = set->count * (u64)t / (u64)threads,
                          .to     = set->count * (u64)(t + 1) / (u64)threads,
                          .lambda = lambda};
    }
    run_jobs(jobs, sizeof(NTJob), threads, pass_job);
    *loss = *result_loss   = 0.0;
    double sums[NT_GROUPS] = {0};
    u64 counts[NT_GROUPS]  = {0};
    for (int t = 0; t < threads; t++) {
        *loss += jobs[t].loss;
        *result_loss += jobs[t].result_loss;
        for (int g = 0; g < NT_GROUPS; g++) {
            sums[g] += jobs[t].group_loss[g];
            counts[g] += jobs[t].group_count[g];
        }
    }
    *loss /= (double)set->count;
    *result_loss /= (double)set->count;
    for (int g = 0; by_group && g < NT_GROUPS; g++) {
        by_group[g] = counts[g] ? sums[g] / (double)counts[g] : 0.0;
    }
}

static u64 ModelRng;

static float model_random(float width) {
    ModelRng = ModelRng * 0xD1342543DE82EF95ULL + 1;
    return ((float)(ModelRng >> 40) / (float)(1 << 24) * 2.0f - 1.0f) * width;
}

static bool model_init(NTModel *model, u64 seed) {
    model->bias_at    = (u64)NN_GRID * NN_GRID * NN_INPUTS * NN_FILTERS;
    model->output_at  = model->bias_at + NT_SLOTS * NN_FILTERS;
    model->skip_at    = model->output_at + NT_SLOTS * NN_BUCKETS * NN_POOLS;
    model->l2_at      = model->skip_at + NT_SLOTS * NN_BUCKETS * 4;
    model->l2_skip_at = model->l2_at + NT_SLOTS * NN_BUCKETS * NN_L2 * NN_POOLS;
    model->l2_bias_at = model->l2_skip_at + NT_SLOTS * NN_BUCKETS * NN_L2 * 3;
    model->l3_at      = model->l2_bias_at + NT_SLOTS * NN_BUCKETS * NN_L2;
    model->l3_bias_at = model->l3_at + NT_SLOTS * NN_BUCKETS * NN_L3 * NN_L2;
    model->l4_at      = model->l3_bias_at + NT_SLOTS * NN_BUCKETS * NN_L3;
    model->count      = model->l4_at + NT_SLOTS * NN_BUCKETS * NN_L3;
    model->p          = calloc(model->count, sizeof(float));
    model->m          = calloc(model->count, sizeof(float));
    model->v          = calloc(model->count, sizeof(float));
    model->q          = calloc(model->count, sizeof(float));
    model->a          = calloc(model->count, sizeof(float));
    if (!model->p || !model->m || !model->v || !model->q || !model->a) {
        return false;
    }
    ModelRng = 0x9E3779B97F4A7C15ULL ^ seed * 0xD1B54A32D192ED03ULL;
    /* The engine's own map from squares to cells, so the two cannot differ. */
    for (int n = MIN_SIZE; n <= MAX_SIZE; n++) {
        for (int g = 0; g < NN_ORIENTATIONS; g++) {
            for (int sq = 0; sq < n * n; sq++) {
                model->image[n][g][sq] = nn_cell(n, g, sq);
            }
        }
    }
    for (u64 i = 0; i < model->bias_at; i++) {
        model->p[i] = model_random(0.1f);
    }
    for (int i = 0; i < NT_SLOTS * NN_FILTERS; i++) {
        model->p[model->bias_at + i] = 0.5f;
    }
    for (int i = 0; i < NT_SLOTS * NN_BUCKETS * NN_POOLS; i++) {
        model->p[model->output_at + i] = model_random(0.05f);
    }
    /* Each unit of the head starts inside its clip, so that it learns from the first step, and
     * adds little to the output until it has. */
    for (u64 i = model->l2_at; i < model->l2_bias_at; i++) {
        model->p[i] = model_random(0.03f);
    }
    for (u64 i = model->l2_bias_at; i < model->l3_at; i++) {
        model->p[i] = 0.5f;
    }
    for (u64 i = model->l3_at; i < model->l3_bias_at; i++) {
        model->p[i] = model_random(0.1f);
    }
    for (u64 i = model->l3_bias_at; i < model->l4_at; i++) {
        model->p[i] = 0.5f;
    }
    for (u64 i = model->l4_at; i < model->count; i++) {
        model->p[i] = model_random(0.05f);
    }
    for (u64 i = 0; i < model->count; i++) {
        snap(model, i);
    }
    return true;
}

typedef struct NTQuantised {
    i16 *filters;
    i16 bias[NT_SLOTS * NN_FILTERS];
    i16 output[NT_SLOTS * NN_BUCKETS * NN_POOLS];
    i32 skip[NT_SLOTS * NN_BUCKETS * 4];
    i16 l2[NT_SLOTS * NN_BUCKETS * NN_L2 * NN_POOLS];
    i16 l2_skip[NT_SLOTS * NN_BUCKETS * NN_L2 * 3];
    i32 l2_bias[NT_SLOTS * NN_BUCKETS * NN_L2];
    i16 l3[NT_SLOTS * NN_BUCKETS * NN_L3 * NN_L2];
    i32 l3_bias[NT_SLOTS * NN_BUCKETS * NN_L3];
    i16 l4[NT_SLOTS * NN_BUCKETS * NN_L3];
} NTQuantised;

/* The same integers the fit read. The limits on `p` keep every one inside its type. */
static void quantise_model(const NTModel *model, NTQuantised *q) {
    for (u64 i = 0; i < model->bias_at; i++) {
        q->filters[i] = (i16)grid(model, i, model->p[i]);
    }
    for (int i = 0; i < NT_SLOTS * NN_FILTERS; i++) {
        q->bias[i] = (i16)grid(model, model->bias_at + i, model->p[model->bias_at + i]);
    }
    for (int i = 0; i < NT_SLOTS * NN_BUCKETS * NN_POOLS; i++) {
        q->output[i] = (i16)grid(model, model->output_at + i, model->p[model->output_at + i]);
    }
    for (int i = 0; i < NT_SLOTS * NN_BUCKETS * 4; i++) {
        q->skip[i] = (i32)grid(model, model->skip_at + i, model->p[model->skip_at + i]);
    }
    for (u64 i = model->l2_at; i < model->l2_skip_at; i++) {
        q->l2[i - model->l2_at] = (i16)grid(model, i, model->p[i]);
    }
    for (u64 i = model->l2_skip_at; i < model->l2_bias_at; i++) {
        q->l2_skip[i - model->l2_skip_at] = (i16)grid(model, i, model->p[i]);
    }
    for (u64 i = model->l2_bias_at; i < model->l3_at; i++) {
        q->l2_bias[i - model->l2_bias_at] = (i32)grid(model, i, model->p[i]);
    }
    for (u64 i = model->l3_at; i < model->l3_bias_at; i++) {
        q->l3[i - model->l3_at] = (i16)grid(model, i, model->p[i]);
    }
    for (u64 i = model->l3_bias_at; i < model->l4_at; i++) {
        q->l3_bias[i - model->l3_bias_at] = (i32)grid(model, i, model->p[i]);
    }
    for (u64 i = model->l4_at; i < model->count; i++) {
        q->l4[i - model->l4_at] = (i16)grid(model, i, model->p[i]);
    }
}

/* What the engine is given for one size. */
static NNWeights weights_for(const NTQuantised *q, int size) {
    return (NNWeights){q->filters,
                       q->bias + size * NN_FILTERS,
                       q->output + size * NN_BUCKETS * NN_POOLS,
                       q->skip + size * NN_BUCKETS * 4,
                       q->l2 + size * NN_BUCKETS * NN_L2 * NN_POOLS,
                       q->l2_skip + size * NN_BUCKETS * NN_L2 * 3,
                       q->l2_bias + size * NN_BUCKETS * NN_L2,
                       q->l3 + size * NN_BUCKETS * NN_L3 * NN_L2,
                       q->l3_bias + size * NN_BUCKETS * NN_L3,
                       q->l4 + size * NN_BUCKETS * NN_L3};
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

/* net.h: the filters, every size's bias, output and skip, and a mask of the sizes it was fitted
 * on, since a size it never saw has only the weights it started with. */
static bool write_header(const char *path, const NTModel *model, const NTQuantised *q, int sizes) {
    FILE *out = fopen(path, "wb");
    if (!out) {
        printf("cannot write %s\n", path);
        return false;
    }
    fprintf(out,
            "/* net.h - the network every size shares. Written by `yapper nntrain`: docs/DATA.md "
            "says how. */\n");
    fprintf(out, "// clang-format off\n#ifndef NET_H\n#define NET_H\n\n#include \"nn.h\"\n\n");
    fprintf(
        out,
        "#define NET_FILTERS %d\n#define NET_SIDES %d\n#define NET_L2 %d\n#define NET_L3 %d\n\n",
        NN_FILTERS, NN_SIDES, NN_L2, NN_L3);
    bool ok = write_array(out, "i16", "NetFilters", "NN_GRID * NN_GRID * NN_INPUTS * NN_FILTERS",
                          q->filters, model->bias_at, false);
    ok      = ok && write_array(out, "i16", "NetBias", "(MAX_SIZE + 1) * NN_FILTERS", q->bias,
                                NT_SLOTS * NN_FILTERS, false);
    ok      = ok && write_array(out, "i16", "NetOutput", "(MAX_SIZE + 1) * NN_BUCKETS * NN_POOLS",
                                q->output, NT_SLOTS * NN_BUCKETS * NN_POOLS, false);
    ok      = ok && write_array(out, "i32", "NetSkip", "(MAX_SIZE + 1) * NN_BUCKETS * 4", q->skip,
                                NT_SLOTS * NN_BUCKETS * 4, true);
    ok = ok && write_array(out, "i16", "NetL2", "(MAX_SIZE + 1) * NN_BUCKETS * NN_L2 * NN_POOLS",
                           q->l2, NT_SLOTS * NN_BUCKETS * NN_L2 * NN_POOLS, false);
    ok = ok && write_array(out, "i16", "NetL2Skip", "(MAX_SIZE + 1) * NN_BUCKETS * NN_L2 * 3",
                           q->l2_skip, NT_SLOTS * NN_BUCKETS * NN_L2 * 3, false);
    ok = ok && write_array(out, "i32", "NetL2Bias", "(MAX_SIZE + 1) * NN_BUCKETS * NN_L2",
                           q->l2_bias, NT_SLOTS * NN_BUCKETS * NN_L2, true);
    ok = ok && write_array(out, "i16", "NetL3", "(MAX_SIZE + 1) * NN_BUCKETS * NN_L3 * NN_L2",
                           q->l3, NT_SLOTS * NN_BUCKETS * NN_L3 * NN_L2, false);
    ok = ok && write_array(out, "i32", "NetL3Bias", "(MAX_SIZE + 1) * NN_BUCKETS * NN_L3",
                           q->l3_bias, NT_SLOTS * NN_BUCKETS * NN_L3, true);
    ok = ok && write_array(out, "i16", "NetL4", "(MAX_SIZE + 1) * NN_BUCKETS * NN_L3", q->l4,
                           NT_SLOTS * NN_BUCKETS * NN_L3, false);
    fprintf(out, "static const int NetSizes = 0x%x;\n\n#endif\n", sizes);
    return fclose(out) == 0 && ok;
}

/* The fixed point the engine runs, through evaluate() itself, against the fit it came from. */
static bool check_engine(const NTModel *model, const NTLoader *loader, const NTQuantised *q,
                         int sizes) {
    for (int size = MIN_SIZE; size <= MAX_SIZE; size++) {
        if (!(sizes >> size & 1)) {
            continue;
        }
        const NNWeights weights = weights_for(q, size);
        if (!nn_install(size, &weights)) {
            printf("out of memory\n");
            return false;
        }
        double total = 0.0;
        int worst = 0, count = 0;
        for (int i = 0; i < loader->check_count; i++) {
            const Board *b = &loader->check[i];
            if (b->size != size) {
                continue;
            }
            NTSample s;
            u16 features[SQUARE_COUNT];
            sample_of(b, &s, features);
            NTActs acts;
            const int fitted = (int)lroundf(forward(model, &s, features, &acts) * NN_SCALE);
            const int played = evaluate(b, 0);
            const int miss   = abs(played - fitted);
            total += miss;
            worst = miss > worst ? miss : worst;
            count++;
        }
        printf("engine against the fit on %d held-out %dx%d positions: mean %.2f, worst %d "
               "hundredths\n",
               count, size, size, total / (count ? count : 1), worst);
    }
    return true;
}

/* Whether position `i` is in an epoch, for a size given `share` of each one. */
static bool in_epoch(u64 i, int epoch, u64 seed, double share) {
    if (share >= 1.0) {
        return true;
    }
    u64 x = i * 0x9E3779B97F4A7C15ULL ^ (u64)epoch * 0xD1B54A32D192ED03ULL ^ seed;
    x     = (x ^ (x >> 31)) * 0xBF58476D1CE4E5B9ULL;
    x     = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return (double)((x ^ (x >> 31)) >> 11) < share * 9007199254740992.0;
}

/* The positions of an epoch, or with no `order` only how many there are. Each epoch sees them in
 * a new order, the same one on every run and every device. */
static u64 epoch_order(u32 *order, const NTLoader *loader, u64 seed, int epoch, u64 *state) {
    const NTSet *train = &loader->train;
    u64 count          = 0;
    for (u64 i = 0; i < train->count; i++) {
        if (in_epoch(i, epoch, seed, loader->share[train->samples[i].size])) {
            if (order) {
                order[count] = (u32)i;
            }
            count++;
        }
    }
    for (u64 i = count - 1; order && i > 0; i--) {
        *state      = *state * 0xD1342543DE82EF95ULL + 1;
        const u64 j = (*state >> 16) % (i + 1);
        const u32 t = order[i];
        order[i]    = order[j];
        order[j]    = t;
    }
    return count;
}

static u64 batches_of(u64 count) { return (count + NT_BATCH - 1) / NT_BATCH; }

static u64 steps_of(const NTLoader *loader, int epochs, u64 seed) {
    u64 steps = 0;
    for (int epoch = 1; epoch <= epochs; epoch++) {
        steps += batches_of(epoch_order(NULL, loader, seed, epoch, NULL));
    }
    return steps;
}

static double rate_at(u64 step, u64 steps) {
    const double progress = (double)step / (double)steps;
    return NT_RATE * (NT_RATE_END + (1.0 - NT_RATE_END) * 0.5 * (1.0 + cos(NT_PI * progress)));
}

/* The weight of a step in the mean of the weights: 0 before the mean starts, then one over the
 * steps in it so far, this one included. */
static double average_at(u64 step, u64 steps) {
    u64 window = (u64)((double)steps * NT_AVERAGE);
    window     = window ? window : 1;
    return step < steps - window ? 0.0 : 1.0 / (double)(step - (steps - window) + 1);
}

static u64 shuffle_seed(u64 seed) { return 0x853C49E6748FEA9BULL ^ seed * 0x9E3779B97F4A7C15ULL; }

/* For a fit on every size, each size's held out against results too, since one can run out of
 * games while the rest still learn. */
static void report_epoch(const NTModel *model, const NTLoader *loader, const double *lambda,
                         int threads, int epoch, double fit) {
    double held_loss, held_result, by_group[NT_GROUPS];
    score_set(model, &loader->held, lambda, threads, &held_loss, &held_result, by_group);
    printf("epoch %3d: fit %.6f, held out %.6f, against results %.6f\n", epoch, fit, held_loss,
           held_result);
    {
        double sums[NT_SLOTS] = {0};
        u64 counts[NT_SLOTS]  = {0};
        for (int g = 0; g < loader->group_count; g++) {
            sums[loader->groups[g].size] += by_group[g] * (double)loader->held_counts[g];
            counts[loader->groups[g].size] += loader->held_counts[g];
        }
        printf("          ");
        for (int n = MIN_SIZE; n <= MAX_SIZE; n++) {
            if (counts[n]) {
                printf(" %dx%d %.6f", n, n, sums[n] / (double)counts[n]);
            }
        }
        printf("\n");
    }
    fflush(stdout);
}

static bool train_cpu(NTModel *model, const NTLoader *loader, const double *lambda, int epochs,
                      int threads, u64 seed, u32 *order) {
    const NTSet *train = &loader->train;
    float *gradients[NT_MAX_THREADS];
    for (int t = 0; t < threads; t++) {
        gradients[t] = calloc(model->count, sizeof(float));
        if (!gradients[t]) {
            return false;
        }
    }
    const u64 steps = steps_of(loader, epochs, seed);
    u64 step        = 0;
    u64 shuffle     = shuffle_seed(seed);
    NTJob jobs[NT_MAX_THREADS];
    for (int epoch = 1; epoch <= epochs; epoch++) {
        const u64 total   = epoch_order(order, loader, seed, epoch, &shuffle);
        const u64 batches = batches_of(total);
        double loss       = 0.0;
        for (u64 batch = 0; batch < batches; batch++, step++) {
            const u64 from = batch * NT_BATCH;
            const u64 to   = from + NT_BATCH < total ? from + NT_BATCH : total;
            for (int t = 0; t < threads; t++) {
                jobs[t] = (NTJob){.model    = model,
                                  .set      = train,
                                  .order    = order,
                                  .from     = from + (to - from) * (u64)t / (u64)threads,
                                  .to       = from + (to - from) * (u64)(t + 1) / (u64)threads,
                                  .lambda   = lambda,
                                  .gradient = gradients[t],
                                  .scale    = 1.0f / (float)(to - from)};
            }
            run_jobs(jobs, sizeof(NTJob), threads, pass_job);
            for (int t = 0; t < threads; t++) {
                loss += jobs[t].loss;
            }
            for (int t = 0; t < threads; t++) {
                jobs[t] = (NTJob){.model          = model,
                                  .from           = model->count * (u64)t / (u64)threads,
                                  .to             = model->count * (u64)(t + 1) / (u64)threads,
                                  .gradients      = gradients,
                                  .gradient_count = threads,
                                  .p              = model->p,
                                  .m              = model->m,
                                  .v              = model->v,
                                  .q              = model->q,
                                  .a              = model->a,
                                  .rate           = rate_at(step, steps),
                                  .average        = average_at(step, steps),
                                  .kept           = 1.0 - pow(NT_BETA1, (double)(step + 1)),
                                  .seen           = 1.0 - pow(NT_BETA2, (double)(step + 1))};
            }
            run_jobs(jobs, sizeof(NTJob), threads, update_job);
        }
        report_epoch(model, loader, lambda, threads, epoch, loss / (double)total);
    }
    return true;
}

/* The kernels. A block is one position and a thread one hidden unit, so a thread's sum over the
 * stacks runs in the order forward() runs it; the output is then added up a warp of filters at a
 * time, a filter a lane, in a fixed tree, and the warps in order. Gradients are added in fixed
 * point, where the order of the adds cannot change the total, so a fit is the same on every run. A
 * filter's are 32-bit, at 2^28: a batch moves one by at most 4, and 64-bit adds cost the card four
 * times as much. The rest, which a batch can move by 8, stay 64-bit at 2^48. */
static const char KernelBody[] =
    "#define F NN_FILTERS\n"
    "#define G NN_ORIENTATIONS\n"
    "#define V NN_SIDES\n"
    "#define FIXED 281474976710656.0\n"
    "#define FIXED32 268435456.0f\n"
    "struct Packed {\n"
    "    unsigned char count, stm, bucket, size;\n"
    "    signed char lead, reserve, komi, spare;\n"
    "    float target;\n"
    "};\n"
    "__device__ static unsigned long long fixed_of(float x) {\n"
    "    return (unsigned long long)__double2ll_rn((double)x * FIXED);\n"
    "}\n"
    "__device__ static int fixed32_of(float x) { return __float2int_rn(x * FIXED32); }\n"
    "__device__ static int row_of(const int *image, int size, int g, unsigned short feature,\n"
    "        int whose) {\n"
    "    const int code = feature & 0x7FFF;\n"
    "    return image[(size * G + g) * IMAGE_STRIDE + code / NN_TOKENS] * NN_INPUTS +\n"
    "           whose * NN_TOKENS + code % NN_TOKENS;\n"
    "}\n"
    "__device__ static float warp_sum(float x) {\n"
    "    for (int width = 16; width > 0; width /= 2) {\n"
    "        x += __shfl_down_sync(0xFFFFFFFFu, x, width);\n"
    "    }\n"
    "    return x;\n"
    "}\n"
    "__device__ static float clipped(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }\n"
    "extern \"C\" __global__ void __launch_bounds__(V * G * F)\n"
    "train_step(const Packed *packed, const unsigned short *stacks,\n"
    "        const float *q, const int *image, int *grad_filters, unsigned long long *grad,\n"
    "        unsigned long long *loss, int squares, int bias_at, int output_at, int skip_at,\n"
    "        int l2_at, int l2_skip_at, int l2_bias_at, int l3_at, int l3_bias_at, int l4_at,\n"
    "        float scale) {\n"
    "    __shared__ unsigned short feature[MAX_SQUARES];\n"
    "    __shared__ float squared[V][G][F];\n"
    "    __shared__ float delta[V][G][F];\n"
    "    __shared__ float pooled[POOLS];\n"
    "    __shared__ float pooled_slope[POOLS];\n"
    "    __shared__ float warp_out[POOLS / 32];\n"
    "    __shared__ float warp_l2[L2][POOLS / 32];\n"
    "    __shared__ float y[L2], y_slope[L2], h[L3], h_slope[L3];\n"
    "    __shared__ float shared_slope;\n"
    "    const Packed s = packed[blockIdx.x];\n"
    "    const int view = threadIdx.x / (G * F), g = threadIdx.x / F % G, f = threadIdx.x % F;\n"
    "    const int k = view * F + f;\n"
    "    if (threadIdx.x < s.count) {\n"
    "        feature[threadIdx.x] = stacks[(size_t)blockIdx.x * squares + threadIdx.x];\n"
    "    }\n"
    "    __syncthreads();\n"
    "    float acc = 0.0f;\n"
    "    for (int i = 0; i < s.count; i++) {\n"
    "        const int whose = ((feature[i] >> 15) != s.stm) ^ view;\n"
    "        acc += q[row_of(image, s.size, g, feature[i], whose) * F + f];\n"
    "    }\n"
    "    const int set = s.size * NN_BUCKETS + s.bucket;\n"
    "    const int bias = bias_at + s.size * F;\n"
    "    const int wout = output_at + set * POOLS;\n"
    "    const int skip_from = skip_at + set * 4;\n"
    "    const int l2 = l2_at + set * L2 * POOLS;\n"
    "    const int l2_skip = l2_skip_at + set * L2 * 3;\n"
    "    const int l2_bias = l2_bias_at + set * L2;\n"
    "    const int l3 = l3_at + set * L3 * L2;\n"
    "    const int l3_bias = l3_bias_at + set * L3;\n"
    "    const int l4 = l4_at + set * L3;\n"
    "    const float in0 = (float)s.lead / LEAD_UNIT, in1 = (float)s.reserve / RESERVE_UNIT;\n"
    "    const float in2 = (float)s.komi / KOMI_UNIT;\n"
    "    const float pre = acc + q[bias + f];\n"
    "    const float x = clipped(pre);\n"
    "    squared[view][g][f] = x * x;\n"
    "    __syncthreads();\n"
    "    if (g == 0) {\n"
    "        float sum = 0.0f;\n"
    "        for (int e = 0; e < G; e++) sum += squared[view][e][f];\n"
    "        pooled[k] = sum * POOL;\n"
    "    }\n"
    "    __syncthreads();\n"
    "    const float u = pooled[k];\n"
    "    if (g == 0) {\n"
    "        const float out = warp_sum(q[wout + k] * u);\n"
    "        if (f % 32 == 0) warp_out[k / 32] = out;\n"
    "    }\n"
    "    for (int j = g; j < L2; j += G) {\n"
    "        const float part = warp_sum(q[l2 + j * POOLS + k] * u);\n"
    "        if (f % 32 == 0) warp_l2[j][k / 32] = part;\n"
    "    }\n"
    "    __syncthreads();\n"
    "    if (threadIdx.x < L2) {\n"
    "        const int j = threadIdx.x;\n"
    "        const float *ws = q + l2_skip + j * 3;\n"
    "        float sum = q[l2_bias + j] + ws[0] * in0 + ws[1] * in1 + ws[2] * in2;\n"
    "        for (int w = 0; w < POOLS / 32; w++) sum += warp_l2[j][w];\n"
    "        y[j] = sum;\n"
    "    }\n"
    "    __syncthreads();\n"
    "    if (threadIdx.x < L3) {\n"
    "        const int c = threadIdx.x;\n"
    "        float sum = q[l3_bias + c];\n"
    "        for (int j = 0; j < L2; j++) sum += q[l3 + c * L2 + j] * clipped(y[j]);\n"
    "        h[c] = sum;\n"
    "    }\n"
    "    __syncthreads();\n"
    "    if (threadIdx.x == 0) {\n"
    "        float out = warp_out[0];\n"
    "        for (int w = 1; w < POOLS / 32; w++) out += warp_out[w];\n"
    "        const float *skip = q + skip_from;\n"
    "        out = skip[0] * in0 + skip[1] * in1 + skip[2] * in2 + skip[3] + out;\n"
    "        for (int c = 0; c < L3; c++) out += q[l4 + c] * clipped(h[c]);\n"
    "        const float p = 1.0f / (1.0f + expf(-out));\n"
    "        const float slope = (p - s.target) * scale;\n"
    "        shared_slope = slope;\n"
    "        const float c = p < 1e-7f ? 1e-7f : p > 1.0f - 1e-7f ? 1.0f - 1e-7f : p;\n"
    "        const float t = s.target;\n"
    "        const float l = -(t * logf(c) + (1.0f - t) * logf(1.0f - c));\n"
    "        atomicAdd(loss, (unsigned long long)__float2ll_rn(l * 4294967296.0f));\n"
    "        unsigned long long *gskip = grad + skip_from;\n"
    "        atomicAdd(gskip + 0, fixed_of(slope * in0));\n"
    "        atomicAdd(gskip + 1, fixed_of(slope * in1));\n"
    "        atomicAdd(gskip + 2, fixed_of(slope * in2));\n"
    "        atomicAdd(gskip + 3, fixed_of(slope));\n"
    "    }\n"
    "    __syncthreads();\n"
    "    const float slope = shared_slope;\n"
    "    if (threadIdx.x < L3) {\n"
    "        const int c = threadIdx.x;\n"
    "        atomicAdd(grad + l4 + c, fixed_of(slope * clipped(h[c])));\n"
    "        const float dh = h[c] > 0.0f && h[c] < 1.0f ? slope * q[l4 + c] : 0.0f;\n"
    "        h_slope[c] = dh;\n"
    "        if (dh != 0.0f) {\n"
    "            atomicAdd(grad + l3_bias + c, fixed_of(dh));\n"
    "            for (int j = 0; j < L2; j++) {\n"
    "                atomicAdd(grad + l3 + c * L2 + j, fixed_of(dh * clipped(y[j])));\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "    __syncthreads();\n"
    "    if (threadIdx.x < L2) {\n"
    "        const int j = threadIdx.x;\n"
    "        float dy = 0.0f;\n"
    "        for (int c = 0; c < L3; c++) dy += h_slope[c] * q[l3 + c * L2 + j];\n"
    "        dy = y[j] > 0.0f && y[j] < 1.0f ? dy : 0.0f;\n"
    "        y_slope[j] = dy;\n"
    "        if (dy != 0.0f) {\n"
    "            atomicAdd(grad + l2_bias + j, fixed_of(dy));\n"
    "            atomicAdd(grad + l2_skip + j * 3, fixed_of(dy * in0));\n"
    "            atomicAdd(grad + l2_skip + j * 3 + 1, fixed_of(dy * in1));\n"
    "            atomicAdd(grad + l2_skip + j * 3 + 2, fixed_of(dy * in2));\n"
    "        }\n"
    "    }\n"
    "    __syncthreads();\n"
    "    for (int j = g; j < L2; j += G) {\n"
    "        if (y_slope[j] != 0.0f) atomicAdd(grad + l2 + j * POOLS + k, fixed_of(y_slope[j] * "
    "u));\n"
    "    }\n"
    "    if (g == 0) {\n"
    "        float d = slope * q[wout + k];\n"
    "        for (int j = 0; j < L2; j++) d += y_slope[j] * q[l2 + j * POOLS + k];\n"
    "        pooled_slope[k] = d * POOL;\n"
    "        atomicAdd(grad + wout + k, fixed_of(slope * u));\n"
    "    }\n"
    "    __syncthreads();\n"
    "    const float d = pre > 0.0f && pre < 1.0f ? pooled_slope[k] * 2.0f * pre : 0.0f;\n"
    "    delta[view][g][f] = d;\n"
    "    __syncthreads();\n"
    "    if (g == 0) {\n"
    "        float delta_sum = 0.0f;\n"
    "        for (int e = 0; e < G; e++) delta_sum += delta[view][e][f];\n"
    "        atomicAdd(grad + bias + f, fixed_of(delta_sum));\n"
    "    }\n"
    "    if (d != 0.0f) {\n"
    "        for (int i = 0; i < s.count; i++) {\n"
    "            const int whose = ((feature[i] >> 15) != s.stm) ^ view;\n"
    "            atomicAdd(grad_filters + row_of(image, s.size, g, feature[i], whose) * F + f,\n"
    "                      fixed32_of(d));\n"
    "        }\n"
    "    }\n"
    "}\n"
    "extern \"C\" __global__ void adam(float *p, float *m, float *v, float *q, float *a,\n"
    "        int *grad_filters, unsigned long long *grad, int count, int bias_at, int output_at,\n"
    "        int skip_at, int l2_at, int l2_bias_at, int l3_at, int l3_bias_at, int l4_at,\n"
    "        double rate, double kept, double seen, double average) {\n"
    "    const int i = blockIdx.x * blockDim.x + threadIdx.x;\n"
    "    if (i >= count) return;\n"
    "    float g;\n"
    "    if (i < bias_at) {\n"
    "        g = (float)((double)grad_filters[i] / (double)FIXED32);\n"
    "        grad_filters[i] = 0;\n"
    "    } else {\n"
    "        g = (float)((double)(long long)grad[i] / FIXED);\n"
    "        grad[i] = 0;\n"
    "    }\n"
    "    m[i] = (float)(BETA1 * m[i] + (1.0 - BETA1) * g);\n"
    "    v[i] = (float)(BETA2 * v[i] + (1.0 - BETA2) * g * g);\n"
    "    const float w = p[i] - (float)(rate * (m[i] / kept) / (sqrt(v[i] / seen) + 1e-8));\n"
    "    const float limit = i < bias_at ? FILTER_LIMIT : i < output_at ? BIAS_LIMIT\n"
    "                      : i < skip_at ? OUTPUT_LIMIT : i < l2_at ? 1e9f\n"
    "                      : i < l2_bias_at ? L2_LIMIT : i < l3_at ? HEAD_BIAS_LIMIT\n"
    "                      : i < l3_bias_at ? L3_LIMIT : i < l4_at ? HEAD_BIAS_LIMIT : L4_LIMIT;\n"
    "    p[i] = w > limit ? limit : w < -limit ? -limit : w;\n"
    "    const int k = (i - skip_at) % 4;\n"
    "    const double skip_unit = k == 0 ? LEAD_UNIT : k == 1 ? RESERVE_UNIT\n"
    "                           : k == 2 ? KOMI_UNIT : 1.0f;\n"
    "    const double unit = i < output_at ? QA : i < skip_at ? QB\n"
    "                      : i < l2_at ? SCALE * 64.0 / skip_unit : i < l2_bias_at ? Q2\n"
    "                      : i < l3_at ? QU * Q2 : i < l3_bias_at ? Q3 : i < l4_at ? QY * Q3 : "
    "Q4;\n"
    "    q[i] = (float)((double)llround((double)p[i] * unit) / unit);\n"
    "    if (average > 0.0) a[i] += (float)((p[i] - a[i]) * average);\n"
    "}\n";

/* The kernels with this build's constants, so the two sides cannot disagree on one. */
static const char *kernel_source(void) {
    static char source[sizeof(KernelBody) + 2048];
    snprintf(source, sizeof(source),
             "#define NN_TOKENS %d\n#define NN_INPUTS %d\n#define NN_SIDES %d\n"
             "#define NN_FILTERS %d\n#define NN_ORIENTATIONS %d\n"
             "#define IMAGE_STRIDE %d\n#define MAX_SQUARES %d\n#define NN_BUCKETS %d\n"
             "#define QA %.17g\n#define QB %.17g\n#define SCALE %.17g\n"
             "#define LEAD_UNIT ((float)%.9g)\n#define RESERVE_UNIT ((float)%.9g)\n"
             "#define KOMI_UNIT ((float)%.9g)\n#define BETA1 %.17g\n#define BETA2 %.17g\n"
             "#define FILTER_LIMIT ((float)%.9g)\n#define BIAS_LIMIT ((float)%.9g)\n"
             "#define OUTPUT_LIMIT ((float)%.9g)\n"
             "#define POOLS %d\n#define L2 %d\n#define L3 %d\n#define QU %.17g\n#define Q2 %.17g\n"
             "#define Q3 %.17g\n#define Q4 %.17g\n#define QY %.17g\n#define POOL ((float)%.9g)\n"
             "#define L2_LIMIT ((float)%.9g)\n#define L3_LIMIT ((float)%.9g)\n"
             "#define L4_LIMIT ((float)%.9g)\n#define HEAD_BIAS_LIMIT ((float)%.9g)\n%s",
             NN_TOKENS, NN_INPUTS, NN_SIDES, NN_FILTERS, NN_ORIENTATIONS, MAX_SIZE * MAX_SIZE,
             SQUARE_COUNT, NN_BUCKETS, (double)NN_QA, (double)NN_QB, (double)NN_SCALE,
             (double)NT_LEAD_UNIT, (double)NT_RESERVE_UNIT, (double)NT_KOMI_UNIT, NT_BETA1,
             NT_BETA2, (double)NT_FILTER_LIMIT, (double)NT_BIAS_LIMIT, (double)NT_OUTPUT_LIMIT,
             NN_POOLS, NN_L2, NN_L3, (double)NN_QU, (double)NN_Q2, (double)NN_Q3, (double)NN_Q4,
             (double)NN_QY, (double)NT_POOL, (double)NT_L2_LIMIT, (double)NT_L3_LIMIT,
             (double)NT_L4_LIMIT, (double)NT_HEAD_BIAS_LIMIT, KernelBody);
    return source;
}

/* One position as the kernels read it. Its stacks follow in a slot of one entry per square. */
typedef struct NTPacked {
    u8 count, stm, bucket, size;
    i8 lead, reserve, komi, spare;
    float target;
} NTPacked;

_Static_assert(sizeof(NTPacked) == 12, "the kernels read this layout");
_Static_assert(NN_FILTERS % 32 == 0, "the kernels add the output up a warp of filters at a time");

typedef struct NTPackJob {
    const NTSet *set;
    const u32 *order;
    u64 from, to, first;
    const double *lambda;
    u64 slot;
    NTPacked *packed;
    u16 *stacks;
} NTPackJob;

static void pack_job(void *argument) {
    const NTPackJob *job = argument;
    for (u64 i = job->from; i < job->to; i++) {
        const NTSample *s = &job->set->samples[job->order[i]];
        const u64 at      = i - job->first;
        job->packed[at]   = (NTPacked){.count   = s->count,
                                       .stm     = s->stm,
                                       .bucket  = s->bucket,
                                       .size    = s->size,
                                       .lead    = s->lead,
                                       .reserve = s->reserve,
                                       .komi    = s->komi,
                                       .target  = target_of(s, job->lambda)};
        memcpy(job->stacks + at * job->slot, job->set->features + s->first, s->count * sizeof(u16));
    }
}

/* Positions `from` to `to` of `order`, laid out for the kernels. */
static void pack_batch(NTPackJob *jobs, int threads, const NTSet *set, const u32 *order, u64 from,
                       u64 to, const double *lambda, u64 slot, NTPacked *packed, u16 *stacks) {
    for (int t = 0; t < threads; t++) {
        jobs[t] = (NTPackJob){.set    = set,
                              .order  = order,
                              .from   = from + (to - from) * (u64)t / (u64)threads,
                              .to     = from + (to - from) * (u64)(t + 1) / (u64)threads,
                              .first  = from,
                              .lambda = lambda,
                              .slot   = slot,
                              .packed = packed,
                              .stacks = stacks};
    }
    run_jobs(jobs, sizeof(NTPackJob), threads, pack_job);
}

/* The CPU lays out each batch while the card works through the one before it. */
static bool train_gpu(NTModel *model, const NTLoader *loader, const double *lambda, int epochs,
                      int threads, u64 seed, u32 *order) {
    const NTSet *train = &loader->train;
    void *step_kernel  = gpu_function("train_step");
    void *adam_kernel  = gpu_function("adam");
    if (!step_kernel || !adam_kernel) {
        return false;
    }
    const u64 slot      = NT_SLOT_SQUARES;
    const u64 group     = (u64)NT_GROUP * NT_BATCH;
    const size_t floats = model->count * sizeof(float);
    GpuPtr p, m, v, q, a, grad_filters, grad, image, loss, packed, stacks;
    if (!gpu_alloc(&p, floats) || !gpu_alloc(&m, floats) || !gpu_alloc(&v, floats) ||
        !gpu_alloc(&q, floats) || !gpu_alloc(&a, floats) ||
        !gpu_alloc(&grad_filters, model->bias_at * sizeof(i32)) ||
        !gpu_alloc(&grad, model->count * sizeof(u64)) || !gpu_alloc(&image, sizeof(model->image)) ||
        !gpu_alloc(&loss, sizeof(u64)) || !gpu_alloc(&packed, group * sizeof(NTPacked)) ||
        !gpu_alloc(&stacks, group * slot * sizeof(u16)) || !gpu_upload(p, model->p, floats) ||
        !gpu_upload(q, model->q, floats) || !gpu_zero(m, floats) || !gpu_zero(v, floats) ||
        !gpu_zero(grad_filters, model->bias_at * sizeof(i32)) ||
        !gpu_zero(grad, model->count * sizeof(u64)) ||
        !gpu_upload(image, model->image, sizeof(model->image))) {
        return false;
    }
    NTPacked *host_packed[2];
    u16 *host_stacks[2];
    for (int b = 0; b < 2; b++) {
        host_packed[b] = gpu_host_alloc(group * sizeof(NTPacked));
        host_stacks[b] = gpu_host_alloc(group * slot * sizeof(u16));
        if (!host_packed[b] || !host_stacks[b]) {
            return false;
        }
    }

    int count = (int)model->count, squares = NT_SLOT_SQUARES;
    int bias_at = (int)model->bias_at, output_at = (int)model->output_at;
    int skip_at = (int)model->skip_at;
    int l2_at = (int)model->l2_at, l2_skip_at = (int)model->l2_skip_at;
    int l2_bias_at = (int)model->l2_bias_at, l3_at = (int)model->l3_at;
    int l3_bias_at = (int)model->l3_bias_at, l4_at = (int)model->l4_at;
    const u64 steps = steps_of(loader, epochs, seed);
    u64 step        = 0;
    u64 shuffle     = shuffle_seed(seed);
    NTPackJob jobs[NT_MAX_THREADS];
    for (int epoch = 1; epoch <= epochs; epoch++) {
        const u64 total  = epoch_order(order, loader, seed, epoch, &shuffle);
        const u64 groups = (total + group - 1) / group;
        if (!gpu_zero(loss, sizeof(u64))) {
            return false;
        }
        pack_batch(jobs, threads, train, order, 0, group < total ? group : total, lambda, slot,
                   host_packed[0], host_stacks[0]);
        for (u64 g = 0; g < groups; g++) {
            const u64 from   = g * group;
            const u64 to     = from + group < total ? from + group : total;
            const int buffer = (int)(g & 1);
            if (!gpu_upload_async(packed, host_packed[buffer], (to - from) * sizeof(NTPacked)) ||
                !gpu_upload_async(stacks, host_stacks[buffer], (to - from) * slot * sizeof(u16))) {
                return false;
            }
            for (u64 at = from; at < to; at += NT_BATCH, step++) {
                const unsigned n       = (unsigned)(at + NT_BATCH < to ? NT_BATCH : to - at);
                GpuPtr batch_packed    = packed + (at - from) * sizeof(NTPacked);
                GpuPtr batch_stacks    = stacks + (at - from) * slot * sizeof(u16);
                float scale            = 1.0f / (float)n;
                double rate            = rate_at(step, steps);
                double kept            = 1.0 - pow(NT_BETA1, (double)(step + 1));
                double seen            = 1.0 - pow(NT_BETA2, (double)(step + 1));
                double average         = average_at(step, steps);
                void *step_arguments[] = {
                    &batch_packed, &batch_stacks, &q,       &image,      &grad_filters, &grad,
                    &loss,         &squares,      &bias_at, &output_at,  &skip_at,      &l2_at,
                    &l2_skip_at,   &l2_bias_at,   &l3_at,   &l3_bias_at, &l4_at,        &scale};
                void *adam_arguments[] = {
                    &p,          &m,       &v,         &q,       &a,     &grad_filters, &grad,
                    &count,      &bias_at, &output_at, &skip_at, &l2_at, &l2_bias_at,   &l3_at,
                    &l3_bias_at, &l4_at,   &rate,      &kept,    &seen,  &average};
                if (!gpu_launch(step_kernel, n, NN_HIDDEN, step_arguments) ||
                    !gpu_launch(adam_kernel, (unsigned)(count + 255) / 256, 256, adam_arguments)) {
                    return false;
                }
            }
            if (g + 1 < groups) {
                pack_batch(jobs, threads, train, order, to, to + group < total ? to + group : total,
                           lambda, slot, host_packed[buffer ^ 1], host_stacks[buffer ^ 1]);
            }
            if (!gpu_wait()) {
                return false;
            }
        }
        u64 sum = 0;
        if (!gpu_download(&sum, loss, sizeof(u64)) || !gpu_download(model->q, q, floats)) {
            return false;
        }
        report_epoch(model, loader, lambda, threads, epoch,
                     (double)sum / 4294967296.0 / (double)total);
    }
    return gpu_download(model->p, p, floats) && gpu_download(model->q, q, floats) &&
           gpu_download(model->a, a, floats);
}

/* Scores the network as written, the mean of the fit's last steps, on each group of games. */
static void report_groups(const NTModel *model, const NTLoader *loader, const double *lambda,
                          int threads) {
    double loss, result, by_group[NT_GROUPS];
    score_set(model, &loader->held, lambda, threads, &loss, &result, by_group);
    printf("averaged: held out %.6f, against results %.6f\n", loss, result);
    for (int g = 0; g < loader->group_count; g++) {
        const NTGroup *group = &loader->groups[g];
        printf("  %dx%d gen %d at %u nodes%s: %.6f against results, %llu held out\n", group->size,
               group->size, group->generation, group->nodes,
               loader->groups_full && g == NT_GROUPS - 1 ? " and the rest" : "", by_group[g],
               (unsigned long long)loader->held_counts[g]);
    }
}

int nntrain_run(const char *out, int keep, const double *lambda, int epochs, int threads, bool gpu,
                unsigned long long seed, const double *share, char **paths, int path_count) {
    if (keep < 1 || epochs < 1 || threads < 1 || threads > NT_MAX_THREADS) {
        printf("bad arguments\n");
        return 2;
    }
    static NTLoader loader;
    loader.keep = keep;
    memcpy(loader.lambda, lambda, sizeof(loader.lambda));
    loader.rng   = 0x2545F4914F6CDD1DULL;
    loader.check = malloc(sizeof(Board) * NT_CHECK_BOARDS);
    if (!loader.check) {
        printf("out of memory\n");
        return 1;
    }
    for (int n = 0; n < NT_SLOTS; n++) {
        loader.share[n] = share ? share[n] : 1.0;
    }
    for (int i = 0; i < path_count; i++) {
        if (!data_replay(paths[i], nt_visit, &loader)) {
            return 1;
        }
    }
    const NTSet *train = &loader.train, *held = &loader.held;
    printf("%llu positions to fit, %llu held out, %.1f stacks a position\n",
           (unsigned long long)train->count, (unsigned long long)held->count,
           train->count ? (double)train->feature_count / (double)train->count : 0.0);
    if (!train->count || !held->count) {
        printf("not enough positions\n");
        return 1;
    }
    int sizes = 0;
    for (int g = 0; g < loader.group_count; g++) {
        sizes |= 1 << loader.groups[g].size;
    }

    static NTModel model;
    if (!model_init(&model, seed)) {
        printf("out of memory\n");
        return 1;
    }
    u32 *order = malloc(train->count * sizeof(u32));
    if (!order) {
        printf("out of memory\n");
        return 1;
    }

    char why[4096] = "not asked for";
    if (gpu && gpu_open(kernel_source(), why, sizeof(why))) {
        printf("training on %s\n", gpu_name());
        if (!train_gpu(&model, &loader, loader.lambda, epochs, threads, seed, order)) {
            printf("%s\n", gpu_error());
            return 1;
        }
    } else {
        printf("training on %d CPU threads, with no GPU: %s\n", threads, why);
        if (!train_cpu(&model, &loader, loader.lambda, epochs, threads, seed, order)) {
            printf("out of memory\n");
            return 1;
        }
    }

    static NTQuantised q;
    q.filters = malloc(model.bias_at * sizeof(i16));
    if (!q.filters) {
        printf("out of memory\n");
        return 1;
    }
    for (u64 i = 0; i < model.count; i++) {
        model.p[i] = model.a[i];
        snap(&model, i);
    }
    report_groups(&model, &loader, loader.lambda, threads);
    quantise_model(&model, &q);
    if (!write_header(out, &model, &q, sizes) || !check_engine(&model, &loader, &q, sizes)) {
        return 1;
    }
    printf("wrote %s\n", out);
    return 0;
}
