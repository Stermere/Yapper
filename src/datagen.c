/* datagen.c - self-play games for training, as small on disk as they can be.
 *
 * A game is kept as its moves and the search's score of each, never as positions or features.
 * Replaying it gives back every position, so the data outlives any evaluation trained on it,
 * and a ply costs four bytes. The search is the one that plays: nothing here depends on what
 * kind of evaluation is behind it. */
#include "datagen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bitboard.h"
#include "movegen.h"
#include "search.h"
#include "tt.h"

/* A game's search fits in this, and clearing it costs under 1% of the game. */
#define DATA_HASH_MB 16

/* Enough random plies that games differ, few enough that they still look like Tak. */
#define OPENING_MIN_PLIES 4
#define OPENING_MAX_PLIES 9

/* An opening the search already calls decided teaches nothing. */
#define OPENING_CHECK_NODES 2000
#define OPENING_SCORE_LIMIT 500

/* A budget stops between iterations, so a score comes from a whole search. The next iteration
 * can be far larger, and this is how much of one is allowed before it is cut off. */
#define HARD_NODES_FACTOR 4

#define REPORT_EVERY 2000

/* Mostly what matches are played at, with enough else that komi can be fitted. */
static const i8 HalfKomiDraws[] = {4, 4, 4, 4, 4, 4, 4, 4, 4, 3, 5, 2, 6, 0};

static u64 DataRngState;

/* xorshift64*. The seed goes through a multiply so that seeds 1, 2, 3 start far apart. */
static u64 data_rng(void) {
    DataRngState ^= DataRngState >> 12;
    DataRngState ^= DataRngState << 25;
    DataRngState ^= DataRngState >> 27;
    return DataRngState * 0x2545F4914F6CDD1DULL;
}

/* A flat, wall or capstone placement by its StoneType, and a spread after those. */
static inline int move_class(Move m) { return move_is_place(m) ? (int)move_stone(m) : 3; }

/* Uniform over the moves would be two thirds walls and capstones. Choosing the kind first keeps
 * the openings playable while still showing capstones at every point of the opening. */
static Move random_move(const Board *b) {
    static Move list[MAX_MOVES];
    const int count = movegen_all(b, list);
    const u64 roll  = data_rng() % 100;
    const int want  = roll < 70 ? FLAT : roll < 85 ? CAP : roll < 90 ? WALL : 3;

    int matching = 0;
    for (int i = 0; i < count; i++) {
        matching += move_class(list[i]) == want;
    }
    if (matching == 0) {
        return list[data_rng() % (u64)count];
    }
    int target = (int)(data_rng() % (u64)matching);
    for (int i = 0; i < count; i++) {
        if (move_class(list[i]) == want && target-- == 0) {
            return list[i];
        }
    }
    return list[0];
}

/* Match runners draw a threefold, so these games must too. */
static bool third_repetition(const Board *b, const u64 *keys, int count) {
    int seen         = 0;
    const int oldest = count - b->reversible;
    for (int i = count - 2; i >= 0 && i >= oldest; i -= 2) {
        seen += keys[i] == b->hash;
    }
    return seen >= 2;
}

/* False if the opening was rejected, which leaves `game` unused. */
static bool play_game(DataGame *game, const DataConfig *config) {
    static u64 keys[DATA_MAX_PLIES + 1];
    DataHeader *header = &game->header;
    int plies          = 0;
    Board board;
    Undo undo;

    const size_t draws = sizeof(HalfKomiDraws) / sizeof(HalfKomiDraws[0]);
    const int halfkomi = HalfKomiDraws[data_rng() % draws];
    board_clear(&board, config->size, halfkomi);
    search_clear();

    const int random_plies =
        OPENING_MIN_PLIES + (int)(data_rng() % (OPENING_MAX_PLIES - OPENING_MIN_PLIES + 1));
    for (; plies < random_plies; plies++) {
        const Move m       = random_move(&board);
        game->plies[plies] = (DataPly){m, DATA_NO_SCORE};
        keys[plies]        = board.hash;
        board_make(&board, m, &undo);
        if (board_outcome_after(&board, &undo) != ONGOING) {
            return false;
        }
    }

    SearchLimits limits;
    memset(&limits, 0, sizeof(limits));
    limits.soft_nodes = OPENING_CHECK_NODES;
    limits.nodes      = OPENING_CHECK_NODES * HARD_NODES_FACTOR;
    if (abs(search_run(&board, keys, plies, &limits, false).score) >= OPENING_SCORE_LIMIT) {
        return false;
    }

    limits.soft_nodes = config->nodes;
    limits.nodes      = config->nodes * HARD_NODES_FACTOR;
    Outcome outcome   = ONGOING;
    int ending        = ENDING_RULES;
    while (outcome == ONGOING) {
        const SearchReport report = search_run(&board, keys, plies, &limits, false);
        game->plies[plies]        = (DataPly){report.best, (i16)report.score};
        keys[plies]               = board.hash;
        plies++;
        board_make(&board, report.best, &undo);
        outcome = board_outcome_after(&board, &undo);
        if (outcome != ONGOING) {
            break;
        }
        if (third_repetition(&board, keys, plies)) {
            outcome = DRAW;
            ending  = ENDING_REPETITION;
        } else if (plies == DATA_MAX_PLIES) {
            outcome = DRAW;
            ending  = ENDING_PLY_LIMIT;
        }
    }

    *header = (DataHeader){.magic        = DATA_MAGIC,
                           .size         = (u8)config->size,
                           .halfkomi     = (i8)halfkomi,
                           .outcome      = (u8)outcome,
                           .ending       = (u8)ending,
                           .plies        = (u16)plies,
                           .random_plies = (u8)random_plies,
                           .generation   = (u8)config->generation,
                           .nodes        = (u32)config->nodes};
    return true;
}

int datagen_run(const DataConfig *config) {
    FILE *out = fopen(config->path, "ab");
    if (!out) {
        printf("cannot write %s\n", config->path);
        return 1;
    }
    static char buffer[1 << 20];
    setvbuf(out, buffer, _IOFBF, sizeof(buffer));

    DataRngState = (config->seed + 1) * 0x9E3779B97F4A7C15ULL;
    tt_resize(DATA_HASH_MB);

    static DataGame game;
    struct timespec start, now;
    timespec_get(&start, TIME_UTC);
    u64 played = 0, positions = 0, rejected = 0;
    bool written = true;

    while (played < config->games && written) {
        if (!play_game(&game, config)) {
            rejected++;
            continue;
        }
        const size_t plies = game.header.plies;
        written            = fwrite(&game.header, sizeof(DataHeader), 1, out) == 1 &&
                  fwrite(game.plies, sizeof(DataPly), plies, out) == plies;
        played++;
        positions += plies;
        if (played % REPORT_EVERY == 0 || played == config->games) {
            timespec_get(&now, TIME_UTC);
            const double seconds =
                (double)(now.tv_sec - start.tv_sec) + (double)(now.tv_nsec - start.tv_nsec) / 1e9;
            printf("%llu games %llu positions %llu rejected %.1f seconds\n",
                   (unsigned long long)played, (unsigned long long)positions,
                   (unsigned long long)rejected, seconds);
            fflush(stdout);
        }
    }
    if (fclose(out) != 0 || !written) {
        printf("cannot write %s\n", config->path);
        return 1;
    }
    return 0;
}

static bool data_read(FILE *in, DataGame *game) {
    if (fread(&game->header, sizeof(DataHeader), 1, in) != 1) {
        return false;
    }
    const size_t plies = game->header.plies;
    return plies <= DATA_MAX_PLIES && fread(game->plies, sizeof(DataPly), plies, in) == plies;
}

bool data_replay(const char *path, DataVisitor visit, void *context) {
    FILE *in = fopen(path, "rb");
    if (!in) {
        printf("cannot read %s\n", path);
        return false;
    }
    static char buffer[1 << 20];
    setvbuf(in, buffer, _IOFBF, sizeof(buffer));

    static DataGame game;
    u64 index   = 0;
    bool intact = true;
    while (intact && data_read(in, &game)) {
        const DataHeader *header = &game.header;
        if (header->magic != DATA_MAGIC || header->size < MIN_SIZE || header->size > MAX_SIZE) {
            printf("%s: game %llu has a bad header\n", path, (unsigned long long)index);
            intact = false;
            break;
        }
        Board board;
        Undo undo;
        board_clear(&board, header->size, header->halfkomi);
        for (int ply = 0; ply < header->plies; ply++) {
            const Move m = game.plies[ply].move;
            if (!movegen_is_legal(&board, m)) {
                printf("%s: game %llu has an illegal move at ply %d\n", path,
                       (unsigned long long)index, ply);
                intact = false;
                break;
            }
            if (!visit(&board, &game, ply, index, context)) {
                fclose(in);
                return true;
            }
            board_make(&board, m, &undo);
        }
        index++;
    }
    fclose(in);
    return intact;
}
