/* main.c - command line entry point. With no arguments, or `tei`, it is the engine. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bench.h"
#include "board.h"
#include "movegen.h"
#include "nn.h"
#include "notation.h"
#include "perft.h"
#include "search.h"
#include "tei.h"

/* A release build is the engine alone: what writes data, fits weights or tests the board is
 * left out. It plays the same as the full build, and bench is how that is checked. */
#ifndef YAPPER_RELEASE
#include "datagen.h"
#include "nntrain.h"
#include "policyfit.h"
#include "test/selftest.h"
#endif

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
    nn_init();
    search_init();

    const char *command = argc >= 2 ? argv[1] : "tei";
    if (strcmp(command, "tei") == 0) {
        tei_loop();
        return 0;
    }
    if (strcmp(command, "perft") == 0) {
        return cmd_perft(argc, argv);
    }
    if (strcmp(command, "bench") == 0) {
        bench_run(argc >= 3 ? atoi(argv[2]) : BENCH_DEFAULT_DEPTH);
        return 0;
    }
#ifndef YAPPER_RELEASE
    if (strcmp(command, "perfttest") == 0 && argc >= 3) {
        return perft_run_suite(argv[2], argc >= 4 ? atoi(argv[3]) : 99) ? 1 : 0;
    }
    if (strcmp(command, "selftest") == 0) {
        return selftest_run() ? 1 : 0;
    }
    if (strcmp(command, "datagen") == 0 && argc >= 6) {
        const DataConfig config = {.path       = argv[2],
                                   .games      = strtoull(argv[3], NULL, 10),
                                   .seed       = strtoull(argv[4], NULL, 10),
                                   .nodes      = strtoull(argv[5], NULL, 10),
                                   .size       = argc > 6 ? atoi(argv[6]) : 6,
                                   .generation = argc > 7 ? atoi(argv[7]) : 0,
                                   .openings   = argc > 8 ? argv[8] : NULL};
        return datagen_run(&config);
    }
    if (strcmp(command, "nntrain") == 0 && argc >= 8) {
        bool cpu = false, warm = false;
        int smooth                 = 0;
        unsigned long long seed    = 0;
        double rate                = 0.0;
        const char *status         = NULL;
        double scale[MAX_SIZE + 1] = {0};
        double share[MAX_SIZE + 1] = {1, 1, 1, 1, 1, 1, 1, 1, 1};
        double lambda[MAX_SIZE + 1];
        for (int size = 0; size <= MAX_SIZE; size++) {
            lambda[size] = atof(argv[4]);
        }
        int first = 7;
        for (; first < argc; first++) {
            int size;
            double part;
            if (strcmp(argv[first], "--cpu") == 0) {
                cpu = true;
            } else if (strcmp(argv[first], "--warm") == 0) {
                warm = true;
            } else if (strcmp(argv[first], "--seed") == 0 && first + 1 < argc) {
                seed = strtoull(argv[++first], NULL, 10);
            } else if (strcmp(argv[first], "--status") == 0 && first + 1 < argc) {
                status = argv[++first];
            } else if (strcmp(argv[first], "--rate") == 0 && first + 1 < argc &&
                       atof(argv[first + 1]) > 0.0) {
                rate = atof(argv[++first]);
            } else if (strcmp(argv[first], "--scale") == 0 && first + 1 < argc &&
                       sscanf(argv[first + 1], "%d=%lf", &size, &part) == 2 && size >= MIN_SIZE &&
                       size <= MAX_SIZE && part > 0.0) {
                scale[size] = part;
                first++;
            } else if (strcmp(argv[first], "--scale") == 0 && first + 1 < argc &&
                       atof(argv[first + 1]) > 0.0) {
                for (int n = 0; n <= MAX_SIZE; n++) {
                    scale[n] = atof(argv[first + 1]);
                }
                first++;
            } else if (strcmp(argv[first], "--smooth") == 0 && first + 1 < argc &&
                       atoi(argv[first + 1]) > 0) {
                smooth = atoi(argv[++first]);
            } else if (strcmp(argv[first], "--share") == 0 && first + 1 < argc &&
                       sscanf(argv[first + 1], "%d=%lf", &size, &part) == 2 && size >= MIN_SIZE &&
                       size <= MAX_SIZE && part > 0.0) {
                share[size] = part;
                first++;
            } else if (strcmp(argv[first], "--lambda") == 0 && first + 1 < argc &&
                       sscanf(argv[first + 1], "%d=%lf", &size, &part) == 2 && size >= MIN_SIZE &&
                       size <= MAX_SIZE && part >= 0.0 && part <= 1.0) {
                lambda[size] = part;
                first++;
            } else {
                break;
            }
        }
        return nntrain_run(argv[2], atoi(argv[3]), lambda, atoi(argv[5]), atoi(argv[6]), !cpu, seed,
                           rate, warm, share, scale, smooth, status, argv + first, argc - first);
    }
    if (strcmp(command, "policyfit") == 0 && argc >= 7) {
        unsigned long long seed = 0;
        double rate             = 0.0;
        int first               = 6;
        for (; first < argc; first++) {
            if (strcmp(argv[first], "--seed") == 0 && first + 1 < argc) {
                seed = strtoull(argv[++first], NULL, 10);
            } else if (strcmp(argv[first], "--rate") == 0 && first + 1 < argc &&
                       atof(argv[first + 1]) > 0.0) {
                rate = atof(argv[++first]);
            } else {
                break;
            }
        }
        return policyfit_run(argv[2], atoi(argv[3]), atoi(argv[4]), atoi(argv[5]), rate, seed,
                             argv + first, argc - first);
    }
    printf(
        "usage: yapper [tei | bench [depth] | selftest | perfttest <suite> [max depth] |\n"
        "               perft <size> <depth> [position] |\n"
        "               datagen <out> <games> <seed> <nodes> [size] [generation] [openings] |\n"
        "               nntrain <net.h> <keep one in> <lambda> <epochs> <threads> [--cpu]\n"
        "                       [--seed n] [--rate r] [--warm] [--share size=part]...\n"
        "                       [--lambda size=lambda]... [--scale [size=]s]... [--status file]\n"
        "                       [--smooth n] <games>...] |\n"
        "               policyfit <policy_net.h> <keep one in> <epochs> <threads>\n"
        "                         [--seed n] [--rate r] <games>...]\n");
#else
    printf("usage: yapper [tei | bench [depth] | perft <size> <depth> [position]]\n");
#endif
    return 2;
}
