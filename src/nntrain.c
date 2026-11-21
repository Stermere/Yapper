/* nntrain.c - fits the networks of the sizes in self-play games and writes src/net<size>.h for
 * each, so a list of one size's games fits that size alone. It lives in the
 * binary because it reads millions of positions through the board, which would take Python
 * hours, and it reads a position the way play does, through nn_feature() and eval_net_inputs(),
 * so the two cannot drift apart. It trains in floating point with each filter stored once, as
 * the engine is given it, and writes the fixed point the engine runs.
 *
 * An NVIDIA GPU does the passes and the steps; a CPU is a hundred times too slow for the games
 * there are. Its forward pass must stay forward(): the CPU scores the held-out positions through
 * forward() and checks the engine against the fit, so a kernel that drifted would show there.
 *
 * The games stay in memory as the files hold them, at 4 bytes a ply, and positions are made from
 * them a block at a time while the fit runs, so what a fit can read is bounded by its games, not
 * by the 80 bytes a position takes once made. */
#include "nntrain.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "bitboard.h"
#include "board.h"
#include "datagen.h"
#include "eval.h"
#include "gpu.h"
#include "movegen.h"
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

/* The fit lays the filters on one grid of NT_GRID by NT_GRID cells, each board with its edges on
 * the grid's edges, so sizes fitted together share a corner, an edge and the lines beside it, and
 * only the middle differs. The engine keeps each size's own cells, which is the grid's part that
 * size reaches, so a size fitted alone is the same fit either way. */
#define NT_GRID MAX_SIZE

/* Line `x` of a board of `n` on the grid: the near half counted from one edge and the rest from
 * the other. */
static int nt_on_grid(int x, int n) { return x < (n + 1) / 2 ? x : NT_GRID - n + x; }

/* The grid cell of a size's cell, as nn_cell() counts them. */
static int nt_grid_cell(int size, int cell) {
    return nt_on_grid(cell / size, size) * NT_GRID + nt_on_grid(cell % size, size);
}

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

/* A file, read whole and kept: its games are replayed each time a block needs them. */
typedef struct NTFile {
    const char *path;
    u8 *bytes;
    u64 size;
    bool ok;
} NTFile;

/* A game the fit trains on. `key` is its listing and its place in its file, and every draw of a
 * position (keep, --share) is made from it, so no draw depends on threads or blocks. */
typedef struct NTGame {
    const u8 *at; /* its header, in its file's bytes */
    u64 key;
    u16 count; /* positions the fit takes from it */
    u8 size;
} NTGame;

/* A path as it was listed: listing a file again weighs it again. With keep at 1 a listing of a
 * file already listed copies the first one's counts and held-out positions rather than replay. */
typedef struct NTListing {
    int file, first; /* `first` is the listing copied, or -1 */
    int last_use;    /* the last listing that copies this one, after which it can go */
    NTGame *games;
    u64 game_count, game_capacity, positions, features;
    NTSet held;
    NTGroup groups[NT_GROUPS]; /* the listing's own, which the loader's then take in order */
    int group_count;
    bool groups_full, ok;
    u32 sizes; /* a bit for each size of its held-out positions */
} NTListing;

typedef struct NTLoader {
    NTSet held;
    NTSet block; /* the positions being fitted now */
    int keep;
    NTGroup groups[NT_GROUPS];
    u64 held_counts[NT_GROUPS];
    int group_count;
    bool groups_full; /* the last group then holds the rest too */
    Board *check;
    int check_count, check_sizes[NT_SLOTS];
    double share[NT_SLOTS]; /* of a size's positions in each epoch, for a size with few games */
    /* The first epoch a size's positions are in. A size with few games for its squares learns
     * best late and briefly, on filters the other sizes have already shaped: E92. */
    int join[NT_SLOTS];
    double lambda[NT_SLOTS];
    double rate; /* at the first step, falling on a cosine from there */
    NTFile *files;
    int file_count;
    NTListing *listings;
    int listing_count;
    NTGame *games;
    u64 game_count, positions, features; /* the training games, and the positions they give */
    u64 block_size;                      /* positions a block holds at most */
    u32 *perm;                           /* the games in this epoch's order */
} NTLoader;

/* Filters, then bias, output and skip, in one array so the optimiser can walk it as one. */
typedef struct NTModel {
    u64 bias_at, output_at, skip_at, l2_at, l2_skip_at, l2_bias_at, l3_at, l3_bias_at, l4_at, count;
    float *p;
    /* `p` rounded to the engine's fixed point. The fit reads only this, so what is fitted is
     * what plays, to within the engine's last rounding, while `p` keeps the small steps. */
    float *q;
    float *a; /* the mean of `p` over the steps averaged so far */
    int image[NT_SLOTS][NN_ORIENTATIONS][MAX_SIZE * MAX_SIZE];
} NTModel;

/* Scores a slice of a set of positions on the CPU. */
typedef struct NTJob {
    const NTModel *model;
    const NTSet *set;
    u64 from, to;
    const double *lambda;
    double loss, result_loss;
    double group_loss[NT_GROUPS]; /* against results */
    u64 group_count[NT_GROUPS];
} NTJob;

/* A set starts small: every listing keeps one for its held-out positions until the listings are
 * put together, and Windows charges a block of this size in full whether or not it is used, so
 * tens of thousands of files at a megabyte each once asked for more than the machine has. */
static void set_add(NTSet *set, const NTSample *sample, const u16 *features) {
    if (set->count == set->capacity) {
        set->capacity = set->capacity ? set->capacity * 2 : 1 << 10;
        set->samples  = realloc(set->samples, set->capacity * sizeof(NTSample));
    }
    if (set->feature_count + SQUARE_COUNT > set->feature_capacity) {
        set->feature_capacity = set->feature_capacity ? set->feature_capacity * 2 : 1 << 15;
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

/* The group of games of one size, generation and budget, added at the end if new. */
static int group_in(NTGroup *groups, int *count, bool *full, NTGroup g) {
    for (int i = 0; i < *count; i++) {
        if (groups[i].size == g.size && groups[i].generation == g.generation &&
            groups[i].nodes == g.nodes) {
            return i;
        }
    }
    if (*count == NT_GROUPS) {
        *full = true;
        return NT_GROUPS - 1;
    }
    groups[*count] = g;
    return (*count)++;
}

static NTGroup group_of(const DataHeader *h) { return (NTGroup){h->size, h->generation, h->nodes}; }

/* The scale the loss reads a score through, by size, which can differ from NN_SCALE: a flat is
 * worth more of a game on a small board. The engine's units stay put, since the search's score in
 * the target ties the output to them. LossRatio is NN_SCALE over it, exactly 1 by default so that
 * a default fit keeps its bytes. */
static float LossScale[NT_SLOTS], LossRatio[NT_SLOTS];

/* With Smooth above 1, the search's score in the target is the mean, as a chance of winning, of
 * the next Smooth scored plies' scores, each read for the side to move here. A search's score
 * moves by about half a flat from one move of a side to its next, and a later search sees
 * further, so the mean is the steadier target. */
static int Smooth = 0;

/* Each step shrinks every filter weight by the rate times this, apart from the step the loss
 * asks for. A row few positions reach is fitted on few examples, and without it nothing pulls
 * such a row back toward zero: E97. The heads are left alone. */
static double Decay;

static int smoothed_score(const DataGame *game, int ply, int size) {
    double sum = 0.0;
    int count  = 0;
    for (int p = ply; p < game->header.plies && count < Smooth; p++) {
        const int raw = game->plies[p].score;
        if (raw == DATA_NO_SCORE) {
            continue;
        }
        const int s = (p - ply) % 2 ? -raw : raw;
        sum += s >= SCORE_WIN_BOUND    ? 1.0
               : s <= -SCORE_WIN_BOUND ? 0.0
                                       : 1.0 / (1.0 + exp(-(double)s / LossScale[size]));
        count++;
    }
    const double mean  = fmin(fmax(sum / count, 1e-4), 1.0 - 1e-4);
    const double score = LossScale[size] * log(mean / (1.0 - mean));
    return (int)lround(fmin(fmax(score, -(SCORE_WIN_BOUND - 1)), SCORE_WIN_BOUND - 1));
}

static u64 nt_mix(u64 x) {
    x = (x ^ (x >> 31)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

/* Whether the fit takes the position before the move at `ply`. A side able to win on the spot, a
 * road being answered, a forced result and the random plies all go to the search rather than to a
 * stand pat, so the evaluation is never asked about them. Of the rest, one in `keep`, drawn from
 * the game's key. */
static bool nt_takes(const Board *b, const DataGame *game, int ply, u64 key, int keep) {
    const int score = game->plies[ply].score;
    if (ply < game->header.random_plies || score == DATA_NO_SCORE ||
        abs(score) >= SCORE_WIN_BOUND) {
        return false;
    }
    if (keep > 1 && nt_mix(key * DATA_MAX_PLIES + (u64)ply + 0x5851F42D4C957F2DULL) % (u64)keep) {
        return false;
    }
    const int us = board_stm(b);
    return !board_road_placements(b, us) && !board_road_placements(b, !us);
}

/* The position as the fit reads it, aimed at the game's result and the search's score. */
static void nt_sample(const Board *b, const DataGame *game, int ply, NTSample *sample,
                      u16 *features) {
    sample_of(b, sample, features);
    const int score = game->plies[ply].score;
    sample->score   = (i16)(Smooth > 1 ? smoothed_score(game, ply, b->size) : score);
    sample->group   = 0;

    const Outcome outcome = (Outcome)game->header.outcome;
    const int winner      = outcome == WHITE_ROAD || outcome == WHITE_FLATS ? WHITE : BLACK;
    sample->result        = (u8)(outcome == DRAW ? 1 : winner == board_stm(b) ? 2 : 0);
}

typedef enum { NT_GAME, NT_END, NT_BAD } NTRead;

/* Copies out the game whose header is at `at`. A file a killed run cut short just ends. */
static NTRead nt_game_at(const u8 *at, const u8 *end, DataGame *game) {
    if ((u64)(end - at) < sizeof(DataHeader)) {
        return NT_END;
    }
    memcpy(&game->header, at, sizeof(DataHeader));
    const DataHeader *h = &game->header;
    if (h->magic != DATA_MAGIC || h->size < MIN_SIZE || h->size > MAX_SIZE ||
        h->plies > DATA_MAX_PLIES) {
        return NT_BAD;
    }
    if ((u64)(end - at) < sizeof(DataHeader) + (u64)h->plies * sizeof(DataPly)) {
        return NT_END;
    }
    memcpy(game->plies, at + sizeof(DataHeader), (u64)h->plies * sizeof(DataPly));
    return NT_GAME;
}

typedef void (*NTVisit)(const Board *b, const DataGame *game, int ply, void *context);

/* Plays a game, calling `visit` at each position the fit takes. False at an illegal move. */
static bool nt_play(const DataGame *game, u64 key, int keep, NTVisit visit, void *context) {
    Board board;
    Undo undo;
    board_clear(&board, game->header.size, game->header.halfkomi);
    for (int ply = 0; ply < game->header.plies; ply++) {
        const Move m = game->plies[ply].move;
        if (!movegen_is_legal(&board, m)) {
            return false;
        }
        if (nt_takes(&board, game, ply, key, keep)) {
            visit(&board, game, ply, context);
        }
        board_make(&board, m, &undo);
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
    return (float)(l * sigmoid((float)s->score / LossScale[s->size]) + (1.0 - l) * s->result / 2.0);
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

/* What a pass works through: every unit before its clip, the pools, and the head's units before
 * theirs. */
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

static inline double cross_entropy(float target, float predicted) {
    const double p = predicted < 1e-7f ? 1e-7 : predicted > 1.0f - 1e-7f ? 1.0 - 1e-7 : predicted;
    return -(target * log(p) + (1.0 - target) * log(1.0 - p));
}

static void pass_job(void *argument) {
    NTJob *job = argument;
    NTActs acts;
    double loss = 0.0, result_loss = 0.0;
    for (u64 i = job->from; i < job->to; i++) {
        const NTSample *s   = &job->set->samples[i];
        const u16 *f        = job->set->features + s->first;
        const float out     = forward(job->model, s, f, &acts);
        const float ratio   = LossRatio[s->size];
        const float p       = sigmoid(out * ratio);
        const float t       = target_of(s, job->lambda);
        const double result = cross_entropy((float)s->result / 2.0f, p);
        loss += cross_entropy(t, p);
        result_loss += result;
        job->group_loss[s->group] += result;
        job->group_count[s->group]++;
    }
    job->loss        = loss;
    job->result_loss = result_loss;
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

/* Where the fit stands, for tools/progress.py, which lists every long job. The file is rewritten
 * in place at most every NT_STATUS_EVERY seconds, and a reader that catches it half written keeps
 * its last good read. The clock goes only into this file, never into the fit. */
#define NT_STATUS_EPOCHS 64
#define NT_STATUS_EVERY  2.0

static struct {
    bool on;
    int writes;
    char path[1024], out[1024], device[160];
    const char *phase;
    double started, written, read_at, train_at, averaged[2];
    bool scored;
    int file, files, epoch, epochs, losses;
    u64 positions, step, steps;
    double loss[NT_STATUS_EPOCHS][3];
} Status;

static double wall_clock(void) {
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void json_string(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') {
            fputc('\\', f);
        }
        fputc(*s, f);
    }
    fputc('"', f);
}

/* `code` is the exit code once the fit has ended, and -1 while it runs. */
static void status_write(bool now, int code) {
    const double t = wall_clock();
    if (!Status.on || (!now && t - Status.written < NT_STATUS_EVERY)) {
        return;
    }
    Status.written = t;
    FILE *f        = fopen(Status.path, "w");
    if (!f) {
        Status.on = Status.writes > 0; /* no folder for it, rather than a reader in the way */
        return;
    }
    Status.writes++;
    fprintf(f, "{\"kind\": \"fit\", \"out\": ");
    json_string(f, Status.out);
    fprintf(f, ", \"device\": ");
    json_string(f, Status.device);
    fprintf(f,
            ", \"pid\": %d, \"started\": %.3f, \"updated\": %.3f, \"phase\": \"%s\", "
            "\"files\": [%d, %d], \"positions\": %llu, \"read_at\": %.3f, \"train_at\": %.3f, "
            "\"epoch\": %d, \"epochs\": %d, \"step\": %llu, \"steps\": %llu, \"losses\": [",
            process_id(), Status.started, t, Status.phase, Status.file, Status.files,
            (unsigned long long)Status.positions, Status.read_at, Status.train_at, Status.epoch,
            Status.epochs, (unsigned long long)Status.step, (unsigned long long)Status.steps);
    for (int e = 0; e < Status.losses; e++) {
        fprintf(f, "%s[%.6f, %.6f, %.6f]", e ? ", " : "", Status.loss[e][0], Status.loss[e][1],
                Status.loss[e][2]);
    }
    fprintf(f, "]");
    if (Status.scored) {
        fprintf(f, ", \"averaged\": [%.6f, %.6f]", Status.averaged[0], Status.averaged[1]);
    }
    if (code >= 0) {
        fprintf(f, ", \"finished\": true, \"code\": %d", code);
    }
    fprintf(f, "}\n");
    fclose(f);
}

static void status_phase(const char *phase) {
    Status.phase = phase;
    status_write(true, -1);
}

/* With no path given, external/games/live/fit-<folder>-<file>.json after the network's last two
 * path parts, which a fit run from anywhere but the repository's root has no folder for. */
static void status_open(const char *path, const char *out, int epochs, int files) {
    const char *base = out, *parent = out;
    for (const char *c = out; *c; c++) {
        if (*c == '/' || *c == '\\') {
            parent = base;
            base   = c + 1;
        }
    }
    char name[256];
    snprintf(name, sizeof(name), "%s", parent);
    for (char *c = name; *c; c++) {
        const bool plain = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                           (*c >= '0' && *c <= '9') || *c == '-' || *c == '_';
        *c = plain ? *c : '-';
    }
    if (path) {
        snprintf(Status.path, sizeof(Status.path), "%s", path);
    } else {
        snprintf(Status.path, sizeof(Status.path), "external/games/live/fit-%s.json", name);
    }
    snprintf(Status.out, sizeof(Status.out), "%s", out);
    Status.on      = true;
    Status.started = wall_clock();
    Status.epochs  = epochs;
    Status.files   = files;
    status_phase("reading");
}

/* Reads a whole file. Read in chunks rather than sized by ftell, which stops at 2 GB on Windows. */
static bool nt_read_file(NTFile *f) {
    FILE *in = fopen(f->path, "rb");
    if (!in) {
        printf("cannot read %s\n", f->path);
        return false;
    }
    u64 capacity = 1 << 24, size = 0;
    u8 *bytes = malloc(capacity);
    while (bytes) {
        size += fread(bytes + size, 1, capacity - size, in);
        if (size < capacity) {
            break;
        }
        capacity *= 2;
        u8 *grown = realloc(bytes, capacity);
        if (!grown) {
            free(bytes);
        }
        bytes = grown;
    }
    const bool failed = !bytes || ferror(in);
    fclose(in);
    if (failed) {
        printf(bytes ? "cannot read %s\n" : "out of memory reading %s\n", f->path);
        free(bytes);
        return false;
    }
    u8 *fitted = realloc(bytes, size + 1); /* and room for a terminating 0, for a text file */
    f->bytes   = fitted ? fitted : bytes;
    f->size    = size;
    return true;
}

typedef struct NTCount {
    NTListing *listing;
    bool held;
    u16 count;
    u64 features;
} NTCount;

/* A held-out position is made now, and a training one only counted: it is made again each time
 * its block comes round. */
static void nt_count_visit(const Board *b, const DataGame *game, int ply, void *context) {
    NTCount *c = context;
    if (c->held) {
        NTListing *l = c->listing;
        NTSample sample;
        u16 features[SQUARE_COUNT];
        nt_sample(b, game, ply, &sample, features);
        sample.group =
            (u8)group_in(l->groups, &l->group_count, &l->groups_full, group_of(&game->header));
        set_add(&l->held, &sample, features);
        l->sizes |= 1u << b->size;
    } else {
        c->count++;
        c->features += (u64)popcount(b->owned[WHITE] | b->owned[BLACK]);
    }
}

/* Replays a listing's games once, for what each gives the fit. A game is held out by its place in
 * its own file, so it is held out whatever else a fit reads, and two fits that share a file are
 * scored on the same games of it. */
static bool nt_count_listing(const NTLoader *loader, int index) {
    NTListing *l    = &loader->listings[index];
    const NTFile *f = &loader->files[l->file];
    const u8 *at    = f->bytes;
    const u8 *end   = f->bytes + f->size;
    static _Thread_local DataGame game;
    for (u64 place = 0;; place++) {
        const NTRead read = nt_game_at(at, end, &game);
        if (read == NT_END) {
            return true;
        }
        if (read == NT_BAD) {
            printf("%s: game %llu has a bad header\n", f->path, (unsigned long long)place);
            return false;
        }
        const u64 key = (u64)index << 32 | place;
        NTCount c     = {.listing = l, .held = place % NT_HOLD_OUT == 0};
        if (!nt_play(&game, key, loader->keep, nt_count_visit, &c)) {
            printf("%s: game %llu has an illegal move\n", f->path, (unsigned long long)place);
            return false;
        }
        if (c.count) {
            if (l->game_count == l->game_capacity) {
                l->game_capacity = l->game_capacity ? l->game_capacity * 2 : 1 << 12;
                NTGame *grown    = realloc(l->games, l->game_capacity * sizeof(NTGame));
                if (!grown) {
                    printf("out of memory\n");
                    return false;
                }
                l->games = grown;
            }
            l->games[l->game_count++] = (NTGame){at, key, c.count, game.header.size};
            l->positions += c.count;
            l->features += c.features;
        }
        at += sizeof(DataHeader) + (u64)game.header.plies * sizeof(DataPly);
    }
}

typedef struct NTReadJob {
    NTLoader *loader;
    atomic_int *next, *done;
    bool files; /* reading the files, or then counting the listings */
} NTReadJob;

static void nt_read_job(void *argument) {
    NTReadJob *job   = argument;
    NTLoader *loader = job->loader;
    const int count  = job->files ? loader->file_count : loader->listing_count;
    for (int i; (i = atomic_fetch_add(job->next, 1)) < count; atomic_fetch_add(job->done, 1)) {
        if (job->files) {
            loader->files[i].ok = nt_read_file(&loader->files[i]);
        } else {
            NTListing *l = &loader->listings[i];
            l->ok        = l->first >= 0 || nt_count_listing(loader, i);
        }
    }
}

/* Every file, or every listing, on every thread, the status file kept current meanwhile. */
static bool nt_read_all(NTLoader *loader, int threads, bool files) {
    atomic_int next = 0, done = 0;
    const int count = files ? loader->file_count : loader->listing_count;
    NTReadJob jobs[NT_MAX_THREADS];
    ThreadHandle handles[NT_MAX_THREADS];
    for (int t = 0; t < threads; t++) {
        jobs[t] = (NTReadJob){loader, &next, &done, files};
        if (!thread_create(&handles[t], nt_read_job, &jobs[t])) {
            printf("cannot start a thread\n");
            exit(1);
        }
    }
    while (atomic_load(&done) < count) {
        Status.file = files ? 0 : atomic_load(&done);
        status_write(false, -1);
        thread_sleep_ms(100);
    }
    for (int t = 0; t < threads; t++) {
        thread_join(handles[t]);
    }
    Status.file = files ? 0 : count;
    for (int i = 0; i < count; i++) {
        if (files ? !loader->files[i].ok : !loader->listings[i].ok) {
            return false;
        }
    }
    return true;
}

/* The listings put together in the order listed, so nothing depends on which thread read what. A
 * listing's own copy goes once the last listing that copies it is in. */
static bool nt_merge(NTLoader *loader) {
    NTListing *ls = loader->listings;
    u64 games = 0, held = 0, held_features = 0;
    for (int i = 0; i < loader->listing_count; i++) {
        ls[i].last_use = i;
    }
    for (int i = 0; i < loader->listing_count; i++) {
        const NTListing *src = ls[i].first >= 0 ? &ls[ls[i].first] : &ls[i];
        games += src->game_count;
        held += src->held.count;
        held_features += src->held.feature_count;
        if (ls[i].first >= 0) {
            ls[ls[i].first].last_use = i;
        }
    }
    NTSet *h            = &loader->held;
    loader->games       = malloc((games ? games : 1) * sizeof(NTGame));
    h->samples          = malloc((held ? held : 1) * sizeof(NTSample));
    h->features         = malloc((held_features ? held_features : 1) * sizeof(u16));
    h->capacity         = held;
    h->feature_capacity = held_features;
    if (!loader->games || !h->samples || !h->features) {
        printf("out of memory\n");
        return false;
    }
    for (int i = 0; i < loader->listing_count; i++) {
        NTListing *src = ls[i].first >= 0 ? &ls[ls[i].first] : &ls[i];
        int map[NT_GROUPS];
        for (int g = 0; g < src->group_count; g++) {
            map[g] = group_in(loader->groups, &loader->group_count, &loader->groups_full,
                              src->groups[g]);
        }
        for (u64 g = 0; g < src->game_count; g++) {
            NTGame game                         = src->games[g];
            game.key                            = (u64)i << 32 | (game.key & 0xFFFFFFFFu);
            loader->games[loader->game_count++] = game;
        }
        for (u64 k = 0; k < src->held.count; k++) {
            NTSample s = src->held.samples[k];
            s.group    = (u8)map[s.group];
            s.first += h->feature_count;
            h->samples[h->count++] = s;
            loader->held_counts[s.group]++;
        }
        memcpy(h->features + h->feature_count, src->held.features,
               src->held.feature_count * sizeof(u16));
        h->feature_count += src->held.feature_count;
        loader->positions += src->positions;
        loader->features += src->features;
        if (src->last_use == i) {
            free(src->games);
            free(src->held.samples);
            free(src->held.features);
            src->games         = NULL;
            src->held.samples  = NULL;
            src->held.features = NULL;
        }
    }
    return true;
}

static void nt_check_visit(const Board *b, const DataGame *game, int ply, void *context) {
    (void)game;
    (void)ply;
    NTLoader *loader = context;
    if (loader->check_sizes[b->size] < NT_CHECK_PER_SIZE) {
        loader->check_sizes[b->size]++;
        loader->check[loader->check_count++] = *b;
    }
}

/* Held-out positions kept whole, to score the engine against the fit: the first of each size, in
 * the order listed. */
static void nt_gather_checks(NTLoader *loader) {
    u32 wanted = 0;
    for (int g = 0; g < loader->group_count; g++) {
        wanted |= 1u << loader->groups[g].size;
    }
    static DataGame game;
    for (int i = 0; i < loader->listing_count; i++) {
        u32 open = 0;
        for (int n = MIN_SIZE; n <= MAX_SIZE; n++) {
            open |= (wanted >> n & 1) && loader->check_sizes[n] < NT_CHECK_PER_SIZE ? 1u << n : 0;
        }
        if (!open) {
            return;
        }
        const NTListing *l = &loader->listings[i];
        if (l->first >= 0 || !(l->sizes & open)) {
            continue;
        }
        const NTFile *f = &loader->files[l->file];
        const u8 *at    = f->bytes;
        for (u64 place = 0; nt_game_at(at, f->bytes + f->size, &game) == NT_GAME; place++) {
            if (place % NT_HOLD_OUT == 0 && (open >> game.header.size & 1)) {
                nt_play(&game, (u64)i << 32 | place, loader->keep, nt_check_visit, loader);
            }
            at += sizeof(DataHeader) + (u64)game.header.plies * sizeof(DataPly);
        }
    }
}

/* Every path listed: each distinct file read once, each listing replayed on every thread, then put
 * together. */
static bool nt_load(NTLoader *loader, char **paths, int path_count, int threads) {
    loader->listings = calloc((size_t)path_count, sizeof(NTListing));
    loader->files    = calloc((size_t)path_count, sizeof(NTFile));
    if (!loader->listings || !loader->files) {
        printf("out of memory\n");
        return false;
    }
    for (int i = 0; i < path_count; i++) {
        int f = 0;
        while (f < loader->file_count && strcmp(loader->files[f].path, paths[i]) != 0) {
            f++;
        }
        if (f == loader->file_count) {
            loader->files[loader->file_count++].path = paths[i];
        }
        NTListing *l = &loader->listings[i];
        l->file      = f;
        l->first     = -1;
        for (int j = 0; loader->keep == 1 && j < i; j++) {
            if (loader->listings[j].file == f && loader->listings[j].first < 0) {
                l->first = j;
                break;
            }
        }
    }
    loader->listing_count = path_count;
    if (!nt_read_all(loader, threads, true) || !nt_read_all(loader, threads, false) ||
        !nt_merge(loader)) {
        return false;
    }
    nt_gather_checks(loader);
    loader->perm = malloc((loader->game_count ? loader->game_count : 1) * sizeof(u32));
    if (!loader->perm) {
        printf("out of memory\n");
        return false;
    }
    return true;
}

typedef struct NTMake {
    NTSet *block;
    u64 sample_at, feature_at;
    u64 end; /* past the game's last slot, which nothing may reach */
    bool over;
} NTMake;

static void nt_make_visit(const Board *b, const DataGame *game, int ply, void *context) {
    NTMake *m = context;
    if (m->sample_at == m->end) {
        m->over = true;
        return;
    }
    NTSample *s = &m->block->samples[m->sample_at++];
    nt_sample(b, game, ply, s, m->block->features + m->feature_at);
    s->first = m->feature_at;
    m->feature_at += s->count;
}

typedef struct NTMakeJob {
    const NTLoader *loader;
    NTSet *block;
    u64 from, to;              /* in the epoch's order of games */
    u64 sample_at, feature_at; /* where its first game's positions go */
    bool ok;
} NTMakeJob;

static void nt_make_job(void *argument) {
    NTMakeJob *job         = argument;
    const NTLoader *loader = job->loader;
    NTMake m = {.block = job->block, .sample_at = job->sample_at, .feature_at = job->feature_at};
    static _Thread_local DataGame game;
    job->ok = true;
    for (u64 j = job->from; j < job->to && job->ok; j++) {
        const NTGame *g = &loader->games[loader->perm[j]];
        const NTFile *f = &loader->files[loader->listings[g->key >> 32].file];
        m.end           = m.sample_at + g->count;
        m.over          = false;
        job->ok         = nt_game_at(g->at, f->bytes + f->size, &game) == NT_GAME &&
                  nt_play(&game, g->key, loader->keep, nt_make_visit, &m) && !m.over &&
                  m.sample_at == m.end;
    }
}

/* Makes the positions of games perm[from, to) on every thread. Each game's go to slots its count
 * fixes, so the block is the same whatever the threads; a game gives at most a slot of stacks a
 * square, and the gaps that leaves among the stacks are never read. */
static bool nt_make_block(NTLoader *loader, u64 from, u64 to, int threads) {
    NTSet *block = &loader->block;
    u64 samples = 0, features = 0;
    for (u64 j = from; j < to; j++) {
        const NTGame *g = &loader->games[loader->perm[j]];
        samples += g->count;
        features += (u64)g->count * g->size * g->size;
    }
    if (samples > block->capacity) {
        free(block->samples);
        block->samples  = malloc(samples * sizeof(NTSample));
        block->capacity = block->samples ? samples : 0;
    }
    if (features > block->feature_capacity) {
        free(block->features);
        block->features         = malloc(features * sizeof(u16));
        block->feature_capacity = block->features ? features : 0;
    }
    if (!block->samples || !block->features) {
        printf("out of memory for a block of %llu positions: give --block fewer\n",
               (unsigned long long)samples);
        return false;
    }
    NTMakeJob jobs[NT_MAX_THREADS];
    u64 j = from, at = 0, stacks = 0;
    for (int t = 0; t < threads; t++) {
        jobs[t]        = (NTMakeJob){loader, block, j, j, at, stacks, true};
        const u64 goal = samples * (u64)(t + 1) / (u64)threads;
        while (j < to && (at < goal || t == threads - 1)) {
            const NTGame *g = &loader->games[loader->perm[j++]];
            at += g->count;
            stacks += (u64)g->count * g->size * g->size;
        }
        jobs[t].to = j;
    }
    run_jobs(jobs, sizeof(NTMakeJob), threads, nt_make_job);
    block->count         = samples;
    block->feature_count = features;
    for (int t = 0; t < threads; t++) {
        if (!jobs[t].ok) {
            printf("a game gave other positions the second time it was read\n");
            return false;
        }
    }
    return true;
}

static u64 ModelRng;

static float model_random(float width) {
    ModelRng = ModelRng * 0xD1342543DE82EF95ULL + 1;
    return ((float)(ModelRng >> 40) / (float)(1 << 24) * 2.0f - 1.0f) * width;
}

static bool model_init(NTModel *model, u64 seed) {
    model->bias_at    = (u64)NT_GRID * NT_GRID * NN_INPUTS * NN_FILTERS;
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
    model->q          = calloc(model->count, sizeof(float));
    model->a          = calloc(model->count, sizeof(float));
    if (!model->p || !model->q || !model->a) {
        return false;
    }
    ModelRng = 0x9E3779B97F4A7C15ULL ^ seed * 0xD1B54A32D192ED03ULL;
    /* The engine's own map from squares to cells, so the two cannot differ. */
    for (int n = MIN_SIZE; n <= MAX_SIZE; n++) {
        for (int g = 0; g < NN_ORIENTATIONS; g++) {
            for (int sq = 0; sq < n * n; sq++) {
                model->image[n][g][sq] = nt_grid_cell(n, nn_cell(n, g, sq));
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

/* One size's network, as the engine is given it, as the fit's weights: its cells onto the grid and
 * its head into its slot. Snapping gives back the same integers. `taken` marks the grid cells an
 * earlier size gave, which must agree, since sizes fitted together share them. */
static bool model_take(NTModel *model, int size, const NNWeights *w, bool *taken) {
    const u64 row = (u64)NN_INPUTS * NN_FILTERS;
    for (int cell = 0; cell < size * size; cell++) {
        const int at = nt_grid_cell(size, cell);
        for (u64 k = 0; k < row; k++) {
            const u64 i  = (u64)at * row + k;
            const long v = w->filters[(u64)cell * row + k];
            if (taken[at] && grid(model, i, model->p[i]) != v) {
                printf("the %dx%d filters differ from another size's where both sit on the grid, "
                       "so the two cannot start one fit\n",
                       size, size);
                return false;
            }
            model->p[i] = (float)((double)v / unit_of(model, i));
            snap(model, i);
        }
        taken[at] = true;
    }
    const struct {
        u64 at, count;
        const void *values;
        bool wide;
    } parts[] = {
        {model->bias_at, NN_FILTERS, w->bias, false},
        {model->output_at, NN_BUCKETS * NN_POOLS, w->output, false},
        {model->skip_at, NN_BUCKETS * 4, w->skip, true},
        {model->l2_at, NN_BUCKETS * NN_L2 * NN_POOLS, w->l2, false},
        {model->l2_skip_at, NN_BUCKETS * NN_L2 * 3, w->l2_skip, false},
        {model->l2_bias_at, NN_BUCKETS * NN_L2, w->l2_bias, true},
        {model->l3_at, NN_BUCKETS * NN_L3 * NN_L2, w->l3, false},
        {model->l3_bias_at, NN_BUCKETS * NN_L3, w->l3_bias, true},
        {model->l4_at, NN_BUCKETS * NN_L3, w->l4, false},
    };
    for (size_t p = 0; p < sizeof(parts) / sizeof(parts[0]); p++) {
        for (u64 j = 0; j < parts[p].count; j++) {
            const u64 i  = parts[p].at + (u64)size * parts[p].count + j;
            const long v = parts[p].wide ? (long)((const i32 *)parts[p].values)[j]
                                         : (long)((const i16 *)parts[p].values)[j];
            model->p[i]  = (float)((double)v / unit_of(model, i));
            snap(model, i);
        }
    }
    return true;
}

/* The networks this build plays for the sizes in `sizes`, as the fit's weights. */
static bool model_warm(NTModel *model, int sizes, bool *taken) {
    for (int size = MIN_SIZE; size <= MAX_SIZE; size++) {
        if (!(sizes >> size & 1)) {
            continue;
        }
        if (!nn_builtin(size)) {
            printf("this build has no %dx%d network to start from\n", size, size);
            return false;
        }
        if (!model_take(model, size, nn_builtin(size), taken)) {
            return false;
        }
    }
    return true;
}

/* A net<size>.h this trainer wrote, as the weights to start from: what model_warm() takes from a
 * network built in, read from a file instead, so any build can carry on any network. A file of
 * another shape holds other counts than this build's. */
static bool model_warm_file(NTModel *model, const char *path, bool *taken) {
    NTFile f = {.path = path};
    if (!nt_read_file(&f)) {
        return false;
    }
    char *text      = (char *)f.bytes;
    text[f.size]    = 0;
    const char *tag = "static const i16 Net";
    const char *at0 = strstr(text, tag);
    int size        = 0;
    if (!at0 || sscanf(at0 + strlen(tag), "%d", &size) != 1 || size < MIN_SIZE || size > MAX_SIZE) {
        printf("%s is not a network this trainer wrote\n", path);
        free(text);
        return false;
    }
    static i16 filters[SQUARE_COUNT * NN_INPUTS * NN_FILTERS];
    static i16 bias[NN_FILTERS], output[NN_BUCKETS * NN_POOLS];
    static i32 skip[NN_BUCKETS * 4];
    static i16 l2[NN_BUCKETS * NN_L2 * NN_POOLS], l2_skip[NN_BUCKETS * NN_L2 * 3];
    static i16 l3[NN_BUCKETS * NN_L3 * NN_L2], l4[NN_BUCKETS * NN_L3];
    static i32 l2_bias[NN_BUCKETS * NN_L2], l3_bias[NN_BUCKETS * NN_L3];
    const struct {
        const char *name;
        void *values;
        u64 count;
        bool wide;
    } arrays[] = {
        {"Filters", filters, (u64)size * size * NN_INPUTS * NN_FILTERS, false},
        {"Bias", bias, NN_FILTERS, false},
        {"Output", output, NN_BUCKETS * NN_POOLS, false},
        {"Skip", skip, NN_BUCKETS * 4, true},
        {"L2", l2, NN_BUCKETS * NN_L2 * NN_POOLS, false},
        {"L2Skip", l2_skip, NN_BUCKETS * NN_L2 * 3, false},
        {"L2Bias", l2_bias, NN_BUCKETS * NN_L2, true},
        {"L3", l3, NN_BUCKETS * NN_L3 * NN_L2, false},
        {"L3Bias", l3_bias, NN_BUCKETS * NN_L3, true},
        {"L4", l4, NN_BUCKETS * NN_L3, false},
    };
    for (size_t a = 0; a < sizeof(arrays) / sizeof(arrays[0]); a++) {
        char name[32];
        snprintf(name, sizeof(name), "Net%d%s[", size, arrays[a].name);
        char *at = strstr(text, name);
        at       = at ? strchr(at, '{') : NULL;
        u64 j    = 0;
        while (at) {
            at += strspn(at + 1, " ,\t\r\n") + 1;
            if (*at == '}') {
                break;
            }
            char *end    = NULL;
            const long v = strtol(at, &end, 10);
            if (end == at || j == arrays[a].count) {
                at = NULL;
                break;
            }
            if (arrays[a].wide) {
                ((i32 *)arrays[a].values)[j++] = (i32)v;
            } else {
                ((i16 *)arrays[a].values)[j++] = (i16)v;
            }
            at = end - 1;
        }
        if (!at || j != arrays[a].count) {
            printf("%s: %.*s holds other than %llu weights\n", path, (int)strlen(name) - 1, name,
                   (unsigned long long)arrays[a].count);
            free(text);
            return false;
        }
    }
    free(text);
    const NNWeights w = {filters, bias, output, skip, l2, l2_skip, l2_bias, l3, l3_bias, l4};
    if (!model_take(model, size, &w, taken)) {
        return false;
    }
    printf("starting %dx%d from %s\n", size, size, path);
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

/* A size's cells of the grid, as the engine keeps them. */
static void local_filters(const NTQuantised *q, int size, i16 *out) {
    const u64 row = (u64)NN_INPUTS * NN_FILTERS;
    for (int cell = 0; cell < size * size; cell++) {
        memcpy(out + (u64)cell * row, q->filters + (u64)nt_grid_cell(size, cell) * row,
               row * sizeof(i16));
    }
}

/* What the engine is given for one size, with `filters` from local_filters(). */
static NNWeights weights_for(const NTQuantised *q, int size, const i16 *filters) {
    return (NNWeights){filters,
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

/* No length is written: the engine checks the one the values give. */
static bool write_array(FILE *out, const char *type, const char *name, const void *values,
                        u64 count, bool wide) {
    fprintf(out, "static const %s %s[] = {\n", type, name);
    for (u64 i = 0; i < count; i++) {
        const long x = wide ? (long)((const i32 *)values)[i] : (long)((const i16 *)values)[i];
        fprintf(out, "%s%ld,%s", i % 24 == 0 ? "    " : "", x,
                i % 24 == 23 || i + 1 == count ? "\n" : "");
    }
    fprintf(out, "};\n\n");
    return !ferror(out);
}

/* `dir`/net<size>.h for each size fitted: its cells of the filters and its own head. A size the
 * games did not have keeps whatever network it has, since it has only the weights it started
 * with. */
static bool write_headers(const char *dir, const NTQuantised *q, int sizes) {
    static i16 filters[SQUARE_COUNT * NN_INPUTS * NN_FILTERS];
    for (int size = MIN_SIZE; size <= MAX_SIZE; size++) {
        if (!(sizes >> size & 1)) {
            continue;
        }
        char path[1024];
        snprintf(path, sizeof(path), "%s/net%d.h", dir, size);
        FILE *out = fopen(path, "wb");
        if (!out) {
            printf("cannot write %s\n", path);
            return false;
        }
        local_filters(q, size, filters);
        const NNWeights w = weights_for(q, size, filters);
        fprintf(out,
                "/* net%d.h - the %dx%d network. Written by `yapper nntrain`: docs/DATA.md says "
                "how. */\n",
                size, size, size);
        fprintf(out,
                "// clang-format off\n#ifndef NET%d_H\n#define NET%d_H\n\n#include \"nn.h\"\n\n",
                size, size);
        const struct {
            const char *type, *name;
            const void *values;
            u64 count;
            bool wide;
        } arrays[] = {
            {"i16", "Filters", w.filters, (u64)size * size * NN_INPUTS * NN_FILTERS, false},
            {"i16", "Bias", w.bias, NN_FILTERS, false},
            {"i16", "Output", w.output, NN_BUCKETS * NN_POOLS, false},
            {"i32", "Skip", w.skip, NN_BUCKETS * 4, true},
            {"i16", "L2", w.l2, NN_BUCKETS * NN_L2 * NN_POOLS, false},
            {"i16", "L2Skip", w.l2_skip, NN_BUCKETS * NN_L2 * 3, false},
            {"i32", "L2Bias", w.l2_bias, NN_BUCKETS * NN_L2, true},
            {"i16", "L3", w.l3, NN_BUCKETS * NN_L3 * NN_L2, false},
            {"i32", "L3Bias", w.l3_bias, NN_BUCKETS * NN_L3, true},
            {"i16", "L4", w.l4, NN_BUCKETS * NN_L3, false},
        };
        bool ok = true;
        for (size_t a = 0; a < sizeof(arrays) / sizeof(arrays[0]); a++) {
            char name[32];
            snprintf(name, sizeof(name), "Net%d%s", size, arrays[a].name);
            ok = ok && write_array(out, arrays[a].type, name, arrays[a].values, arrays[a].count,
                                   arrays[a].wide);
        }
        fprintf(out, "#endif\n");
        if (fclose(out) != 0 || !ok) {
            printf("cannot write %s\n", path);
            return false;
        }
        printf("wrote %s\n", path);
    }
    return true;
}

/* The fixed point the engine runs, through evaluate() itself, against the fit it came from. */
static bool check_engine(const NTModel *model, const NTLoader *loader, const NTQuantised *q,
                         int sizes) {
    static i16 filters[SQUARE_COUNT * NN_INPUTS * NN_FILTERS];
    for (int size = MIN_SIZE; size <= MAX_SIZE; size++) {
        if (!(sizes >> size & 1)) {
            continue;
        }
        local_filters(q, size, filters);
        const NNWeights weights = weights_for(q, size, filters);
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

/* The training games in an epoch's order: as listed when one block holds them all, since its
 * positions are then made once, and otherwise shuffled afresh each epoch. */
static void epoch_games(NTLoader *loader, u64 seed, int epoch) {
    for (u64 g = 0; g < loader->game_count; g++) {
        loader->perm[g] = (u32)g;
    }
    if (loader->positions <= loader->block_size) {
        return;
    }
    u64 state = nt_mix(seed ^ (u64)epoch * 0x9E3779B97F4A7C15ULL);
    for (u64 i = loader->game_count; i > 1; i--) {
        state               = state * 0xD1342543DE82EF95ULL + 1;
        const u64 j         = (state >> 16) % i;
        const u32 t         = loader->perm[i - 1];
        loader->perm[i - 1] = loader->perm[j];
        loader->perm[j]     = t;
    }
}

/* Where the block that starts at perm[from] ends: games while they give no more than a block's
 * positions, and always one. */
static u64 block_end(const NTLoader *loader, u64 from) {
    u64 to = from, positions = 0;
    while (to < loader->game_count) {
        const u64 count = loader->games[loader->perm[to]].count;
        if (to > from && positions + count > loader->block_size) {
            break;
        }
        positions += count;
        to++;
    }
    return to;
}

/* The positions of a block that an epoch sees, by their place in the block, in a new order each
 * epoch, the same on every run; or with no `order`, only how many there are. A position is drawn
 * for --share by its game's key and its place in the game. */
static u64 block_order(const NTLoader *loader, u64 from, u64 to, u64 seed, int epoch, u32 *order,
                       u64 *state) {
    u64 count = 0, base = 0;
    for (u64 j = from; j < to; j++) {
        const NTGame *g    = &loader->games[loader->perm[j]];
        const double share = epoch < loader->join[g->size] ? 0.0 : loader->share[g->size];
        for (u64 k = 0; k < g->count; k++) {
            if (in_epoch(g->key * DATA_MAX_PLIES + k, epoch, seed, share)) {
                if (order) {
                    order[count] = (u32)(base + k);
                }
                count++;
            }
        }
        base += g->count;
    }
    for (u64 i = count; order && i > 1; i--) {
        *state       = *state * 0xD1342543DE82EF95ULL + 1;
        const u64 j  = (*state >> 16) % i;
        const u32 t  = order[i - 1];
        order[i - 1] = order[j];
        order[j]     = t;
    }
    return count;
}

static u64 batches_of(u64 count) { return (count + NT_BATCH - 1) / NT_BATCH; }

static u64 steps_of(NTLoader *loader, int epochs, u64 seed) {
    u64 steps = 0;
    for (int epoch = 1; epoch <= epochs; epoch++) {
        epoch_games(loader, seed, epoch);
        for (u64 from = 0, to; from < loader->game_count; from = to) {
            to = block_end(loader, from);
            steps += batches_of(block_order(loader, from, to, seed, epoch, NULL, NULL));
        }
    }
    return steps;
}

static double rate_at(double first, u64 step, u64 steps) {
    const double progress = (double)step / (double)steps;
    return first * (NT_RATE_END + (1.0 - NT_RATE_END) * 0.5 * (1.0 + cos(NT_PI * progress)));
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
    if (Status.losses < NT_STATUS_EPOCHS) {
        Status.loss[Status.losses][0] = fit;
        Status.loss[Status.losses][1] = held_loss;
        Status.loss[Status.losses][2] = held_result;
        Status.losses++;
    }
    status_write(true, -1);
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
    "        const float p = 1.0f / (1.0f + expf(-out * LOSS_RATIO[s.size]));\n"
    "        const float slope = (p - s.target) * scale * LOSS_RATIO[s.size];\n"
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
    "        double rate, double kept, double seen, double decay, double average) {\n"
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
    "    float w = p[i] - (float)(rate * (m[i] / kept) / (sqrt(v[i] / seen) + 1e-8));\n"
    "    if (decay > 0.0 && i < bias_at) w -= (float)(rate * decay * p[i]);\n"
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
    char ratios[NT_SLOTS * 20] = "";
    for (int n = 0; n < NT_SLOTS; n++) {
        snprintf(ratios + strlen(ratios), sizeof(ratios) - strlen(ratios), "%s%.9g", n ? ", " : "",
                 (double)LossRatio[n]);
    }
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
             "#define L4_LIMIT ((float)%.9g)\n#define HEAD_BIAS_LIMIT ((float)%.9g)\n"
             "__constant__ float LOSS_RATIO[%d] = {%s};\n%s",
             NN_TOKENS, NN_INPUTS, NN_SIDES, NN_FILTERS, NN_ORIENTATIONS, MAX_SIZE * MAX_SIZE,
             SQUARE_COUNT, NN_BUCKETS, (double)NN_QA, (double)NN_QB, (double)NN_SCALE,
             (double)NT_LEAD_UNIT, (double)NT_RESERVE_UNIT, (double)NT_KOMI_UNIT, NT_BETA1,
             NT_BETA2, (double)NT_FILTER_LIMIT, (double)NT_BIAS_LIMIT, (double)NT_OUTPUT_LIMIT,
             NN_POOLS, NN_L2, NN_L3, (double)NN_QU, (double)NN_Q2, (double)NN_Q3, (double)NN_Q4,
             (double)NN_QY, (double)NT_POOL, (double)NT_L2_LIMIT, (double)NT_L3_LIMIT,
             (double)NT_L4_LIMIT, (double)NT_HEAD_BIAS_LIMIT, NT_SLOTS, ratios, KernelBody);
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

/* Block by block, the positions of each made while the card waits; within a block the CPU lays
 * out each group of batches while the card works through the one before it. */
static bool train_gpu(NTModel *model, NTLoader *loader, const double *lambda, int epochs,
                      int threads, u64 seed, u32 *order) {
    const NTSet *block = &loader->block;
    const bool once    = loader->positions <= loader->block_size;
    bool made          = false;
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
    Status.steps = steps;
    status_phase("training");
    for (int epoch = 1; epoch <= epochs; epoch++) {
        Status.epoch = epoch;
        if (!gpu_zero(loss, sizeof(u64))) {
            return false;
        }
        epoch_games(loader, seed, epoch);
        u64 seen_here = 0;
        for (u64 first = 0, last; first < loader->game_count; first = last) {
            last = block_end(loader, first);
            if (!once || !made) {
                status_phase("making positions");
                if (!nt_make_block(loader, first, last, threads)) {
                    return false;
                }
                made = true;
                status_phase("training");
            }
            const u64 total  = block_order(loader, first, last, seed, epoch, order, &shuffle);
            const u64 groups = (total + group - 1) / group;
            seen_here += total;
            if (!total) {
                continue;
            }
            pack_batch(jobs, threads, block, order, 0, group < total ? group : total, lambda, slot,
                       host_packed[0], host_stacks[0]);
            for (u64 g = 0; g < groups; g++) {
                const u64 from   = g * group;
                const u64 to     = from + group < total ? from + group : total;
                const int buffer = (int)(g & 1);
                if (!gpu_upload_async(packed, host_packed[buffer],
                                      (to - from) * sizeof(NTPacked)) ||
                    !gpu_upload_async(stacks, host_stacks[buffer],
                                      (to - from) * slot * sizeof(u16))) {
                    return false;
                }
                for (u64 at = from; at < to; at += NT_BATCH, step++) {
                    const unsigned n       = (unsigned)(at + NT_BATCH < to ? NT_BATCH : to - at);
                    GpuPtr batch_packed    = packed + (at - from) * sizeof(NTPacked);
                    GpuPtr batch_stacks    = stacks + (at - from) * slot * sizeof(u16);
                    float scale            = 1.0f / (float)n;
                    double rate            = rate_at(loader->rate, step, steps);
                    double kept            = 1.0 - pow(NT_BETA1, (double)(step + 1));
                    double seen            = 1.0 - pow(NT_BETA2, (double)(step + 1));
                    double decay           = Decay;
                    double average         = average_at(step, steps);
                    void *step_arguments[] = {
                        &batch_packed, &batch_stacks, &q,       &image,      &grad_filters, &grad,
                        &loss,         &squares,      &bias_at, &output_at,  &skip_at,      &l2_at,
                        &l2_skip_at,   &l2_bias_at,   &l3_at,   &l3_bias_at, &l4_at,        &scale};
                    void *adam_arguments[] = {
                        &p,          &m,     &v,          &q,         &a,       &grad_filters,
                        &grad,       &count, &bias_at,    &output_at, &skip_at, &l2_at,
                        &l2_bias_at, &l3_at, &l3_bias_at, &l4_at,     &rate,    &kept,
                        &seen,       &decay, &average};
                    if (!gpu_launch(step_kernel, n, NN_HIDDEN, step_arguments) ||
                        !gpu_launch(adam_kernel, (unsigned)(count + 255) / 256, 256,
                                    adam_arguments)) {
                        return false;
                    }
                }
                if (g + 1 < groups) {
                    pack_batch(jobs, threads, block, order, to,
                               to + group < total ? to + group : total, lambda, slot,
                               host_packed[buffer ^ 1], host_stacks[buffer ^ 1]);
                }
                if (!gpu_wait()) {
                    return false;
                }
                Status.step = step;
                status_write(false, -1);
            }
        }
        u64 sum = 0;
        if (!gpu_download(&sum, loss, sizeof(u64)) || !gpu_download(model->q, q, floats)) {
            return false;
        }
        report_epoch(model, loader, lambda, threads, epoch,
                     (double)sum / 4294967296.0 / (double)(seen_here ? seen_here : 1));
    }
    return gpu_download(model->p, p, floats) && gpu_download(model->q, q, floats) &&
           gpu_download(model->a, a, floats);
}

/* Scores the network on each group of games: as written, the mean of the fit's last steps, or a
 * warm fit's before its first step. `scored`, if not NULL, takes the loss and the loss against
 * results. */
static void report_groups(const NTModel *model, const NTLoader *loader, const double *lambda,
                          int threads, const char *when, double *scored) {
    double loss, result, by_group[NT_GROUPS];
    score_set(model, &loader->held, lambda, threads, &loss, &result, by_group);
    printf("%s: held out %.6f, against results %.6f\n", when, loss, result);
    if (scored) {
        scored[0] = loss;
        scored[1] = result;
    }
    for (int g = 0; g < loader->group_count; g++) {
        const NTGroup *group = &loader->groups[g];
        printf("  %dx%d gen %d at %u nodes%s: %.6f against results, %llu held out\n", group->size,
               group->size, group->generation, group->nodes,
               loader->groups_full && g == NT_GROUPS - 1 ? " and the rest" : "", by_group[g],
               (unsigned long long)loader->held_counts[g]);
    }
}

static int fit_network(const char *out, int keep, const double *lambda, int epochs, int threads,
                       unsigned long long seed, double rate, bool warm, const char *const *from,
                       int from_count, const double *share, const int *join, double decay,
                       const double *scale, int smooth, double block, char **paths,
                       int path_count) {
    if (keep < 1 || epochs < 1 || threads < 1 || threads > NT_MAX_THREADS || rate < 0.0 ||
        block < 1.0 || block > 4000.0 || decay < 0.0 || decay > 1.0) {
        printf("bad arguments\n");
        return 2;
    }
    /* Before the hours of fitting, not after them. */
    struct stat folder;
    if (stat(out, &folder) != 0 || !S_ISDIR(folder.st_mode)) {
        printf("%s is not a folder: the networks are written there as net<size>.h\n", out);
        return 2;
    }
    if (smooth > 1) {
        Smooth = smooth;
        printf("the search's score is smoothed over %d scored plies\n", smooth);
    }
    Decay = decay;
    if (Decay > 0.0) {
        printf("each step shrinks a filter weight by the rate times %g\n", Decay);
    }
    for (int n = 0; n < NT_SLOTS; n++) {
        LossScale[n] = scale && scale[n] > 0.0 ? (float)scale[n] : (float)NN_SCALE;
        LossRatio[n] = LossScale[n] == (float)NN_SCALE ? 1.0f : (float)NN_SCALE / LossScale[n];
        if (LossScale[n] != (float)NN_SCALE && n >= MIN_SIZE) {
            printf("%dx%d reads a score through a scale of %g, the engine's units %d\n", n, n,
                   (double)LossScale[n], NN_SCALE);
        }
    }
    /* The card first, so a machine without one says so before reading gigabytes. */
    char why[4096] = "";
    if (!gpu_open(kernel_source(), why, sizeof(why))) {
        printf("nntrain fits on an NVIDIA GPU, and found none: %s\n", why);
        return 1;
    }
    snprintf(Status.device, sizeof(Status.device), "%s", gpu_name());

    static NTLoader loader;
    loader.keep       = keep;
    loader.rate       = rate > 0.0 ? rate : NT_RATE;
    loader.block_size = (u64)(block * 1e6);
    memcpy(loader.lambda, lambda, sizeof(loader.lambda));
    loader.check = malloc(sizeof(Board) * NT_CHECK_BOARDS);
    if (!loader.check) {
        printf("out of memory\n");
        return 1;
    }
    for (int n = 0; n < NT_SLOTS; n++) {
        loader.share[n] = share ? share[n] : 1.0;
        loader.join[n]  = join ? join[n] : 0;
        if (loader.join[n] > 1 && n >= MIN_SIZE) {
            printf("the %dx%d positions join at epoch %d\n", n, n, loader.join[n]);
        }
    }
    if (!nt_load(&loader, paths, path_count, threads)) {
        return 1;
    }
    const NTSet *held = &loader.held;
    Status.positions  = loader.positions;
    Status.read_at    = wall_clock();
    printf("%llu positions to fit, %llu held out, %.1f stacks a position\n",
           (unsigned long long)loader.positions, (unsigned long long)held->count,
           loader.positions ? (double)loader.features / (double)loader.positions : 0.0);
    u64 bytes = 0;
    for (int f = 0; f < loader.file_count; f++) {
        bytes += loader.files[f].size;
    }
    const bool once = loader.positions <= loader.block_size;
    printf("%llu games to fit from %d files, %.2f GB in memory, their positions made %s\n",
           (unsigned long long)loader.game_count, loader.file_count, (double)bytes / 1e9,
           once ? "once" : "afresh each epoch, in blocks of games shuffled each epoch");
    if (!loader.positions || !held->count) {
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
    if (from_count || warm) {
        bool taken[NT_GRID * NT_GRID] = {false};
        for (int k = 0; k < from_count; k++) {
            if (!model_warm_file(&model, from[k], taken)) {
                return 1;
            }
        }
        if (!from_count && !model_warm(&model, sizes, taken)) {
            return 1;
        }
        status_phase("scoring the start");
        report_groups(&model, &loader, loader.lambda, threads, "start", NULL);
    }
    const u64 most = once ? loader.positions : loader.block_size + DATA_MAX_PLIES;
    u32 *order     = malloc(most * sizeof(u32));
    if (!order) {
        printf("out of memory\n");
        return 1;
    }

    Status.train_at = wall_clock();
    printf("training on %s\n", gpu_name());
    if (!train_gpu(&model, &loader, loader.lambda, epochs, threads, seed, order)) {
        const char *error = gpu_error();
        if (error && *error) {
            printf("%s\n", error);
        }
        return 1;
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
    Status.step = Status.steps;
    status_phase("scoring");
    report_groups(&model, &loader, loader.lambda, threads, "averaged", Status.averaged);
    Status.scored = true;
    status_phase("checking the engine");
    quantise_model(&model, &q);
    if (!write_headers(out, &q, sizes) || !check_engine(&model, &loader, &q, sizes)) {
        return 1;
    }
    return 0;
}

/* The games listed, each `@file` replaced by the paths it holds, one a line: Windows starts no
 * command longer than 32,767 characters, and a run of thousands of files is longer. */
static char **expand_paths(char **paths, int count, int *expanded) {
    int total = 0, capacity = count + 16;
    char **out = malloc((size_t)capacity * sizeof(char *));
    for (int i = 0; out && i < count; i++) {
        if (paths[i][0] != '@') {
            if (total == capacity) {
                capacity *= 2;
                out = realloc(out, (size_t)capacity * sizeof(char *));
            }
            if (out) {
                out[total++] = paths[i];
            }
            continue;
        }
        NTFile list = {.path = paths[i] + 1};
        if (!nt_read_file(&list)) {
            free(out);
            return NULL;
        }
        list.bytes[list.size] = 0;
        for (char *line = (char *)list.bytes; out && *line;) {
            char *end       = line + strcspn(line, "\r\n");
            const bool more = *end != 0;
            *end            = 0;
            if (*line && *line != '#') {
                if (total == capacity) {
                    capacity *= 2;
                    out = realloc(out, (size_t)capacity * sizeof(char *));
                }
                if (out) {
                    out[total++] = line;
                }
            }
            line = more ? end + 1 : end;
        }
    }
    if (!out) {
        printf("out of memory\n");
        return NULL;
    }
    *expanded = total;
    return out;
}

int nntrain_run(const char *out, int keep, const double *lambda, int epochs, int threads,
                unsigned long long seed, double rate, bool warm, const char *const *from,
                int from_count, const double *share, const int *join, double decay,
                const double *scale, int smooth, double block, const char *status, char **paths,
                int path_count) {
    int count    = 0;
    char **games = expand_paths(paths, path_count, &count);
    if (!games) {
        return 1;
    }
    status_open(status, out, epochs, count);
    const int code =
        fit_network(out, keep, lambda, epochs, threads, seed, rate, warm, from, from_count, share,
                    join, decay, scale, smooth, block, games, count);
    Status.phase = code == 0 ? "done" : "failed";
    status_write(true, code);
    return code;
}
