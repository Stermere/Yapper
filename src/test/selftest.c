/* selftest.c - what perft cannot see. Perft proves the trees are the right size; this
 * proves the state behind them is exactly right at every step of random games, and pins
 * each rule to a position that fails if the rule is broken. */
#include "selftest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../bitboard.h"
#include "../board.h"
#include "../eval.h"
#include "../movegen.h"
#include "../nn.h"
#include "../notation.h"
#include "../policy.h"
#include "../search.h"

static int Failures;
static u64 Checks;

#define CHECK(cond, ...)                                   \
    do {                                                   \
        Checks++;                                          \
        if (unlikely(!(cond))) {                           \
            if (Failures++ < 20) {                         \
                printf("FAIL %s:%d ", __FILE__, __LINE__); \
                printf(__VA_ARGS__);                       \
                printf("\n");                              \
            }                                              \
        }                                                  \
    } while (0)

/* xorshift64*: a fixed stream, so a failure reproduces. */
static u64 RngState = 0x2545F4914F6CDD1DULL;

static u64 rng(void) {
    RngState ^= RngState >> 12;
    RngState ^= RngState << 25;
    RngState ^= RngState >> 27;
    return RngState * 0x2545F4914F6CDD1DULL;
}

static void show(const Board *b, char *out, size_t cap) { board_to_tps(b, out, cap); }

/* Everything derived, recomputed from the stacks. */
static void check_consistent(const Board *b) {
    char tps[TPS_BUFFER];
    show(b, tps, sizeof(tps));

    u64 owned[2]  = {0, 0};
    u64 tall      = 0;
    u64 words     = 0;
    int stones[2] = {0, 0}, caps[2] = {0, 0};
    for (int sq = 0; sq < SQUARE_COUNT; sq++) {
        const int height = board_height(b, sq);
        words |= b->stones[sq];
        if (!((b->mask >> sq) & 1)) {
            CHECK(height == 0, "stones off the board at %d: %s", sq, tps);
            continue;
        }
        if (height == 0) {
            CHECK(b->stones[sq] == 1 && b->deep[sq] == 0, "residue on empty %d: %s", sq, tps);
            continue;
        }
        const int top = (int)(b->stones[sq] & 1);
        owned[top] |= 1ULL << sq;
        tall |= (u64)(height > 1) << sq;
        for (int i = 0; i < height; i++) {
            stones[board_stone(b, sq, i)]++;
        }
        caps[top] += (int)((b->caps >> sq) & 1);
    }
    CHECK(owned[WHITE] == b->owned[WHITE], "white squares: %s", tps);
    CHECK(owned[BLACK] == b->owned[BLACK], "black squares: %s", tps);
    CHECK(tall == b->tall, "tall squares: %s", tps);
    CHECK((words >> TOWER_BITS != 0) == b->towers, "towers: %s", tps);
    CHECK((b->walls & b->caps) == 0, "wall and capstone on one square: %s", tps);
    CHECK(((b->walls | b->caps) & ~(b->owned[0] | b->owned[1])) == 0, "typed empty: %s", tps);
    CHECK(b->hash == board_compute_hash(b), "incremental hash: %s", tps);
    for (int color = 0; color < 2; color++) {
        CHECK(stones[color] - caps[color] + b->flats[color] == board_start_flats(b->size),
              "stones of colour %d: %s", color, tps);
        CHECK(caps[color] + b->capstones[color] == board_start_capstones(b->size),
              "capstones of colour %d: %s", color, tps);
    }
}

static void check_tps_round_trip(const Board *b) {
    char tps[TPS_BUFFER], again[TPS_BUFFER];
    board_to_tps(b, tps, sizeof(tps));
    Board parsed;
    CHECK(board_from_tps(&parsed, tps, b->halfkomi), "TPS did not parse: %s", tps);
    board_to_tps(&parsed, again, sizeof(again));
    CHECK(strcmp(tps, again) == 0, "TPS changed: %s became %s", tps, again);
    CHECK(parsed.hash == b->hash, "TPS changed the hash: %s", tps);
    CHECK(parsed.flats[0] == b->flats[0] && parsed.flats[1] == b->flats[1] &&
              parsed.capstones[0] == b->capstones[0] && parsed.capstones[1] == b->capstones[1],
          "TPS changed the reserves: %s", tps);
}

/* Every move the answer filter turns away has to leave the other side a road to place, or lose
 * on the spot. Only where the search asks it: no road for the mover, and stones and squares to
 * spare. */
static void check_answers(Board *b, const Move *list, int count, const char *tps) {
    const int us      = board_stm(b);
    const u64 threats = board_road_placements(b, !us);
    const u64 empty   = board_empty(b);
    const bool spare  = b->flats[us] + b->capstones[us] > 1 &&
                       b->flats[!us] + b->capstones[!us] > 1 && (empty & (empty - 1)) != 0;
    u64 ends[4];
    board_road_ends(b, us, ends);
    if (!threats || !spare || (board_road_gaps(ends) & empty) || movegen_road_spread(b, us, ends)) {
        return;
    }
    RoadAnswer answer;
    movegen_road_answer(b, !us, threats, &answer);
    const Outcome loss = us == WHITE ? BLACK_ROAD : WHITE_ROAD;
    for (int i = 0; i < count; i++) {
        if (movegen_may_answer(&answer, list[i])) {
            continue;
        }
        Undo undo;
        board_make(b, list[i], &undo);
        const Outcome outcome = board_outcome(b);
        const bool lost =
            outcome == loss || (outcome == ONGOING && board_road_placements(b, !us) != 0);
        board_unmake(b, list[i], &undo);
        char text[MOVE_BUFFER];
        CHECK(lost, "%s turned away but stops the road: %s", move_format(list[i], text), tps);
    }
}

/* Every move of the position, against every way of looking at it. */
static void check_moves(Board *b) {
    static u8 seen[65536];
    static Move list[MAX_MOVES];
    char tps[TPS_BUFFER];
    show(b, tps, sizeof(tps));

    const int count = movegen_all(b, list);
    CHECK(count > 0, "no moves in an unfinished game: %s", tps);
    CHECK(count == movegen_count(b), "count %d vs %d: %s", count, movegen_count(b), tps);

    memset(seen, 0, sizeof(seen));
    const Board before = *b;
    const u64 winning  = b->ply >= 2 ? board_road_placements(b, board_stm(b)) : 0;
    u64 found          = 0;
    bool spread_road   = false;

    for (int i = 0; i < count; i++) {
        const Move m = list[i];
        char text[MOVE_BUFFER];
        CHECK(!seen[m], "duplicate %s: %s", move_format(m, text), tps);
        seen[m] = 1;
        CHECK(movegen_is_legal(b, m), "generated but illegal %s: %s", move_format(m, text), tps);
        CHECK(move_parse(move_format(m, text)) == m, "PTN round trip %s: %s", text, tps);

        Undo undo;
        board_make(b, m, &undo);
        const Outcome fast = board_outcome_after(b, &undo);
        const Outcome slow = board_outcome(b);
        CHECK(fast == slow, "outcome %d vs %d after %s: %s", fast, slow, move_format(m, text), tps);
        if (move_is_place(m)) {
            CHECK(board_outcome_after_place(b, m) == slow, "outcome after placing %s: %s",
                  move_format(m, text), tps);
        }
        CHECK(b->hash == board_compute_hash(b), "hash after %s: %s", move_format(m, text), tps);
        const Outcome win = board_stm(&before) == WHITE ? WHITE_ROAD : BLACK_ROAD;
        if (move_is_place(m) && move_stone(m) != WALL && slow == win) {
            found |= 1ULL << move_square(m);
        }
        spread_road |= !move_is_place(m) && slow == win;
        board_unmake(b, m, &undo);
        CHECK(memcmp(b, &before, sizeof(Board)) == 0, "unmake of %s: %s", move_format(m, text),
              tps);
    }

    if (b->ply >= 2 && (b->flats[board_stm(b)] || b->capstones[board_stm(b)])) {
        CHECK(found == winning, "road placements %llx vs %llx: %s", (unsigned long long)winning,
              (unsigned long long)found, tps);
    }
    if (b->ply >= 2) {
        u64 ends[4];
        board_road_ends(b, board_stm(b), ends);
        CHECK(movegen_road_spread(b, board_stm(b), ends) == spread_road,
              "road by a spread, expected %d: %s", spread_road, tps);
        CHECK(memcmp(b, &before, sizeof(Board)) == 0, "looking for a road spread: %s", tps);
        check_answers(b, list, count, tps);
    }

    /* Random words catch legality claims the generator would never produce. */
    for (int i = 0; i < 400; i++) {
        const Move m = (Move)rng();
        char text[MOVE_BUFFER];
        CHECK(movegen_is_legal(b, m) == (seen[m] != 0), "legality of %s: %s", move_format(m, text),
              tps);
    }
}

/* `b` turned or reflected, one of eight ways, and with the colours exchanged if asked. */
static void board_transformed(const Board *b, Board *out, int transform, bool swap) {
    board_clear(out, b->size, b->halfkomi);
    const int last = b->size - 1;
    u64 squares    = b->owned[WHITE] | b->owned[BLACK];
    while (squares) {
        const int sq     = pop_lsb(&squares);
        const int height = board_height(b, sq);
        u8 colors[MAX_STONES];
        for (int i = 0; i < height; i++) {
            colors[height - 1 - i] = (u8)(board_stone(b, sq, i) ^ swap);
        }
        int rank = sq >> 3, file = sq & 7;
        rank = transform & 1 ? last - rank : rank;
        file = transform & 2 ? last - file : file;
        if (transform & 4) {
            const int held = rank;
            rank           = file;
            file           = held;
        }
        board_set_stack(out, rank * 8 + file, colors, height, board_top(b, sq));
    }
    out->ply = b->ply + swap;
    board_refresh(out);
}

static int evaluate_here(const Board *b) {
    return evaluate(b, board_road_placements(b, !board_stm(b)));
}

/* An evaluation that treats a square or a colour differently is a bug nothing else finds:
 * games still play, and every test but this one still passes. */
static void check_eval(const Board *b) {
    char tps[TPS_BUFFER];
    show(b, tps, sizeof(tps));

    const int score = evaluate_here(b);
    CHECK(abs(score) < SCORE_WIN_BOUND / 2, "eval %d out of range: %s", score, tps);

    Board other;
    for (int transform = 1; transform < 8; transform++) {
        board_transformed(b, &other, transform, false);
        CHECK(evaluate_here(&other) == score, "transform %d changed the eval: %s", transform, tps);
    }
    if (b->halfkomi == 0) {
        board_transformed(b, &other, 0, true);
        CHECK(evaluate_here(&other) == score, "swapping colours changed the eval: %s", tps);
    }
}

/* Mostly spreads once there are any, or the stacks that make the rules hard never form. */
static int compare_ints(const void *a, const void *b) {
    const int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

/* `m` on the board board_transformed() makes, its square and direction turned the same way. */
static Move move_transformed(Move m, int size, int transform) {
    if (m == MOVE_NONE) {
        return m;
    }
    const int last = size - 1;
    int rank = move_square(m) >> 3, file = move_square(m) & 7;
    rank = transform & 1 ? last - rank : rank;
    file = transform & 2 ? last - file : file;
    if (transform & 4) {
        const int held = rank;
        rank           = file;
        file           = held;
    }
    if (move_is_place(m)) {
        return move_place(rank * 8 + file, move_stone(m));
    }
    int dir = (int)move_direction(m);
    dir     = transform & 1 && (dir == NORTH || dir == SOUTH) ? dir ^ 2 : dir;
    dir     = transform & 2 && (dir == EAST || dir == WEST) ? dir ^ 2 : dir;
    dir     = transform & 4 ? dir ^ 1 : dir;
    return move_spread(rank * 8 + file, (Direction)dir, move_pattern(m));
}

/* The prior of every move of the position, sorted, which a turned or recoloured board must give
 * back unchanged without anything having to say which move became which. Scoring a node's moves
 * at once must give what scoring each alone does. */
static int sorted_priors(const Board *b, Move prev, Move prev2, int *out) {
    static Move list[MAX_MOVES];
    const int count = movegen_all(b, list);
    u64 ends[4], their_ends[4];
    board_road_ends(b, board_stm(b), ends);
    board_road_ends(b, !board_stm(b), their_ends);
    PolicyContext ctx;
    policy_context(b, ends, their_ends, prev, prev2, &ctx);
    static i32 batch[MAX_MOVES];
    policy_logits(b, &ctx, list, count, batch);
    for (int i = 0; i < count; i++) {
        out[i] = policy_logit(b, &ctx, list[i]);
        CHECK(out[i] == batch[i], "a node's priors at once differ from one by one");
    }
    qsort(out, (size_t)count, sizeof(int), compare_ints);
    return count;
}

/* `prev` and `prev2` are the moves that led here, turned with the board. */
static void check_policy(const Board *b, Move prev, Move prev2) {
    static int base[MAX_MOVES], other[MAX_MOVES];
    char tps[TPS_BUFFER];
    show(b, tps, sizeof(tps));
    const int count = sorted_priors(b, prev, prev2, base);
    Board turned;
    for (int transform = 1; transform < 8 + (b->halfkomi == 0); transform++) {
        board_transformed(b, &turned, transform & 7, transform == 8);
        const int n = sorted_priors(&turned, move_transformed(prev, b->size, transform & 7),
                                    move_transformed(prev2, b->size, transform & 7), other);
        CHECK(n == count && memcmp(base, other, sizeof(int) * (size_t)count) == 0,
              "transform %d changed the move priors: %s", transform, tps);
    }
}

static Move pick_move(const Move *list, int count) {
    if (rng() % 4) {
        int spreads = 0;
        for (int i = 0; i < count; i++) {
            spreads += !move_is_place(list[i]);
        }
        if (spreads) {
            int target = (int)(rng() % (u64)spreads);
            for (int i = 0; i < count; i++) {
                if (!move_is_place(list[i]) && target-- == 0) {
                    return list[i];
                }
            }
        }
    }
    return list[rng() % (u64)count];
}

static void random_games(int size, int games, int halfkomi) {
    static Move list[MAX_MOVES];
    for (int game = 0; game < games; game++) {
        Board board;
        board_clear(&board, size, halfkomi);
        Undo undo;
        Move made[2] = {MOVE_NONE, MOVE_NONE};
        for (int ply = 0; ply < 400; ply++) {
            check_consistent(&board);
            check_tps_round_trip(&board);
            check_moves(&board);
            if (ply >= 2) {
                check_eval(&board);
                check_policy(&board, made[0], made[1]);
            }
            const int count = movegen_all(&board, list);
            made[1]         = made[0];
            made[0]         = pick_move(list, count);
            board_make(&board, made[0], &undo);
            if (board_outcome_after(&board, &undo) != ONGOING) {
                break;
            }
        }
    }
}

/* A frame carried over gaps, as table hits leave them in a search, against one built from
 * nothing. Any square the carry misses shows up here, and nowhere else: games still play. */
static void check_frame(const Board *b, NNFrame *frames, const Move *made, int ply) {
    char tps[TPS_BUFFER];
    show(b, tps, sizeof(tps));

    NNFrame *frame = &frames[ply];
    u64 touched    = 0;
    int base       = ply - 1;
    while (base >= 0 && ply - base <= 8) {
        touched |= nn_move_squares(made[base]);
        if (frames[base].computed) {
            break;
        }
        base--;
    }
    if (base >= 0 && ply - base <= 8) {
        nn_derive(b, frame, &frames[base], touched);
    } else {
        nn_refresh(b, frame);
    }
    NNFrame fresh;
    nn_refresh(b, &fresh);
    CHECK(memcmp(frame->acc, fresh.acc, sizeof(fresh.acc)) == 0, "carried accumulator: %s", tps);
    CHECK(memcmp(frame->feature, fresh.feature, sizeof(fresh.feature)) == 0, "carried tokens: %s",
          tps);
    const u64 threats = board_road_placements(b, !board_stm(b));
    CHECK(evaluate_frame(b, threats, frame) == evaluate(b, threats), "frame scored apart: %s", tps);
    frame->computed = rng() % 3 != 0;
}

/* Random weights on every size, whatever is compiled in, so the network is held to its gates
 * before one has been trained. Symmetry holds for any weights, by construction. */
static void network_games(int size, int games, int halfkomi) {
    static i16 filters[NN_GRID * NN_GRID * NN_INPUTS * NN_FILTERS];
    static i16 bias[NN_FILTERS], output[NN_BUCKETS * NN_POOLS];
    static i32 skip[NN_BUCKETS * 4];
    static i16 l2[NN_BUCKETS * NN_L2 * NN_POOLS], l2_skip[NN_BUCKETS * NN_L2 * 3];
    static i16 l3[NN_BUCKETS * NN_L3 * NN_L2], l4[NN_BUCKETS * NN_L3];
    static i32 l2_bias[NN_BUCKETS * NN_L2], l3_bias[NN_BUCKETS * NN_L3];
    for (int i = 0; i < NN_GRID * NN_GRID * NN_INPUTS * NN_FILTERS; i++) {
        filters[i] = (i16)(rng() % 129) - 64;
    }
    for (int i = 0; i < NN_FILTERS; i++) {
        bias[i] = (i16)(rng() % 201) - 100;
    }
    for (int i = 0; i < NN_BUCKETS * NN_POOLS; i++) {
        output[i] = (i16)(rng() % 255) - 127;
    }
    /* The head's units sit inside their clips often enough to be tested on both sides of them. */
    for (int i = 0; i < NN_BUCKETS * NN_L2 * NN_POOLS; i++) {
        l2[i] = (i16)(rng() % 65) - 32;
    }
    for (int i = 0; i < NN_BUCKETS * NN_L2 * 3; i++) {
        l2_skip[i] = (i16)(rng() % 65) - 32;
    }
    for (int i = 0; i < NN_BUCKETS * NN_L2; i++) {
        l2_bias[i] = (i32)(rng() % (NN_QU * NN_Q2 / 2)) + NN_QU * NN_Q2 / 4;
    }
    for (int i = 0; i < NN_BUCKETS * NN_L3 * NN_L2; i++) {
        l3[i] = (i16)(rng() % 129) - 64;
    }
    for (int i = 0; i < NN_BUCKETS * NN_L3; i++) {
        l3_bias[i] = (i32)(rng() % (NN_QY * NN_Q3 / 2)) + NN_QY * NN_Q3 / 4;
        l4[i]      = (i16)(rng() % 401) - 200;
    }
    for (int i = 0; i < NN_BUCKETS * 4; i++) {
        skip[i] = (i32)(rng() % 2001) - 1000;
    }
    const NNWeights random = {filters, bias, output, skip, l2, l2_skip, l2_bias, l3, l3_bias, l4};
    CHECK(nn_install(size, &random), "installing a network on %d", size);

    static Move list[MAX_MOVES];
    static NNFrame frames[400];
    static Move made[400];
    for (int game = 0; game < games; game++) {
        Board board;
        board_clear(&board, size, halfkomi);
        Undo undo;
        for (int ply = 0; ply < 400; ply++) {
            check_frame(&board, frames, made, ply);
            if (ply >= 2) {
                check_eval(&board);
            }
            const int count = movegen_all(&board, list);
            made[ply]       = pick_move(list, count);
            board_make(&board, made[ply], &undo);
            if (board_outcome_after(&board, &undo) != ONGOING) {
                break;
            }
        }
    }
    CHECK(nn_install(size, nn_builtin(size)), "restoring the network on %d", size);
}

static void expect_outcome(const char *position, int size, int halfkomi, Outcome expected,
                           const char *rule) {
    Board board;
    CHECK(position_parse(&board, position, size, halfkomi, NULL, 0, NULL), "%s: position", rule);
    const Outcome got = board_outcome(&board);
    CHECK(got == expected, "%s: expected %d, got %d", rule, expected, got);
}

static void expect_legal(const char *position, int size, const char *move, bool legal,
                         const char *rule) {
    Board board;
    CHECK(position_parse(&board, position, size, 0, NULL, 0, NULL), "%s: position", rule);
    const Move m = move_parse(move);
    CHECK(movegen_is_legal(&board, m) == legal, "%s: %s should be %s", rule, move,
          legal ? "legal" : "illegal");
}

/* One position per rule of https://ustak.org/play-beautiful-game-tak/. */
static void rules(void) {
    /* Piece counts. */
    for (int size = MIN_SIZE; size <= MAX_SIZE; size++) {
        Board board;
        board_clear(&board, size, 0);
        static const int flats[] = {10, 15, 21, 30, 40, 50};
        static const int caps[]  = {0, 0, 1, 1, 2, 2};
        CHECK(board.flats[WHITE] == flats[size - 3], "flats on %d", size);
        CHECK(board.capstones[BLACK] == caps[size - 3], "capstones on %d", size);
    }

    /* The first turn of each player places the opponent's stone, flat. */
    {
        Board board;
        board_clear(&board, 6, 0);
        CHECK(!movegen_is_legal(&board, move_parse("Sa1")), "no wall on the first turn");
        CHECK(!movegen_is_legal(&board, move_parse("Ca1")), "no capstone on the first turn");
        Undo undo;
        board_make(&board, move_parse("a1"), &undo);
        CHECK(board.owned[BLACK] == 1 && board.owned[WHITE] == 0, "white places black");
        CHECK(board.flats[BLACK] == 29 && board.flats[WHITE] == 30, "reserve of the stone's owner");
        CHECK(!movegen_is_legal(&board, move_parse("a1>")), "black cannot move its first stone");
        board_make(&board, move_parse("b1"), &undo);
        CHECK(board.owned[WHITE] == 2 && board.flats[WHITE] == 29, "black places white");
        CHECK(movegen_is_legal(&board, move_parse("Cc1")), "third ply is unrestricted");
    }

    /* Roads: edge to opposite edge, orthogonal, flats and capstones but never walls. */
    expect_outcome("tps 1,1,1,1,1/x4,2/x4,2/x4,2/2,x4 2 10", 5, 0, WHITE_ROAD, "straight road");
    expect_outcome("tps 2,x3,1/2,x3,1/2,x3,1/2,x3,1/2,x3,1 2 10", 5, 0, WHITE_ROAD,
                   "double road, black to move: white made it");
    expect_outcome("tps 2,x3,1/2,x3,1/2,x3,1/2,x3,1/2,x3,1 1 11", 5, 0, BLACK_ROAD,
                   "double road, white to move: black made it");
    expect_outcome("tps 1,1,1C,1,1/x4,2/x4,2/x4,2/2,x4 2 10", 5, 0, WHITE_ROAD, "capstone in road");
    expect_outcome("tps 1,1,1S,1,1/x4,2/x4,2/x4,2/2,x4 2 10", 5, 0, ONGOING, "wall breaks road");
    expect_outcome("tps 1,1,x3/x,1,x3/x,1,1,x2/x2,1,x2/x2,1,1,1 2 10", 5, 0, WHITE_ROAD, "zig-zag");
    expect_outcome("tps 1,x4/x,1,x3/x2,1,x2/x3,1,x/x4,1 2 10", 5, 0, ONGOING, "no diagonals");
    expect_outcome("tps 21,21,21,21,21/x4,2/x4,2/x4,2/2,x4 2 10", 5, 0, WHITE_ROAD,
                   "stacks count by their top stone");
    expect_outcome("tps x4,2/x4,2/1,1,1,1,1/x4,2/2,x4 2 10", 5, 0, WHITE_ROAD, "east-west road");

    /* A move that only completes the opponent's road loses. Black's stack on b1 leaves
     * white on top, and white's rank is complete. */
    {
        Board board;
        CHECK(board_from_tps(&board, "x5/x5/x5/x5/1,12,1,1,1 2 10", 0), "suicide: position");
        Undo undo;
        board_make(&board, move_parse("b1+"), &undo);
        CHECK(board_outcome_after(&board, &undo) == WHITE_ROAD, "suicide road goes to opponent");
    }

    /* A move that completes both roads wins for the mover. Lifting black's two stones off
     * a3 uncovers the white stone that finishes file a, and they land on c3 and finish
     * file c. */
    {
        Board board;
        CHECK(board_from_tps(&board, "1,x,2,x2/1,x,2,x2/122,x4/1,x,2,x2/1,x,2,x2 2 10", 0),
              "both roads: position");
        Undo undo;
        board_make(&board, move_parse("2a3>11"), &undo);
        CHECK(board_outcome_after(&board, &undo) == BLACK_ROAD, "both roads go to the mover");
        CHECK(board_outcome(&board) == BLACK_ROAD, "both roads from scratch");
    }

    /* Flats end the game on a full board or when a player's last piece goes down, and
     * only flats count: not walls, not capstones. */
    expect_outcome("tps 1,2,1/2,1,2/1,2,1 1 8", 3, 0, WHITE_FLATS, "full board, five to four");
    expect_outcome("tps 1,2,1/2,1,2/1,2,2 1 8", 3, 0, BLACK_FLATS, "full board, five to four");
    expect_outcome("tps 1S,2,1/2,1,2/1,2,1S 1 8", 3, 0, BLACK_FLATS, "walls do not count");
    expect_outcome("tps 1,2,1/2,1,2/1,2,1 1 8", 3, 2, DRAW, "one komi against a one-flat lead");
    expect_outcome("tps 1,2,1/2,1,2/1,2,1 1 8", 3, 1, WHITE_FLATS, "half komi against a lead");
    expect_outcome("tps 1,2,1/2,1,2/1,2,1 1 8", 3, 3, BLACK_FLATS, "komi beats a lead");
    expect_outcome("tps 2,2,2,x2/1,1,1,x2/x5/x5/x5 1 4", 5, 0, ONGOING, "stones left: not over");
    {
        /* All ten of white's stones are on the board, so white has nothing to place. */
        Board board;
        CHECK(board_from_tps(&board, "111111111,1,x/x3/x2,2 2 12", 0), "reserves: position");
        CHECK(board.flats[WHITE] == 0, "reserves: white is out");
        CHECK(board_outcome(&board) == WHITE_FLATS, "last piece ends the game");
    }

    /* The carry limit is the board size, however tall the stack. */
    {
        const char *tall = "tps x5,1212121/x6/x6/x6/x6/2,x5 1 10";
        expect_legal(tall, 6, "6f6-", true, "carry six on 6x6");
        expect_legal(tall, 6, "7f6-", false, "carry seven on 6x6");
        expect_legal(tall, 6, "2f6-", true, "a partial carry");
    }

    /* A spread must drop at least one stone on every square it passes, and goes straight. */
    expect_legal("tps x4,121/x5/x5/x5/2,x4 1 10", 5, "3e5-111", true, "one stone a square");
    expect_legal("tps x4,121/x5/x5/x5/2,x4 1 10", 5, "3e5-12", true, "uneven drops");
    expect_legal("tps x4,121/x5/x5/x5/2,x4 1 10", 5, "3e5-1", false, "drops must add up");
    expect_legal("tps x4,121/x5/x5/x5/2,x4 1 10", 5, "3e5>", false, "off the board");

    /* Walls and capstones block; only a lone capstone flattens, and only at the end. */
    const char *cap = "tps x5/x5/x,21C,x,2S,x/x5/x5 1 10";
    expect_legal(cap, 5, "b3>", true, "open square");
    expect_legal(cap, 5, "2b3>2", true, "both stones to the open square");
    expect_legal(cap, 5, "2b3>11", true, "capstone alone onto the wall");
    expect_legal("tps x5/x5/x,21C,2S,x2/x5/x5 1 10", 5, "b3>", true, "capstone flattens wall");
    expect_legal("tps x5/x5/x,21C,2S,x2/x5/x5 1 10", 5, "2b3>", false, "not with company");
    expect_legal("tps x5/x5/x,21C,1S,x2/x5/x5 1 10", 5, "b3>", true, "flatten its own wall");
    expect_legal("tps x5/x5/x,21,2S,x2/x5/x5 1 10", 5, "b3>", false, "flats do not flatten");
    expect_legal("tps x5/x5/x,21S,2S,x2/x5/x5 1 10", 5, "b3>", false, "walls do not flatten");
    expect_legal("tps x5/x5/x,21S,x3/x5/x5 1 10", 5, "b3>", true, "a wall may move");
    expect_legal("tps x5/x5/x,1C,2C,x2/x5/x5 1 10", 5, "b3>", false, "capstones never move on");
    expect_legal("tps x5/x5/x,1,2S,x,x/x5/x5 1 10", 5, "b3>", false, "no jumping a wall");
    expect_legal("tps x5/x5/x,11,2C,x,x/x5/x5 1 10", 5, "2b3>11", false, "no jumping a capstone");
    expect_legal("tps x5/x5/x,21C,2S,1S,x/x5/x5 1 10", 5, "b3>", true, "one wall at a time");

    /* Flattening: the wall joins the stack as a flat of its own colour. */
    {
        Board board;
        CHECK(board_from_tps(&board, "x5/x5/x,21C,2S,x2/x5/x5 1 10", 0), "flatten: position");
        const Board before = board;
        Undo undo;
        board_make(&board, move_parse("b3>"), &undo);
        const int wall = 2 * 8 + 2;
        CHECK(!((board.walls >> wall) & 1), "wall is gone");
        CHECK((board.caps >> wall) & 1, "capstone is on top");
        CHECK(board_height(&board, wall) == 2, "wall stayed as a stone");
        CHECK((board.stones[wall] >> 1) & 1, "and stayed black");
        CHECK(board.reversible == 0, "a flattening cannot be taken back");
        board_unmake(&board, move_parse("b3>"), &undo);
        CHECK(memcmp(&board, &before, sizeof(Board)) == 0, "unmake restores the wall");
    }

    /* No passing: a position that is not over always has a move. Covered by check_moves. */
}

/* A tower past 64 stones, which nothing else can reach. It alternates colours, so any slip
 * in the two-word shift register shows up in the stones left behind. */
static void towers(void) {
    char tps[TPS_BUFFER] = "";
    char *p              = tps;
    for (int i = 0; i < 98; i++) {
        *p++ = (char)('1' + (i & 1));
    }
    sprintf(p, ",x7/x8/x8/x8/x8/x8/x8/x8 2 60");
    Board board;
    CHECK(board_from_tps(&board, tps, 0), "tower: position");
    CHECK(board_height(&board, 56) == 98, "tower: height %d", board_height(&board, 56));
    check_consistent(&board);
    check_tps_round_trip(&board);

    /* Walk it apart and back together, in every direction available. */
    static Move list[MAX_MOVES];
    const Board before = board;
    const int count    = movegen_all(&board, list);
    CHECK(count > 100, "tower: %d moves", count);
    for (int i = 0; i < count; i++) {
        char text[MOVE_BUFFER];
        Undo undo;
        board_make(&board, list[i], &undo);
        CHECK(board.hash == board_compute_hash(&board), "tower hash %s",
              move_format(list[i], text));
        Undo inner;
        Move replies[MAX_MOVES];
        const int reply_count = movegen_all(&board, replies);
        for (int j = 0; j < reply_count; j += 7) {
            board_make(&board, replies[j], &inner);
            CHECK(board.hash == board_compute_hash(&board), "tower reply hash");
            board_unmake(&board, replies[j], &inner);
        }
        board_unmake(&board, list[i], &undo);
        CHECK(memcmp(&board, &before, sizeof(Board)) == 0, "tower unmake %s",
              move_format(list[i], text));
    }
}

int selftest_run(void) {
    rules();
    towers();
    /* Random weights, so that a prior that tells a turned board from the original shows it. */
    static i16 rows[4096 * POLICY_LANES];
    i16 bias[POLICY_LANES], out[POLICY_LANES];
    for (int i = 0; i < policy_feature_count() * POLICY_LANES && i < 4096 * POLICY_LANES; i++) {
        rows[i] = (i16)(rng() % 201) - 100;
    }
    for (int i = 0; i < POLICY_LANES; i++) {
        bias[i] = (i16)(rng() % 129);
        out[i]  = i < POLICY_HIDDEN ? (i16)(rng() % 513) - 256 : 0;
    }
    policy_install(rows, bias, out);
    for (int size = MIN_SIZE; size <= MAX_SIZE; size++) {
        random_games(size, size <= 6 ? 400 : 200, 0);
    }
    random_games(6, 200, 4);
    policy_install(NULL, NULL, NULL);
    for (int size = MIN_SIZE; size <= MAX_SIZE; size++) {
        network_games(size, 60, 0);
    }
    network_games(6, 30, 4);
    printf("selftest: %llu checks, %d failures\n", (unsigned long long)Checks, Failures);
    return Failures;
}
