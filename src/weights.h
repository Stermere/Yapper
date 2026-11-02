/* weights.h - what each term of the evaluation is worth, in hundredths of a flat. Written by
 * `yapper tune`: docs/DATA.md says how. */
#ifndef WEIGHTS_H
#define WEIGHTS_H

#include "eval.h"

/* A term's value in the opening and at the end of the game, in one word, so a term costs one
 * add. A sum of these stays one as long as each half fits in 16 bits. */
#define S(opening, ending) ((i32)((u32)(ending) << 16) + (opening))

static const i32 Weights[TERM_COUNT] = {
    [T_LEAD_1] = S(4, 101),      [T_LEAD_2] = S(8, 112),        [T_LEAD_3] = S(11, 98),
    [T_LEAD_4] = S(14, 91),      [T_LEAD_6] = S(12, 83),        [T_LEAD_9] = S(23, 77),
    [T_LEAD_14] = S(46, 68),     [T_LEAD_MORE] = S(105, 17),    [T_KOMI] = S(-63, 33),
    [T_RESERVE] = S(-91, -132),  [T_WALL] = S(-12, 30),         [T_CAP] = S(-105, -22),
    [T_FLAT_INNER] = S(29, -1),  [T_FLAT_CORE] = S(14, 1),      [T_FLAT_CORNER] = S(-44, -1),
    [T_WALL_INNER] = S(36, 5),   [T_WALL_CORE] = S(1, 0),       [T_CAP_INNER] = S(198, 52),
    [T_CAP_CORE] = S(84, 39),    [T_FRIEND_FLAT] = S(44, 21),   [T_FRIEND_WALL] = S(76, 98),
    [T_FRIEND_CAP] = S(82, 138), [T_CAPTIVE_FLAT] = S(-25, -3), [T_CAPTIVE_WALL] = S(82, 38),
    [T_CAPTIVE_CAP] = S(65, 33), [T_HARD_CAP] = S(46, 61),      [T_GROUP_1] = S(68, 25),
    [T_GROUP_2] = S(14, 3),      [T_GROUP_3] = S(-32, 2),       [T_GROUP_4] = S(-52, 1),
    [T_GROUP_5] = S(-63, 3),     [T_ADJACENT] = S(-29, 5),      [T_DIAGONAL] = S(21, 2),
    [T_LIBERTY] = S(15, -2),     [T_FLAT_CONTACT] = S(10, -15), [T_WALL_CONTACT] = S(57, -4),
    [T_CAP_CONTACT] = S(60, 8),  [T_THREAT_ONE] = S(-60, -60),  [T_THREAT_MANY] = S(-220, -220),
    [T_TEMPO] = S(140, 124),
};

#undef S

#endif
