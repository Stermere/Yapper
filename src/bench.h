/* bench.h - the deterministic node-count benchmark. */
#ifndef BENCH_H
#define BENCH_H

/* Part of the benchmark's definition. Changing it, or the positions, changes every node
 * count on record, so do it deliberately and say so in docs/EXPERIMENTS.md. */
#define BENCH_DEFAULT_DEPTH 9

/* Pinned, or a table of another size would change the node count. */
#define BENCH_HASH_MB 64

/* Searches a frozen set of positions. The last line printed is always
 * "<nodes> nodes <nps> nps". */
void bench_run(int depth);

#endif
