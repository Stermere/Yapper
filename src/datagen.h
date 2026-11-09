/* datagen.h - self-play games on disk, for training an evaluation. docs/DATA.md is the format,
 * and anything reading these files outside the engine works from that. */
#ifndef DATAGEN_H
#define DATAGEN_H

#include "board.h"
#include "types.h"

#define DATA_MAGIC    0x31474459u /* the bytes "YDG1" */
#define DATA_NO_SCORE 0x7FFF      /* a ply with no search behind it */

/* A game this long has stopped being about anything, and the limit keeps a game's size known. */
#define DATA_MAX_PLIES 512

typedef enum { ENDING_RULES, ENDING_REPETITION, ENDING_PLY_LIMIT } DataEnding;

/* Little-endian on disk, as on every target. Changing any of this means a new magic. */
typedef struct DataHeader {
    u32 magic;
    u8 size;
    i8 halfkomi;
    u8 outcome; /* an Outcome */
    u8 ending;  /* a DataEnding */
    u16 plies;
    u8 random_plies; /* the first plies were chosen at random, and carry no score */
    u8 generation;   /* which round of training produced this game */
    u32 nodes;       /* the search budget per move */
} DataHeader;

typedef struct DataPly {
    Move move;
    i16 score; /* from the side to move's point of view */
} DataPly;

_Static_assert(sizeof(DataHeader) == 16, "the header is the file format");
_Static_assert(sizeof(DataPly) == 4, "a ply is the file format");

typedef struct DataGame {
    DataHeader header;
    DataPly plies[DATA_MAX_PLIES];
} DataGame;

typedef struct DataConfig {
    const char *path;
    u64 games;
    u64 seed;
    u64 nodes;
    int size;
    int generation;
    /* A book of openings, one a line as PTN moves, played in place of the random plies; NULL
     * for random plies. tools/mkopenings.py writes one from Yapper's own games or people's. */
    const char *openings;
} DataConfig;

/* Plays until the file holds this many games, carrying on after any it already holds. The same
 * seed and build give the same file, however often it was stopped and started again. Non-zero
 * if the file cannot be written, ends part way through a game, or holds another run's games. */
int datagen_run(const DataConfig *config);

/* Called for every position of every game, before the move at `ply` is made. `index` counts
 * games within the file. Returning false stops the replay. */
typedef bool (*DataVisitor)(const Board *b, const DataGame *game, int ply, u64 index,
                            void *context);

/* Gives back every position of a file by playing its games. False if the file cannot be read
 * or a game in it is not a game. A file cut short by a killed run just ends. */
bool data_replay(const char *path, DataVisitor visit, void *context);

#endif
