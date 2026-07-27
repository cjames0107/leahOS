#ifndef _THREAD_H
#define _THREAD_H

/* Threads inside one process: they share the address space, the heap and the
 * open-file table, and differ only in their stack and registers. A thread is
 * also a child of its creator, so join() is wait() under the covers. */

typedef int tid_t;

/* Start `fn(arg)` on a new thread. Returns its tid, or -1. */
tid_t thread_create(void (*fn)(void*), void* arg);

/* Wait for any one thread (or child) to finish. Returns its tid, or -1. */
tid_t thread_join(void);

/* The calling thread's id. Distinct per thread, unlike getpid(). */
tid_t gettid(void);

/* End the calling thread without ending the process. */
void thread_exit(void);

#endif /* _THREAD_H */
