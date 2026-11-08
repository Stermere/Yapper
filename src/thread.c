/* thread.c - Win32 / pthreads implementations of the thread.h shim. */
#include "thread.h"

#include <stdlib.h>
#include <time.h>

/* The search recurses, and a default worker stack differs by an order of magnitude
 * between platforms. */
#define THREAD_STACK_BYTES (8u * 1024 * 1024)

typedef struct {
    ThreadEntry fn;
    void *arg;
} ThreadStart;

#if defined(_WIN32)

static DWORD WINAPI thread_trampoline(LPVOID raw) {
    ThreadStart start = *(ThreadStart *)raw;
    free(raw);
    start.fn(start.arg);
    return 0;
}

bool thread_create(ThreadHandle *handle, ThreadEntry fn, void *arg) {
    ThreadStart *start = malloc(sizeof(*start));
    if (!start) {
        return false;
    }
    start->fn  = fn;
    start->arg = arg;
    *handle    = CreateThread(NULL, THREAD_STACK_BYTES, thread_trampoline, start, 0, NULL);
    if (!*handle) {
        free(start);
        return false;
    }
    return true;
}

void thread_join(ThreadHandle handle) {
    WaitForSingleObject(handle, INFINITE);
    CloseHandle(handle);
}

void thread_sleep_ms(int ms) { Sleep((DWORD)ms); }

int process_id(void) { return (int)GetCurrentProcessId(); }

#else

#include <unistd.h>

static void *thread_trampoline(void *raw) {
    ThreadStart start = *(ThreadStart *)raw;
    free(raw);
    start.fn(start.arg);
    return NULL;
}

bool thread_create(ThreadHandle *handle, ThreadEntry fn, void *arg) {
    ThreadStart *start = malloc(sizeof(*start));
    if (!start) {
        return false;
    }
    start->fn  = fn;
    start->arg = arg;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, THREAD_STACK_BYTES);
    const int rc = pthread_create(handle, &attr, thread_trampoline, start);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        free(start);
        return false;
    }
    return true;
}

void thread_join(ThreadHandle handle) { pthread_join(handle, NULL); }

void thread_sleep_ms(int ms) {
    const struct timespec pause = {ms / 1000, (long)(ms % 1000) * 1000000};
    nanosleep(&pause, NULL);
}

int process_id(void) { return (int)getpid(); }

#endif
