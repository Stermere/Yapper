/* thread.h - Win32 threads or pthreads. Not C11 <threads.h>: MinGW-w64 lacks it. */
#ifndef THREAD_H
#define THREAD_H

#include <stdbool.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
typedef HANDLE ThreadHandle;
#else
#include <pthread.h>
typedef pthread_t ThreadHandle;
#endif

typedef void (*ThreadEntry)(void *);

/* False if the thread could not be created, leaving *handle unspecified. */
bool thread_create(ThreadHandle *handle, ThreadEntry fn, void *arg);

void thread_join(ThreadHandle handle);

/* For waiting on an external event, never inside the search. */
void thread_sleep_ms(int ms);

#endif
