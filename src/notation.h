/* notation.h - PTN moves and TPS positions. */
#ifndef NOTATION_H
#define NOTATION_H

#include <stddef.h>

#include "board.h"
#include "types.h"

/* Room for any TPS: 64 squares of up to 104 stones, with separators and the two fields. */
#define TPS_BUFFER 512

/* Longest PTN move, "8h8>11111111" plus the terminator. */
#define MOVE_BUFFER 16

/* Reads a PTN move: placement ("a1", "Fa1", "Sa1", "Ca1") or spread ("a1>", "3a1>12"),
 * with any trailing '*' and annotation marks ignored. Says nothing about legality. */
Move move_parse(const char *text);

/* The canonical PTN of `m`. Returns `out`. */
char *move_format(Move m, char *out);

/* False on anything malformed, leaving `b` unspecified. */
bool board_from_tps(Board *b, const char *tps, int halfkomi);

void board_to_tps(const Board *b, char *out, size_t cap);

/* The argument of TEI's `position`: "startpos [moves ...]" or "tps <tps> [moves ...]".
 * Every move is checked for legality. When `hashes` is given it receives the position key
 * before each move, for repetition detection, and `*count` how many there were. */
bool position_parse(Board *b, const char *text, int size, int halfkomi, u64 *hashes, int hashes_cap,
                    int *count);

#endif
