#include <sys/syscall.h>
#include <thread.h>

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

int futex_wait(volatile int* addr, int expected)
{
    return (int)__syscall(SYS_futex, (long)addr, FUTEX_WAIT, expected, 0, 0);
}

int futex_wake(volatile int* addr, int count)
{
    return (int)__syscall(SYS_futex, (long)addr, FUTEX_WAKE, count, 0, 0);
}

/* Compare-and-swap on the lock word, returning what was actually there. */
static int cas(volatile int* word, int expected, int desired)
{
    int seen = expected;
    __atomic_compare_exchange_n(word, &seen, desired, 0,
                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return seen;
}

/* The three-state mutex. The middle state matters: a lock released from state 1
 * knows nobody is waiting and can skip the futex_wake syscall entirely, so an
 * uncontended lock/unlock pair is two atomics and no kernel entry. State 2 is
 * what records "someone is asleep on this, you must wake them". */
void mutex_lock(mutex_t* m)
{
    int c = cas(&m->state, 0, 1);
    if (c == 0)
        return;                     /* was free: taken, uncontended */

    do {
        /* Mark it contended before sleeping, so whoever unlocks knows to wake
         * us. If it went free in the meantime, cas tells us and we retry. */
        if (c == 2 || cas(&m->state, 1, 2) != 0)
            futex_wait(&m->state, 2);
        /* Re-acquire as contended: there may be other sleepers behind us. */
        c = cas(&m->state, 0, 2);
    } while (c != 0);
}

int mutex_trylock(mutex_t* m)
{
    return cas(&m->state, 0, 1) == 0 ? 0 : -1;
}

void mutex_unlock(mutex_t* m)
{
    /* Dropping from 1 means there were no waiters, so no syscall is needed. */
    if (__atomic_fetch_sub(&m->state, 1, __ATOMIC_SEQ_CST) != 1) {
        __atomic_store_n(&m->state, 0, __ATOMIC_SEQ_CST);
        futex_wake(&m->state, 1);
    }
}
