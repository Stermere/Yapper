/* main.c - command line entry point. With no arguments, or `tei`, it is the engine. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bench.h"
#include "board.h"
#include "datagen.h"
#include "movegen.h"
#include "notation.h"
#include "perft.h"
#include "search.h"
#include "tei.h"
#include "test/selftest.h"
#include "tune.h"

static double now_seconds(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* yapper perft <size> <depth> [position] */
static int cmd_perft(int argc, char **argv) {
    if (argc < 4) {
        printf("usage: yapper perft <size> <depth> [position]\n");
        return 2;
    }
    const int size      = atoi(argv[2]);
    const int depth     = atoi(argv[3]);
    char position[4096] = "startpos";
    if (argc > 4) {
        position[0] = '\0';
        for (int i = 4; i < argc; i++) {
            strncat(position, argv[i], sizeof(position) - strlen(position) - 2);
            strcat(position, " ");
        }
    }
    Board board;
    if (!position_parse(&board, position, size, 0, NULL, 0, NULL)) {
        printf("bad position: %s\n", position);
        return 2;
    }
    for (int bulk = 0; bulk < 2; bulk++) {
        for (int d = 1; d <= depth; d++) {
            const PerftOptions options     = {.bulk = bulk, .ended_is_leaf = false};
            const double start             = now_seconds();
            const unsigned long long nodes = perft(&board, d, options);
            const double elapsed           = now_seconds() - start;
            printf("%s depth %d: %llu  %.3fs  %.1f Mnps\n", bulk ? "bulk" : "full", d, nodes,
                   elapsed, elapsed > 0 ? (double)nodes / elapsed / 1e6 : 0.0);
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    board_init();
    movegen_init();
    search_init();

    const char *command = argc >= 2 ? argv[1] : "tei";
    if (strcmp(command, "tei") == 0) {
        tei_loop();
        return 0;
    }
    if (strcmp(command, "perft") == 0) {
        return cmd_perft(argc, argv);
    }
    if (strcmp(command, "perfttest") == 0 && argc >= 3) {
        return perft_run_suite(argv[2], argc >= 4 ? atoi(argv[3]) : 99) ? 1 : 0;
    }
    if (strcmp(command, "selftest") == 0) {
        return selftest_run() ? 1 : 0;
    }
    if (strcmp(command, "bench") == 0) {
        bench_run(argc >= 3 ? atoi(argv[2]) : BENCH_DEFAULT_DEPTH);
        return 0;
    }
    if (strcmp(command, "datagen") == 0 && argc >= 6) {
        const DataConfig config = {.path       = argv[2],
                                   .games      = strtoull(argv[3], NULL, 10),
                                   .seed       = strtoull(argv[4], NULL, 10),
                                   .nodes      = strtoull(argv[5], NULL, 10),
                                   .size       = argc > 6 ? atoi(argv[6]) : 6,
                                   .generation = argc > 7 ? atoi(argv[7]) : 0};
        return datagen_run(&config);
    }
    if (strcmp(command, "tune") == 0 && argc >= 6) {
        return tune_run(argv[2], atoi(argv[3]), atof(argv[4]), argv + 5, argc - 5);
    }
    printf(
        "usage: yapper [tei | bench [depth] | selftest | perfttest <suite> [max depth] |\n"
        "               perft <size> <depth> [position] |\n"
        "               datagen <out> <games> <seed> <nodes> [size] [generation] |\n"
        "               tune <weights.h> <keep one in> <scale, or 0 to report it> <games>...]\n");
    return 2;
}
