/* tune.h - fits the evaluation's weights to self-play games. */
#ifndef TUNE_H
#define TUNE_H

#include "types.h"

/* `keep` takes one position in that many. With a scale of 0 this reports the scale that best
 * fits the weights as built, and writes nothing. Otherwise the fitted weights replace the file
 * at `out`, for the next build. Non-zero on failure. */
int tune_run(const char *out, int keep, double scale, char **paths, int path_count);

#endif
