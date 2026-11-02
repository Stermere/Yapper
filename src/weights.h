/* weights.h - what each term of the evaluation is worth, in hundredths of a flat. Written by
 * `yapper tune`: docs/DATA.md says how. */
#ifndef WEIGHTS_H
#define WEIGHTS_H

#include "eval.h"

/* A term's value in the opening and at the end of the game, in one word, so a term costs one
 * add. A sum of these stays one as long as each half fits in 16 bits. */
#define S(opening, ending) ((i32)((u32)(ending) << 16) + (opening))

static const i32 Weights[TERM_COUNT] = {
    [T_LEAD_1] = S(4, 102),      [T_LEAD_2] = S(8, 111),        [T_LEAD_3] = S(11, 99),
    [T_LEAD_4] = S(14, 92),      [T_LEAD_6] = S(13, 85),        [T_LEAD_9] = S(25, 79),
    [T_LEAD_14] = S(52, 68),     [T_LEAD_MORE] = S(108, 20),    [T_KOMI] = S(-66, 33),
    [T_RESERVE] = S(-102, -136), [T_WALL] = S(-15, 30),         [T_CAP] = S(-130, -30),
    [T_FLAT_INNER] = S(31, -3),  [T_FLAT_CORE] = S(12, 2),      [T_FLAT_CORNER] = S(-45, 3),
    [T_WALL_INNER] = S(45, 10),  [T_WALL_CORE] = S(5, 2),       [T_CAP_INNER] = S(213, 53),
    [T_CAP_CORE] = S(100, 41),   [T_FRIEND_FLAT] = S(47, 25),   [T_FRIEND_WALL] = S(89, 99),
    [T_FRIEND_CAP] = S(96, 144), [T_CAPTIVE_FLAT] = S(-33, -2), [T_CAPTIVE_WALL] = S(84, 39),
    [T_CAPTIVE_CAP] = S(67, 35), [T_HARD_CAP] = S(57, 74),      [T_GROUP_1] = S(71, 29),
    [T_GROUP_2] = S(18, 5),      [T_GROUP_3] = S(-35, 1),       [T_GROUP_4] = S(-56, 1),
    [T_GROUP_5] = S(-69, 1),     [T_ADJACENT] = S(-32, 6),      [T_DIAGONAL] = S(24, 4),
    [T_LIBERTY] = S(15, 1),      [T_FLAT_CONTACT] = S(10, -13), [T_WALL_CONTACT] = S(50, 0),
    [T_CAP_CONTACT] = S(62, 11), [T_THREAT_ONE] = S(-60, -60),  [T_THREAT_MANY] = S(-220, -220),
    [T_TEMPO] = S(150, 128),
};

#undef S

#endif
