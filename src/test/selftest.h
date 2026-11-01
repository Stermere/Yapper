/* selftest.h - board and rule invariants, compiled into the binary on purpose so the gate
 * cannot pass on a different build than the one that plays. */
#ifndef SELFTEST_H
#define SELFTEST_H

/* Returns the number of failures. */
int selftest_run(void);

#endif
