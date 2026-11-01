/* bench.c - the deterministic node-count benchmark. */
#include "bench.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "notation.h"
#include "search.h"
#include "tt.h"

typedef struct {
    int size;
    int halfkomi;
    const char *position;
} BenchPosition;

/* 6x6 with 2 komi is where the strongest engines meet, so it gets most of the weight.
 * Frozen: see BENCH_DEFAULT_DEPTH. */
static const BenchPosition BenchPositions[] = {
    {6, 4, "startpos moves a6 f1"},
    {6, 4, "startpos moves c5 b1 e1 d5"},
    {6, 4,
     "tps 2,2,21S,2,2,2/2,x,222221,2,2,x/1,1,2221C,x,111112C,2S/x,1,2S,x2,121211212/"
     "1,1,1212S,1S,2,1S/x2,2,1,21,1 1 42"},
    {6, 4, "tps x4,2,1/2,x2,21S,12,1/2,1,1,2,1,2/1221C,1,2,112C,1,1/2,1,2,1,2,1/2,x,2,2,1,x 1 22"},
    {6, 4,
     "tps x,1,x4/2S,1,x,2,x2/2,121C,212S,x,2C,x/1,211121S,1S,112,11112,x/2221,x,2,1,2,1S/"
     "2222221,x,2,2,2,2 2 43"},
    {6, 4,
     "tps x3,2,1,1/x3,21,1,21/x,2,121S,2,2,11/1,1,1,2,2,21/x2,2221C,12C,2,2/2,2,21,x,2,x 2 25"},
    {6, 4, "tps 2,x4,1/x,2,x,2,2S,1/x,1,2,1112C,1,1/1,2,x,2221C,x,1/2,1,1,x,12,2/x4,1,x 2 18"},
    {6, 4,
     "tps 2,2,221S,121S,x,12/x,2S,2,x,12S,12S/1,2,112C,22121C,1,1/x2,2,1,1,x/1,1,12,x,1,x/"
     "x2,2,x,1,x 1 28"},
    {5, 0, "startpos moves a5 e1"},
    {5, 0, "startpos moves d3 c3 c4 1d3< 1c4- Sc4"},
    {5, 0, "startpos moves c2 c3 d3 b3 c4 1c2+ 1d3< 1b3> 1c4- Cc2 a1 1c2+ a2"},
    {7, 4, "startpos moves a7 g1"},
    {4, 0, "startpos moves a4 d1"},
};

void bench_run(int depth) {
    u64 nodes = 0;
    struct timespec start, end;
    const int hash_mb = tt_size_mb();
    tt_resize(BENCH_HASH_MB);
    timespec_get(&start, TIME_UTC);

    /* Determinism is the point: each position starts from an empty table and history. */
    const int count = (int)(sizeof(BenchPositions) / sizeof(BenchPositions[0]));
    for (int i = 0; i < count; i++) {
        const BenchPosition *p = &BenchPositions[i];
        Board board;
        u64 history[MAX_GAME_PLIES];
        int history_count = 0;
        if (!position_parse(&board, p->position, p->size, p->halfkomi, history, MAX_GAME_PLIES,
                            &history_count)) {
            printf("bench: bad position %s\n", p->position);
            return;
        }
        SearchLimits limits;
        memset(&limits, 0, sizeof(limits));
        limits.depth = depth;
        search_clear();
        const SearchReport report = search_run(&board, history, history_count, &limits, false);
        nodes += report.nodes;

        char text[MOVE_BUFFER];
        printf("%2d/%d: %10llu nodes  depth %2d  score %6d  %s\n", i + 1, count,
               (unsigned long long)report.nodes, report.depth, report.score,
               move_format(report.best, text));
    }

    timespec_get(&end, TIME_UTC);
    const double elapsed =
        (double)(end.tv_sec - start.tv_sec) + (double)(end.tv_nsec - start.tv_nsec) / 1e9;
    printf("%llu nodes %llu nps\n", (unsigned long long)nodes,
           (unsigned long long)((double)nodes / (elapsed > 0 ? elapsed : 1e-9)));
    fflush(stdout);
    tt_resize(hash_mb);
    search_clear();
}
