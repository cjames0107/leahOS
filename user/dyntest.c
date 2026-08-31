/* dyntest - what the dynamic linker left behind.
 *
 * A reproducer, not a test suite: the self-tests fault in the threads section
 * on a machine whose every other program works, and running the whole suite to
 * see it costs a boot. This prints the things that would say why - where the
 * objects were placed, what mmap hands out, and whether the code of a libc
 * function is still there when it is called.
 */

#include <loader.h>
#include <stdio.h>
#include <string.h>
#include <proc.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <thread.h>
#include <sys/wait.h>
#include <unistd.h>

static void show(const char* when, const void* at)
{
    const unsigned char* p = (const unsigned char*)at;
    printf("%-22s %p:", when, at);
    for (int i = 0; i < 8; ++i)
        printf(" %02x", p[i]);
    printf("\n");
    fflush(stdout);
}

static volatile int g_smash_len = 96;

__attribute__((noinline)) static void smash(void)
{
    char small[8];
    for (int i = 0; i < g_smash_len; ++i)
        small[i] = (char)('A' + (i & 15));
    write(1, small, 1);
}

/* Every page of libc's code, looking for one that has turned into zeros. */
static void scan(const char* when, unsigned long base)
{
    const unsigned char* e = (const unsigned char*)base;
    unsigned long phoff;
    unsigned short phent, phnum;
    memcpy(&phoff, e + 32, 8);
    memcpy(&phent, e + 54, 2);
    memcpy(&phnum, e + 56, 2);
    for (unsigned short k = 0; k < phnum; ++k) {
        const unsigned char* ph = e + phoff + (unsigned long)k * phent;
        unsigned type, flags;
        unsigned long vaddr, filesz;
        memcpy(&type, ph, 4);
        memcpy(&flags, ph + 4, 4);
        memcpy(&vaddr, ph + 16, 8);
        memcpy(&filesz, ph + 32, 8);
        if (type != 1 || (flags & 1) == 0)
            continue;
        for (unsigned long at = 0; at + 4096 <= filesz; at += 4096) {
            const unsigned long* page = (const unsigned long*)(base + vaddr + at);
            unsigned long any = 0;
            for (int w = 0; w < 512; ++w)
                any |= page[w];
            if (any == 0) {
                printf("!! %s: libc code page %p is zero\n", when,
                       (void*)(base + vaddr + at));
                fflush(stdout);
                return;
            }
        }
    }
    printf("   %s: libc code intact\n", when);
    fflush(stdout);
}

static volatile int g_spun;

static void spin(void* arg)
{
    (void)arg;
    for (int i = 0; i < 1000; ++i)
        g_spun = g_spun + 1;
}

/* What a process actually costs in physical memory, measured rather than
 * reasoned about: start some children, see what the machine spent. */
static void cost_of_processes(int many)
{
    struct mem_info before, after;
    mem_info(&before);

    int kids[16];
    if (many > 16)
        many = 16;
    for (int i = 0; i < many; ++i) {
        kids[i] = fork();
        if (kids[i] == 0) {
            char* const args[] = { (char*)"sleep", (char*)"6", 0 };
            execve("/bin/sleep", args, 0);
            exit(1);
        }
    }
    msleep(1500);
    mem_info(&after);
    printf("memory: %lu KiB used by %d processes, %lu KiB each\n",
           (unsigned long)((after.used - before.used) / 1024), many,
           (unsigned long)((after.used - before.used) / 1024 / (unsigned)many));
    fflush(stdout);
    for (int i = 0; i < many; ++i)
        if (kids[i] > 0)
            wait(0);
}

int main(void)

{
    const struct loader_table* t = (const struct loader_table*)LOADER_TABLE_ADDR;
    printf("loader table magic %#lx, %lu objects, entry %#lx\n",
           t->magic, t->count, t->entry);
    for (unsigned long i = 0; i < t->count && i < LOADER_MAX_OBJECTS; ++i)
        printf("  %-12s base %#012lx dynamic %#012lx\n",
               t->objects[i].name, t->objects[i].base, t->objects[i].dynamic);

    /* libc's own code, read through the base the table gave. gettid lives a
     * long way into the text segment, which is the part that went missing. */
    const unsigned long libc = t->count > 1 ? t->objects[1].base : 0;
    show("libc+0", (const void*)libc);
    show("libc+0x2ae20", (const void*)(libc + 0x2ae20));

    const unsigned long len = 3 * 4096 + 100;
    char* m = mmap(0, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    printf("mmap(%lu) -> %p\n", len, (void*)m);
    fflush(stdout);
    if (m != MAP_FAILED) {
        for (unsigned long i = 0; i < len; ++i)
            m[i] = (char)(i * 7 + 3);
        show("after mmap writes", (const void*)(libc + 0x2ae20));
        char* q = mmap(0, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        printf("second mmap -> %p\n", (void*)q);
        fflush(stdout);
        munmap(m, len);
        if (q != MAP_FAILED)
            munmap(q, 4096);
        show("after munmap", (const void*)(libc + 0x2ae20));
    }

    /* Forking is the suspect: a fork shares every page copy-on-write, and a
     * child that exits or execs frees what it had. A frame freed while the
     * parent still has it mapped comes back from the next allocation as a page
     * of zeros - which is what the faulting program found where its code
     * should have been. */
    for (int i = 0; i < 8; ++i) {
        const int pid = fork();
        if (pid == 0)
            exit(0);
        if (pid > 0)
            wait(0);
    }
    show("after 8 forks", (const void*)(libc + 0x2ae20));

    for (int i = 0; i < 4; ++i) {
        const int pid = fork();
        if (pid == 0) {
            char* const args[] = { (char*)"true", 0 };
            execve("/bin/true", args, 0);
            exit(1);
        }
        if (pid > 0)
            wait(0);
    }
    show("after 4 execs", (const void*)(libc + 0x2ae20));

    /* Threads. The self-test suite is the only program in the system that
     * makes any, and the page that goes missing is always the same one - so
     * this is the one variable left untried. */
    for (int round = 0; round < 3; ++round) {
        tid_t made[4];
        for (int i = 0; i < 4; ++i)
            made[i] = thread_create(spin, 0);
        for (int i = 0; i < 4; ++i)
            if (made[i] >= 0)
                thread_join();
        show("after 4 threads", (const void*)(libc + 0x2ae20));
    }

    /* A child that dies from a smashed stack, which is the one thing the
     * self-tests do just before the page goes missing and the one thing this
     * had not tried. */
    for (int round = 0; round < 4; ++round) {
        const int pid = fork();
        if (pid == 0) {
            smash();
            exit(0);
        }
        if (pid > 0)
            wait(0);
        scan("after a smashed child", libc);
    }

    /* The auxiliary vector, walked the way anything else would find it: past
     * the environment's terminator. */
    {
        unsigned long* a = (unsigned long*)(void*)environ;
        while (*a != 0) ++a;
        ++a;
        int seen = 0;
        unsigned long entry = 0, phnum = 0, pagesz = 0, phdr = 0;
        for (; a[0] != 0 && seen < 32; a += 2, ++seen) {
            if (a[0] == 9)  entry  = a[1];
            if (a[0] == 5)  phnum  = a[1];
            if (a[0] == 6)  pagesz = a[1];
            if (a[0] == 3)  phdr   = a[1];
        }
        printf("auxv: %d entries, AT_ENTRY %lx AT_PHDR %lx AT_PHNUM %lu AT_PAGESZ %lu\n",
               seen, entry, phdr, phnum, pagesz);
        fflush(stdout);
    }

    cost_of_processes(12);

    printf("about to call gettid\n");
    fflush(stdout);
    printf("gettid = %d, getpid = %d\n", (int)gettid(), (int)getpid());
    fflush(stdout);
    return 0;
}
