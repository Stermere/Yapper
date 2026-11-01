/* notation.c - PTN moves and TPS positions. */
#include "notation.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "movegen.h"

static bool parse_square(const char *text, int *sq) {
    const int file = text[0] - 'a';
    const int rank = text[1] - '1';
    if (file < 0 || file >= MAX_SIZE || rank < 0 || rank >= MAX_SIZE) {
        return false;
    }
    *sq = rank * 8 + file;
    return true;
}

Move move_parse(const char *text) {
    const char *p = text;
    int count     = 0;
    bool counted  = false;
    if (*p >= '1' && *p <= '8') {
        count   = *p++ - '0';
        counted = true;
    }

    StoneType type = FLAT;
    bool typed     = false;
    if (*p == 'F' || *p == 'S' || *p == 'C') {
        type  = *p == 'S' ? WALL : *p == 'C' ? CAP : FLAT;
        typed = true;
        p++;
    }

    int sq;
    if (!parse_square(p, &sq)) {
        return MOVE_NONE;
    }
    p += 2;

    int dir = -1;
    for (int d = 0; d < 4; d++) {
        if (*p == DirectionChar[d]) {
            dir = d;
        }
    }
    if (dir < 0) {
        return counted ? MOVE_NONE : move_place(sq, type);
    }
    if (typed) {
        return MOVE_NONE;
    }
    p++;

    if (!counted) {
        count = 1;
    }

    int pattern = 0, dropped = 0;
    while (*p >= '1' && *p <= '8') {
        dropped += *p++ - '0';
        if (dropped > MAX_SIZE) {
            return MOVE_NONE;
        }
        pattern |= 1 << (dropped - 1);
    }
    if (dropped == 0) {
        pattern = 1 << (count - 1);
        dropped = count;
    }
    if (dropped != count) {
        return MOVE_NONE;
    }

    return move_spread(sq, (Direction)dir, pattern);
}

char *move_format(Move m, char *out) {
    const int sq    = move_square(m);
    const char file = (char)('a' + (sq & 7));
    const char rank = (char)('1' + (sq >> 3));
    char *p         = out;

    if (move_is_place(m)) {
        if (move_stone(m) == WALL) {
            *p++ = 'S';
        } else if (move_stone(m) == CAP) {
            *p++ = 'C';
        }
        *p++ = file;
        *p++ = rank;
    } else {
        const int carry = move_carry(m);
        int pattern     = move_pattern(m);
        if (carry > 1) {
            *p++ = (char)('0' + carry);
        }
        *p++ = file;
        *p++ = rank;
        *p++ = DirectionChar[move_direction(m)];
        if (__builtin_popcount(pattern) > 1) {
            while (pattern) {
                const int d = __builtin_ctz(pattern) + 1;
                pattern >>= d;
                *p++ = (char)('0' + d);
            }
        }
    }
    *p = '\0';
    return out;
}

bool board_from_tps(Board *b, const char *tps, int halfkomi) {
    /* The size is whatever the first rank adds up to. */
    int size = 1;
    for (const char *p = tps; *p && *p != '/' && *p != ' '; p++) {
        if (*p == ',') {
            size++;
        } else if (*p == 'x' && isdigit((unsigned char)p[1])) {
            size += p[1] - '0' - 1;
        }
    }
    if (size < MIN_SIZE || size > MAX_SIZE) {
        return false;
    }
    board_clear(b, size, halfkomi);

    const char *p = tps;
    for (int rank = size - 1; rank >= 0; rank--) {
        int file = 0;
        while (file < size) {
            if (*p == 'x') {
                p++;
                int run = 1;
                if (isdigit((unsigned char)*p)) {
                    run = *p++ - '0';
                }
                file += run;
            } else {
                u8 colors[MAX_STONES];
                int height = 0;
                while (*p == '1' || *p == '2') {
                    if (height == MAX_STONES) {
                        return false;
                    }
                    colors[height++] = (u8)(*p++ - '1');
                }
                if (height == 0) {
                    return false;
                }
                StoneType top = FLAT;
                if (*p == 'S' || *p == 'C') {
                    top = *p == 'S' ? WALL : CAP;
                    p++;
                }
                board_set_stack(b, rank * 8 + file, colors, height, top);
                file++;
            }
            if (file < size) {
                if (*p != ',') {
                    return false;
                }
                p++;
            }
        }
        if (file != size) {
            return false;
        }
        if (rank > 0) {
            if (*p != '/') {
                return false;
            }
            p++;
        }
    }

    int player = 0, move = 0;
    char tail = 0;
    if (sscanf(p, " %d %d %c", &player, &move, &tail) != 2 || player < 1 || player > 2 ||
        move < 1) {
        return false;
    }
    b->ply = (move - 1) * 2 + (player - 1);
    board_refresh(b);
    return true;
}

void board_to_tps(const Board *b, char *out, size_t cap) {
    char *p         = out;
    char *const end = out + cap - 16;
    for (int rank = b->size - 1; rank >= 0 && p < end; rank--) {
        int run    = 0;
        bool first = true;
        for (int file = 0; file < b->size && p < end; file++) {
            const int sq     = rank * 8 + file;
            const int height = board_height(b, sq);
            if (height == 0) {
                run++;
                continue;
            }
            if (run) {
                p += sprintf(p, "%sx", first ? "" : ",");
                if (run > 1) {
                    *p++ = (char)('0' + run);
                }
                first = false;
                run   = 0;
            }
            if (!first) {
                *p++ = ',';
            }
            first = false;
            for (int i = height - 1; i >= 0 && p < end; i--) {
                *p++ = (char)('1' + board_stone(b, sq, i));
            }
            const StoneType top = board_top(b, sq);
            if (top != FLAT) {
                *p++ = top == WALL ? 'S' : 'C';
            }
        }
        if (run) {
            p += sprintf(p, "%sx", first ? "" : ",");
            if (run > 1) {
                *p++ = (char)('0' + run);
            }
        }
        if (rank > 0) {
            *p++ = '/';
        }
    }
    sprintf(p, " %d %d", board_stm(b) + 1, b->ply / 2 + 1);
}

bool position_parse(Board *b, const char *text, int size, int halfkomi, u64 *hashes, int hashes_cap,
                    int *count) {
    if (count) {
        *count = 0;
    }
    while (*text == ' ') {
        text++;
    }

    const char *moves = strstr(text, "moves");
    if (strncmp(text, "startpos", 8) == 0) {
        board_clear(b, size, halfkomi);
    } else if (strncmp(text, "tps ", 4) == 0) {
        if (!board_from_tps(b, text + 4, halfkomi)) {
            /* board_from_tps() reads to the end, so retry without the moves. */
            char buffer[TPS_BUFFER];
            const size_t length = moves ? (size_t)(moves - (text + 4)) : strlen(text + 4);
            if (length >= sizeof(buffer)) {
                return false;
            }
            memcpy(buffer, text + 4, length);
            buffer[length] = '\0';
            if (!board_from_tps(b, buffer, halfkomi)) {
                return false;
            }
        }
    } else {
        return false;
    }

    if (!moves) {
        return true;
    }

    const char *p = moves + 5;
    for (;;) {
        while (*p == ' ') {
            p++;
        }
        if (*p == '\0' || *p == '\n' || *p == '\r') {
            return true;
        }
        char word[MOVE_BUFFER];
        size_t length = 0;
        while (*p && !isspace((unsigned char)*p)) {
            if (length + 1 < sizeof(word)) {
                word[length++] = *p;
            }
            p++;
        }
        word[length] = '\0';

        const Move m = move_parse(word);
        if (!movegen_is_legal(b, m)) {
            return false;
        }
        if (hashes && count && *count < hashes_cap) {
            hashes[(*count)++] = b->hash;
        }
        Undo undo;
        board_make(b, m, &undo);
    }
}
