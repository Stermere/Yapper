/* tt.c - a bucketed, depth-and-age replaced transposition table. */
#include "tt.h"

#include <stdlib.h>
#include <string.h>

#define BUCKET_ENTRIES 3

/* One cache line holds two of these, so a probe never touches a second line. */
typedef struct {
    TTEntry entries[BUCKET_ENTRIES];
    u16 padding;
} Bucket;

static Bucket *Table;
static u64 BucketMask;
static u8 Generation;
static int Megabytes;

bool tt_resize(int megabytes) {
    u64 buckets = 1;
    while (buckets * 2 * sizeof(Bucket) <= (u64)megabytes * 1024 * 1024) {
        buckets *= 2;
    }
    Bucket *table = malloc(buckets * sizeof(Bucket));
    if (!table) {
        return false;
    }
    free(Table);
    Table      = table;
    BucketMask = buckets - 1;
    Megabytes  = megabytes;
    tt_clear();
    return true;
}

int tt_size_mb(void) { return Megabytes; }

void tt_clear(void) {
    memset(Table, 0, (BucketMask + 1) * sizeof(Bucket));
    Generation = 0;
}

void tt_new_search(void) { Generation = (Generation + 1) & 63; }

/* The position hash is a sum, weak in its low bits, and this is the one reader of those. */
static inline u64 tt_key(u64 hash) { return (hash ^ hash >> 32) * 0x9E3779B97F4A7C15ULL; }

/* The index comes from the low bits and the check from the high bits, so the two are
 * independent and a table of any size keeps a full 16 bits of verification. */
static inline Bucket *bucket_of(u64 key) { return &Table[key & BucketMask]; }
static inline u16 check_of(u64 key) { return (u16)(key >> 48); }

bool tt_probe(u64 hash, TTEntry *out) {
    const u64 key        = tt_key(hash);
    const Bucket *bucket = bucket_of(key);
    const u16 check      = check_of(key);
    for (int i = 0; i < BUCKET_ENTRIES; i++) {
        const TTEntry *e = &bucket->entries[i];
        if (e->key == check && (e->gen_bound & 3) != BOUND_NONE) {
            *out = *e;
            return true;
        }
    }
    return false;
}

void tt_store(u64 hash, Move move, int score, int eval, int depth, Bound bound) {
    const u64 key   = tt_key(hash);
    Bucket *bucket  = bucket_of(key);
    const u16 check = check_of(key);

    TTEntry *slot = &bucket->entries[0];
    int worst     = 1 << 30;
    for (int i = 0; i < BUCKET_ENTRIES; i++) {
        TTEntry *e = &bucket->entries[i];
        if (e->key == check || (e->gen_bound & 3) == BOUND_NONE) {
            slot = e;
            break;
        }
        const int age   = (Generation - (e->gen_bound >> 2)) & 63;
        const int value = e->depth - age * 8;
        if (value < worst) {
            worst = value;
            slot  = e;
        }
    }

    if (bound != BOUND_EXACT && slot->key == check && slot->depth > depth + 3 &&
        (slot->gen_bound >> 2) == Generation) {
        return;
    }
    /* An entry that has no move of its own keeps the one it had. */
    if (move != MOVE_NONE || slot->key != check) {
        slot->move = move;
    }
    slot->key       = check;
    slot->score     = (i16)score;
    slot->eval      = (i16)eval;
    slot->depth     = (u8)depth;
    slot->gen_bound = (u8)(Generation << 2 | bound);
}

int tt_hashfull(void) {
    int used         = 0;
    const int sample = BucketMask + 1 < 1000 ? (int)(BucketMask + 1) : 1000;
    for (int i = 0; i < sample; i++) {
        for (int j = 0; j < BUCKET_ENTRIES; j++) {
            const TTEntry *e = &Table[i].entries[j];
            used += (e->gen_bound & 3) != BOUND_NONE && (e->gen_bound >> 2) == Generation;
        }
    }
    return used * 1000 / (sample * BUCKET_ENTRIES);
}
