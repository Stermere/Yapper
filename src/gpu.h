/* gpu.h - an NVIDIA GPU for the tools, found when they run rather than when they are built.
 *
 * The driver and its runtime compiler are loaded by name, so no build needs CUDA and a machine
 * without it simply has no GPU. Kernels arrive as CUDA C source and are compiled for the card in
 * hand. Everything runs on the default stream, in the order it was issued. */
#ifndef GPU_H
#define GPU_H

#include <stdbool.h>
#include <stddef.h>

#include "types.h"

typedef u64 GpuPtr;

/* Loads the driver and the compiler, takes the first device, and compiles `source`. False with a
 * reason if any of it is missing; the tools then fall back to the CPU. */
bool gpu_open(const char *source, char *why, size_t why_size);

/* The card's name, after gpu_open(). */
const char *gpu_name(void);

/* A __global__ function of the compiled source, declared extern "C". NULL if absent. */
void *gpu_function(const char *name);

/* Each of these returns false and leaves the reason in gpu_error() if the driver refuses. */
bool gpu_alloc(GpuPtr *ptr, size_t bytes);
bool gpu_upload(GpuPtr to, const void *from, size_t bytes);
bool gpu_download(void *to, GpuPtr from, size_t bytes);
bool gpu_zero(GpuPtr to, size_t bytes);
/* `arguments` points at each argument in turn, as the driver's cuLaunchKernel() takes them. */
bool gpu_launch(void *function, unsigned blocks, unsigned threads, void **arguments);
bool gpu_wait(void);

/* Host memory the card can copy from while the host goes on, for gpu_upload_async(). */
void *gpu_host_alloc(size_t bytes);
bool gpu_upload_async(GpuPtr to, const void *from, size_t bytes);

const char *gpu_error(void);

#endif
