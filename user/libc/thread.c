#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <thread.h>
#include <unistd.h>

#define THREAD_STACK (64 * 1024)

/* What the new thread is handed: the clone syscall only passes one register, so
 * the function and its argument travel together in a struct parked at the base
 * of the thread's own stack mapping. */
struct thread_start {
    void (*fn)(void*);
    void* arg;
};

/* The thread's real entry point. clone() starts execution here with the struct
 * in rdi; when fn returns there is nowhere to return to, so the thread exits
 * rather than falling off the end of its stack. */
static void thread_trampoline(struct thread_start* start)
{
    start->fn(start->arg);
    thread_exit();
}

tid_t thread_create(void (*fn)(void*), void* arg)
{
    /* The stack is the caller's to provide, so it is an ordinary anonymous
     * mapping. The start struct sits at the low end and the stack grows down
     * from the top, so they only meet if the thread overflows its stack. */
    char* stack = mmap(0, THREAD_STACK, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED)
        return -1;

    struct thread_start* start = (struct thread_start*)stack;
    start->fn = fn;
    start->arg = arg;

    long tid = __syscall(SYS_clone, (long)thread_trampoline, (long)start,
                         (long)(stack + THREAD_STACK), 0, 0);
    if (tid < 0) {
        munmap(stack, THREAD_STACK);
        return -1;
    }
    return (tid_t)tid;
}

tid_t thread_join(void)
{
    return (tid_t)wait(0);
}

tid_t gettid(void)
{
    return (tid_t)__syscall(SYS_gettid, 0, 0, 0, 0, 0);
}

void thread_exit(void)
{
    /* SYS_exit ends the calling task, which for a thread means just this
     * thread; the process lives on until its last thread is gone. */
    __syscall(SYS_exit, 0, 0, 0, 0, 0);
}
