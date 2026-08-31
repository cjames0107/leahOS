/* ld.so - the dynamic linker.
 *
 * By the time this runs, everything is already in memory: execve read the
 * program, the interpreter and every library it named, and handed the kernel
 * one list of segments to map. See <loader.h> for why the reading happens
 * there and not here.
 *
 * So this is only the linking, which is the half that has to happen in the new
 * address space and could not have been done anywhere else. Two passes over
 * every object: apply the relocations that need no symbol, then the ones that
 * do, looking each name up across every object in load order - the program
 * first, which is what lets a program define a symbol a library will use.
 *
 * Relocations are all resolved before the program starts. The build passes
 * -z now, so there is no lazy binding, no resolver trampoline, and no writable
 * PLT to be tricked into pointing somewhere else. Lazy binding buys a faster
 * start for a program that never calls most of what it links against, and
 * nothing here is big enough for that to be worth the machinery.
 *
 * Nothing in this file may call libc. It is the thing that makes libc
 * reachable, so it has its own three system calls and its own string
 * routines, and it is linked at a fixed address as an ordinary executable -
 * an interpreter that needed relocating would have to relocate itself first,
 * and that bootstrap is a page of subtle assembly bought for nothing when
 * there is a spare address to put it at.
 */

#include <loader.h>

typedef unsigned long      u64;
typedef unsigned int       u32;
typedef unsigned short     u16;
typedef long               i64;

/* --- the three system calls this needs ------------------------------------ */

#define SYS_exit   0
#define SYS_write  1

static long sys(long number, long a, long b, long c)
{
    long result;
    register long r10 __asm__("r10") = c;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(number), "D"(a), "S"(b), "d"(c), "r"(r10)
                     : "rcx", "r11", "memory");
    return result;
}

static unsigned long slen(const char* s)
{
    unsigned long n = 0;
    while (s[n] != '\0')
        ++n;
    return n;
}

static void say(const char* text)
{
    sys(SYS_write, 2, (long)text, (long)slen(text));
}

/* A failure here is fatal and has to say why on its own: there is no program
 * yet to report it, and a silent exit looks exactly like the program running
 * and doing nothing. */
static void die(const char* what, const char* detail)
{
    say("ld.so: ");
    say(what);
    if (detail != 0) {
        say(": ");
        say(detail);
    }
    say("\n");
    sys(SYS_exit, 127, 0, 0);
    for (;;) { }
}

static int same(const char* a, const char* b)
{
    while (*a != '\0' && *a == *b) { ++a; ++b; }
    return *a == *b;
}

/* --- ELF, only the parts a linker reads ----------------------------------- */

#define DT_NULL     0
#define DT_HASH     4
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_RELA     7
#define DT_RELASZ   8
#define DT_RELAENT  9
#define DT_PLTRELSZ 2
#define DT_JMPREL   23
#define DT_PLTREL   20

#define R_X86_64_64        1
#define R_X86_64_COPY      5
#define R_X86_64_GLOB_DAT  6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE  8

#define SHN_UNDEF 0

struct dyn   { u64 tag; u64 value; };
struct rela  { u64 offset; u64 info; i64 addend; };
struct sym   { u32 name; unsigned char info, other; u16 shndx; u64 value, size; };

/* Everything about one object that a relocation needs. Gathered once, because
 * walking the dynamic array for each of a few hundred relocations is the kind
 * of thing that turns a linear cost into a quadratic one for no reason. */
struct object {
    u64          base;
    const char*  name;
    const char*  strtab;
    const struct sym* symtab;
    const u32*   hash;                  /* nbucket, nchain, buckets, chain */
    u64          symcount;
    const struct rela* rela;
    u64          relasz;
    const struct rela* jmprel;
    u64          jmprelsz;
};

static struct object g_objects[LOADER_MAX_OBJECTS];
static unsigned      g_count;

static void gather(struct object* o, u64 base, u64 dynamic, const char* name)
{
    const struct dyn* d = (const struct dyn*)dynamic;
    u64 hash = 0;

    o->base = base;
    o->name = name;
    o->strtab = 0;
    o->symtab = 0;
    o->hash = 0;
    o->symcount = 0;
    o->rela = 0;
    o->relasz = 0;
    o->jmprel = 0;
    o->jmprelsz = 0;

    for (; d->tag != DT_NULL; ++d) {
        /* The addresses in a dynamic entry are link-time ones, so every one of
         * them needs the base added. The sizes do not, which is the whole of
         * the difference between the two halves of this switch. */
        switch (d->tag) {
        case DT_HASH:     hash = base + d->value; break;
        case DT_STRTAB:   o->strtab = (const char*)(base + d->value); break;
        case DT_SYMTAB:   o->symtab = (const struct sym*)(base + d->value); break;
        case DT_RELA:     o->rela = (const struct rela*)(base + d->value); break;
        case DT_RELASZ:   o->relasz = d->value; break;
        case DT_JMPREL:   o->jmprel = (const struct rela*)(base + d->value); break;
        case DT_PLTRELSZ: o->jmprelsz = d->value; break;
        default: break;
        }
    }

    /* How many symbols there are is not in the dynamic array. It is the second
     * word of the hash table - nchain, which by construction is one entry per
     * symbol - and that is the only place it is written down. */
    if (hash != 0) {
        o->hash = (const u32*)hash;
        o->symcount = o->hash[1];
    }
}

/* The hash a SysV symbol table is indexed by. Defined by the ELF ABI down to
 * the shifts, so this is a transcription rather than a choice. */
static u32 elf_hash(const char* name)
{
    u32 h = 0;
    for (const unsigned char* p = (const unsigned char*)name; *p != 0; ++p) {
        h = (h << 4) + *p;
        const u32 high = h & 0xF0000000u;
        if (high != 0)
            h ^= high >> 24;
        h &= ~high;
    }
    return h;
}

/* The definition of `name` in one object, or 0.
 *
 * Through the hash table, which is the reason there is one. libc exports some
 * seven hundred symbols and a program's relocations ask about a few hundred
 * names; a scan per question is a hundred thousand string comparisons before
 * the program has run an instruction, and this is one or two. */
static u64 defines(const struct object* o, const char* name)
{
    if (o->hash == 0 || o->symtab == 0 || o->strtab == 0)
        return 0;
    const u32 nbucket = o->hash[0];
    if (nbucket == 0)
        return 0;
    const u32* bucket = o->hash + 2;
    const u32* chain = bucket + nbucket;

    for (u32 i = bucket[elf_hash(name) % nbucket]; i != 0; i = chain[i]) {
        if (i >= o->symcount)
            return 0;                   /* a chain that leaves the table */
        const struct sym* sy = &o->symtab[i];
        if (sy->shndx == SHN_UNDEF)
            continue;                   /* it wants one too, it has not got one */
        if (same(o->strtab + sy->name, name))
            return o->base + sy->value;
    }
    return 0;
}

/* The definition of `name`, searched in load order.
 *
 * Load order is the scope rule, not a detail: the program comes first, so a
 * program that defines a symbol a library also defines is the one that wins,
 * for itself and for the library. That is what makes it possible to replace
 * one function of a library without replacing the library.
 */
static u64 lookup(const char* name)
{
    for (unsigned i = 0; i < g_count; ++i) {
        const u64 value = defines(&g_objects[i], name);
        if (value != 0)
            return value;
    }
    return 0;
}

static void apply(const struct object* o, const struct rela* r, u64 bytes)
{
    if (r == 0 || bytes == 0)
        return;
    const u64 count = bytes / sizeof(struct rela);
    for (u64 i = 0; i < count; ++i) {
        u64* where = (u64*)(o->base + r[i].offset);
        const u32 type = (u32)(r[i].info & 0xFFFFFFFFu);
        const u64 index = r[i].info >> 32;

        if (type == R_X86_64_RELATIVE) {
            /* No symbol involved: a pointer that was written down as if the
             * object were at zero. This is most of them. */
            *where = o->base + (u64)r[i].addend;
            continue;
        }

        if (o->symtab == 0 || o->strtab == 0)
            die("relocation needs symbols and there are none", o->name);
        const char* name = o->strtab + o->symtab[index].name;
        const u64 value = lookup(name);
        if (value == 0) {
            /* A weak undefined symbol resolves to zero and that is an answer,
             * not a failure - it is how a program asks "is this present?". */
            if ((o->symtab[index].info >> 4) == 2) {
                *where = 0;
                continue;
            }
            die("undefined symbol", name);
        }

        switch (type) {
        case R_X86_64_JUMP_SLOT:
        case R_X86_64_GLOB_DAT:
            *where = value;
            break;
        case R_X86_64_64:
            *where = value + (u64)r[i].addend;
            break;
        case R_X86_64_COPY:
            /* Only an ET_EXEC needs these, and everything here is built
             * position-independent so that nothing does. Saying so beats
             * copying the wrong number of bytes quietly. */
            die("copy relocation in a position-independent object", o->name);
            break;
        default:
            die("relocation type this linker does not know", o->name);
            break;
        }
    }
}

/* Called from _start. Returns the address to jump to. */
u64 ld_main(void)
{
    const struct loader_table* t = (const struct loader_table*)LOADER_TABLE_ADDR;

    if (t->magic != LOADER_MAGIC)
        die("the loader table is not there", 0);
    if (t->count == 0 || t->count > LOADER_MAX_OBJECTS)
        die("the loader table is malformed", 0);

    g_count = (unsigned)t->count;
    for (unsigned i = 0; i < g_count; ++i)
        gather(&g_objects[i], t->objects[i].base, t->objects[i].dynamic,
               t->objects[i].name);

    /* Both relocation tables of every object. They are two tables for a reason
     * that no longer applies once binding is eager - .rela.plt is the one that
     * would have been done lazily - so they are treated the same. */
    for (unsigned i = 0; i < g_count; ++i) {
        apply(&g_objects[i], g_objects[i].rela, g_objects[i].relasz);
        apply(&g_objects[i], g_objects[i].jmprel, g_objects[i].jmprelsz);
    }

    return t->entry;
}
