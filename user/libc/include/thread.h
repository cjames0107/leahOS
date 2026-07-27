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

/* --- synchronisation ------------------------------------------------------
 *
 * A futex-backed mutex: the uncontended paths are pure atomics in userland and
 * never enter the kernel, and only a thread that actually has to wait pays for
 * a syscall. Static initialisation to all-zero is a valid unlocked mutex. */

typedef struct {
    volatile int state;     /* 0 unlocked, 1 locked, 2 locked and contended */
} mutex_t;

#define MUTEX_INIT { 0 }

void mutex_lock(mutex_t* m);
void mutex_unlock(mutex_t* m);
int  mutex_trylock(mutex_t* m);     /* 0 on success, -1 if already held */

/* The raw operations, for building other primitives. futex_wait returns
 * immediately if *addr no longer equals `expected`. */
int futex_wait(volatile int* addr, int expected);
int futex_wake(volatile int* addr, int count);

#endif /* _THREAD_H */
