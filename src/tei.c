/* tei.c - the protocol loop. There is no written TEI specification: this speaks what
 * racetrack and PTN Ninja send, see docs/TEI.md. */
#include "tei.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench.h"
#include "board.h"
#include "eval.h"
#include "movegen.h"
#include "notation.h"
#include "perft.h"
#include "search.h"
#include "tt.h"

#define DEFAULT_SIZE 6
#define MIN_HASH     1
#define MAX_HASH     65536

/* What racetrack's --komi can express. */
#define MIN_HALFKOMI (-10)
#define MAX_HALFKOMI 10

/* `position` lists every move of the game: a long game on a big board, at 12 bytes a
 * move, against the longest game the search keeps keys for. */
#define LINE_BUFFER (MAX_GAME_PLIES * 12)

static Board Root;
static u64 RootHistory[MAX_GAME_PLIES];
static int RootHistoryCount;
static int BoardSize = DEFAULT_SIZE;
static int HalfKomi;

static void send_identity(void) {
    printf("id name Yapper\n");
    printf("id author Collin Kees\n");
    printf("option name HalfKomi type spin default 0 min %d max %d\n", MIN_HALFKOMI, MAX_HALFKOMI);
    printf("option name Hash type spin default %d min %d max %d\n", TT_DEFAULT_MB, MIN_HASH,
           MAX_HASH);
    printf("teiok\n");
}

static void reset_root(void) {
    board_clear(&Root, BoardSize, HalfKomi);
    RootHistoryCount = 0;
}

static void set_halfkomi(int value) {
    if (value < MIN_HALFKOMI || value > MAX_HALFKOMI) {
        printf("info string HalfKomi %d is outside %d..%d\n", value, MIN_HALFKOMI, MAX_HALFKOMI);
        return;
    }
    HalfKomi      = value;
    Root.halfkomi = value;
}

/* Both spellings exist: `teinewgame 6`, and `teinewgame size 6 halfkomi 4` from PTN Ninja
 * when an engine declares TEI 1. */
static void handle_newgame(char *args) {
    int size    = BoardSize;
    char *token = strtok(args, " \t\r\n");
    while (token) {
        if (strcmp(token, "size") == 0) {
            token = strtok(NULL, " \t\r\n");
            size  = token ? atoi(token) : size;
        } else if (strcmp(token, "halfkomi") == 0) {
            token = strtok(NULL, " \t\r\n");
            if (token) {
                set_halfkomi(atoi(token));
            }
        } else {
            size = atoi(token);
        }
        token = token ? strtok(NULL, " \t\r\n") : NULL;
    }
    if (size < MIN_SIZE || size > MAX_SIZE) {
        printf("info string unsupported size %d\n", size);
        return;
    }
    BoardSize = size;
    search_clear();
    reset_root();
}

static void handle_position(const char *args) {
    if (!position_parse(&Root, args, BoardSize, HalfKomi, RootHistory, MAX_GAME_PLIES,
                        &RootHistoryCount)) {
        printf("info string invalid position: %s\n", args);
        reset_root();
    }
}

static void handle_setoption(char *args) {
    /* setoption name <name> value <value> */
    char *name  = strstr(args, "name");
    char *value = strstr(args, "value");
    if (!name || !value) {
        printf("info string malformed setoption\n");
        return;
    }
    name += 4;
    while (isspace((unsigned char)*name)) {
        name++;
    }
    const size_t length = strcspn(name, " \t\r\n");
    const int number    = atoi(value + 5);

    if (strncmp(name, "HalfKomi", length) == 0 && length == 8) {
        set_halfkomi(number);
        search_clear();
    } else if (strncmp(name, "Hash", length) == 0 && length == 4) {
        if (number < MIN_HASH || number > MAX_HASH || !tt_resize(number)) {
            printf("info string cannot set Hash to %d\n", number);
        }
    } else {
        printf("info string unknown option %.*s\n", (int)length, name);
    }
}

static void handle_go(char *args) {
    SearchLimits limits;
    memset(&limits, 0, sizeof(limits));
    char *token = strtok(args, " \t\r\n");
    while (token) {
        char *value = NULL;
        if (strcmp(token, "infinite") == 0) {
            limits.infinite = true;
        } else if ((value = strtok(NULL, " \t\r\n")) != NULL) {
            const long long number = atoll(value);
            if (strcmp(token, "depth") == 0) {
                limits.depth = (int)number;
            } else if (strcmp(token, "nodes") == 0) {
                limits.nodes = (u64)number;
            } else if (strcmp(token, "movetime") == 0) {
                limits.movetime = number;
            } else if (strcmp(token, "wtime") == 0) {
                limits.clock       = true;
                limits.time[WHITE] = number;
            } else if (strcmp(token, "btime") == 0) {
                limits.clock       = true;
                limits.time[BLACK] = number;
            } else if (strcmp(token, "winc") == 0) {
                limits.inc[WHITE] = number;
            } else if (strcmp(token, "binc") == 0) {
                limits.inc[BLACK] = number;
            }
        }
        token = strtok(NULL, " \t\r\n");
    }
    search_start(&Root, RootHistory, RootHistoryCount, &limits);
}

/* Not TEI: what the tools and a person at a terminal use. */
static bool handle_debug(const char *command, char *args) {
    if (strcmp(command, "print") == 0) {
        char tps[TPS_BUFFER];
        board_to_tps(&Root, tps, sizeof(tps));
        const int outcome = board_outcome(&Root);
        printf("tps %s\nhash %016llx\noutcome %d\neval %d\n", tps, (unsigned long long)Root.hash,
               outcome,
               evaluate(&Root, Root.ply >= 2 ? board_road_placements(&Root, !Root.stm) : 0));
    } else if (strcmp(command, "moves") == 0) {
        Move list[MAX_MOVES];
        const int count = board_outcome(&Root) == ONGOING ? movegen_all(&Root, list) : 0;
        printf("moves");
        for (int i = 0; i < count; i++) {
            char text[MOVE_BUFFER];
            printf(" %s", move_format(list[i], text));
        }
        printf("\n");
    } else if (strcmp(command, "perft") == 0) {
        const PerftOptions options = {.bulk = true, .ended_is_leaf = false};
        printf("perft %llu\n", (unsigned long long)perft(&Root, atoi(args), options));
    } else if (strcmp(command, "divide") == 0) {
        perft_divide(&Root, atoi(args));
    } else if (strcmp(command, "bench") == 0) {
        bench_run(BENCH_DEFAULT_DEPTH);
    } else {
        return false;
    }
    return true;
}

void tei_loop(void) {
    static char line[LINE_BUFFER];
    reset_root();

    while (fgets(line, sizeof(line), stdin)) {
        char *cursor = line;
        while (isspace((unsigned char)*cursor)) {
            cursor++;
        }
        char *end           = cursor + strcspn(cursor, " \t\r\n");
        char *args          = *end ? end + 1 : end;
        *end                = '\0';
        const char *command = cursor;
        if (*command == '\0') {
            continue;
        }

        /* Answered in the middle of a search, on this thread: that is why the search has
         * its own. */
        if (strcmp(command, "isready") == 0) {
            printf("readyok\n");
        } else if (strcmp(command, "stop") == 0) {
            search_stop();
        } else if (strcmp(command, "quit") == 0) {
            search_stop();
            search_wait();
            return;
        } else {
            /* Anything else changes what the search is reading, or must not race it. */
            search_wait();
            if (strcmp(command, "tei") == 0) {
                send_identity();
            } else if (strcmp(command, "teinewgame") == 0) {
                handle_newgame(args);
            } else if (strcmp(command, "position") == 0) {
                handle_position(args);
            } else if (strcmp(command, "setoption") == 0) {
                handle_setoption(args);
            } else if (strcmp(command, "go") == 0) {
                handle_go(args);
            } else if (!handle_debug(command, args)) {
                printf("info string unknown command %s\n", command);
            }
        }
        fflush(stdout);
    }
    search_stop();
    search_wait();
}
