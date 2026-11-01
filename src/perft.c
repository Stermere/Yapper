/* perft.c - exact tree counts. */
#include "perft.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "movegen.h"
#include "notation.h"

static u64 perft_node(Board *b, int depth, PerftOptions options) {
    if (options.bulk && depth == 1) {
        return (u64)movegen_count(b);
    }

    Move list[MAX_MOVES];
    const int count = movegen_all(b, list);
    u64 nodes       = 0;
    for (int i = 0; i < count; i++) {
        Undo undo;
        board_make(b, list[i], &undo);
        if (depth == 1) {
            nodes++;
        } else if (board_outcome_after(b, &undo) != ONGOING) {
            nodes += options.ended_is_leaf;
        } else {
            nodes += perft_node(b, depth - 1, options);
        }
        board_unmake(b, list[i], &undo);
    }
    return nodes;
}

u64 perft(Board *b, int depth, PerftOptions options) {
    if (depth == 0) {
        return 1;
    }
    if (board_outcome(b) != ONGOING) {
        return options.ended_is_leaf;
    }
    return perft_node(b, depth, options);
}

void perft_divide(Board *b, int depth) {
    const PerftOptions options = {.bulk = true, .ended_is_leaf = false};
    Move list[MAX_MOVES];
    const int count = board_outcome(b) == ONGOING ? movegen_all(b, list) : 0;
    u64 total       = 0;
    for (int i = 0; i < count; i++) {
        Undo undo;
        board_make(b, list[i], &undo);
        const u64 nodes = depth <= 1 ? 1 : perft(b, depth - 1, options);
        board_unmake(b, list[i], &undo);
        char text[MOVE_BUFFER];
        printf("%s: %llu\n", move_format(list[i], text), (unsigned long long)nodes);
        total += nodes;
    }
    printf("total: %llu\n", (unsigned long long)total);
}

/*
 * One position a line, '#' for comments:
 *
 *   <size> | <convention> | <position> | <count at depth 0> <count at depth 1> ...
 *
 * The convention is `none` or `leaf` (see PerftOptions) and the position is what TEI's
 * `position` takes.
 */
static int run_line(char *line, int max_depth, int number) {
    char *fields[4];
    char *cursor = line;
    for (int i = 0; i < 4; i++) {
        fields[i] = cursor;
        cursor    = strchr(cursor, '|');
        if (i < 3) {
            if (!cursor) {
                printf("line %d: expected four fields\n", number);
                return 1;
            }
            *cursor++ = '\0';
        }
    }

    const int size           = atoi(fields[0]);
    const bool ended_is_leaf = strstr(fields[1], "leaf") != NULL;
    Board board;
    if (!position_parse(&board, fields[2], size, 0, NULL, 0, NULL)) {
        printf("line %d: bad position:%s\n", number, fields[2]);
        return 1;
    }

    int failures = 0;
    char *token  = fields[3];
    char *end;
    for (int depth = 0; depth <= max_depth; depth++) {
        const unsigned long long expected = strtoull(token, &end, 10);
        if (end == token) {
            break;
        }
        token                        = end;
        const PerftOptions options   = {.bulk = true, .ended_is_leaf = ended_is_leaf};
        const unsigned long long got = perft(&board, depth, options);
        if (got != expected) {
            printf("line %d: FAIL depth %d: expected %llu, got %llu\n  %s\n", number, depth,
                   expected, got, fields[2]);
            failures++;
        }
    }
    return failures;
}

int perft_run_suite(const char *path, int max_depth) {
    FILE *file = fopen(path, "r");
    if (!file) {
        printf("cannot open %s\n", path);
        return 1;
    }

    static char line[8192];
    int number = 0, positions = 0, failures = 0;
    while (fgets(line, sizeof(line), file)) {
        number++;
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') {
            continue;
        }
        positions++;
        failures += run_line(line, max_depth, number);
    }
    fclose(file);
    printf("%s: %d positions, %d failures\n", path, positions, failures);
    return failures;
}
