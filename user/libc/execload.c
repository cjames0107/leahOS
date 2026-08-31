/* Working out what running a program means.
 *
 * This is the loading half of dynamic linking; ld.so is the linking half. The
 * split is described in <loader.h>: the kernel does not know what an ELF is,
 * execve does, and loading a library is the same act as loading a program, so
 * whoever loads one loads the other.
 *
 * What has changed since that was first written is where the bytes live. They
 * used to be read here, every time, and handed to the kernel as a blob to copy
 * into fresh pages - so every process had its own private copy of libc and
 * every exec paid to read three hundred kilobytes it had read a hundred times
 * before. Now the kernel *holds* each file as an image, and this asks for one
 * by name before reading anything. On a hit nothing is read and nothing is
 * copied: the program's code is mapped from the image's own frames, which is
 * how libc comes to exist once in memory rather than once per process.
 *
 * The parsing therefore has to work on a file that is not in this address
 * space, which is what `struct source` and grab() are for. Everything else
 * here is the same ELF reading it always was.
 */

#include <errno.h>
#include <fcntl.h>
#include <loader.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

/* ELF, only what a loader reads. */
#define PT_LOAD     1
#define PT_DYNAMIC  2
#define PT_INTERP   3
#define PT_PHDR     6

#define ET_EXEC 2
#define ET_DYN  3

#define DT_NULL   0
#define DT_NEEDED 1
#define DT_STRTAB 5

#define PF_R 4

int __image_find(const char* name, unsigned long version)
{
    return (int)__syscall(SYS_imagefind, (long)name, (long)version, 0, 0, 0);
}

int __image_create(const char* name, unsigned long version, const void* bytes,
                   unsigned long size)
{
    return (int)__syscall(SYS_imagecreate, (long)name, (long)version,
                          (long)bytes, (long)size, 0);
}

int __image_read(int id, unsigned long offset, void* into, unsigned long bytes)
{
    return (int)__syscall(SYS_imageread, id, (long)offset, (long)into,
                          (long)bytes, 0);
}

/* --- a file, wherever it happens to be ------------------------------------ */

/* One object being loaded. `bytes` is set only when this exec had to read the
 * file; on a cache hit it is null and every read goes to the image. */
struct source {
    const unsigned char* bytes;
    long          size;
    int           image;
    unsigned long base;
    char          name[LOADER_NAME_MAX];
};

static int grab(const struct source* s, unsigned long off, void* into,
                unsigned long n)
{
    if (off + n > (unsigned long)s->size)
        return -1;
    if (s->bytes != 0) {
        memcpy(into, s->bytes + off, n);
        return 0;
    }
    return __image_read(s->image, off, into, n) == 0 ? 0 : -1;
}

static unsigned long u64_of(const struct source* s, unsigned long off)
{
    unsigned long v = 0;
    grab(s, off, &v, 8);
    return v;
}

static unsigned u32_of(const struct source* s, unsigned long off)
{
    unsigned v = 0;
    grab(s, off, &v, 4);
    return v;
}

static unsigned short u16_of(const struct source* s, unsigned long off)
{
    unsigned short v = 0;
    grab(s, off, &v, 2);
    return v;
}

/* A program header, copied out into the caller's own space. Returns 0 on
 * success; `into` is 56 bytes, which is what an entry is. */
static int phdr(const struct source* s, unsigned i, unsigned char* into)
{
    const unsigned long off = u64_of(s, 32);
    const unsigned short entsize = u16_of(s, 54);
    const unsigned short num = u16_of(s, 56);
    if (i >= num || entsize < 56)
        return -1;
    return grab(s, off + (unsigned long)i * entsize, into, 56);
}

static unsigned phdr_count(const struct source* s) { return u16_of(s, 56); }

static unsigned long ph_u64(const unsigned char* ph, unsigned long at)
{
    unsigned long v;
    memcpy(&v, ph + at, 8);
    return v;
}

static unsigned ph_u32(const unsigned char* ph, unsigned long at)
{
    unsigned v;
    memcpy(&v, ph + at, 4);
    return v;
}

/* A link-time address turned back into a file offset, by way of whichever
 * PT_LOAD contains it. The dynamic section records addresses, and reading one
 * out of a file that has not been mapped means undoing that. */
static long file_offset_of(const struct source* s, unsigned long vaddr)
{
    unsigned char ph[56];
    for (unsigned i = 0; i < phdr_count(s); ++i) {
        if (phdr(s, i, ph) != 0 || ph_u32(ph, 0) != PT_LOAD)
            continue;
        const unsigned long v = ph_u64(ph, 16), fs = ph_u64(ph, 32);
        if (vaddr >= v && vaddr < v + fs)
            return (long)(ph_u64(ph, 8) + (vaddr - v));
    }
    return -1;
}

/* --- getting hold of one -------------------------------------------------- */

/* An image of `path`, and the rights that came with it.
 *
 * Nothing is read here any more, and that is the point rather than an
 * optimisation. This used to stat the file, open it, read all of it and hand
 * the bytes to the kernel - which meant the only place execute permission
 * could be checked was here, in the process's own library, where the process
 * could simply not call it. The filesystem server issues the image now, having
 * seen the file and the caller's credentials together, and what comes back is
 * a capability that cannot be minted on this side.
 *
 * `running` separates the program from what it links against. A library is
 * mapped and read, never run: it needs no execute bit on disk and does not
 * receive the right to be a program.
 */
static int obtain(const char* path, int running, struct source* out)
{
    long size = 0;
    const int handle = __vfs_image(path, running, &size);
    if (handle < 0)
        return -1;
    out->bytes = 0;
    out->size = size;
    out->base = 0;
    out->image = handle;
    return 0;
}

/* The last component of a path, which is the name a library is known by. */
static void short_name(const char* path, char* out)
{
    const char* p = path;
    for (const char* q = path; *q != '\0'; ++q)
        if (*q == '/' && q[1] != '\0')
            p = q + 1;
    unsigned n = 0;
    while (p[n] != '\0' && n < LOADER_NAME_MAX - 1) {
        out[n] = p[n];
        ++n;
    }
    out[n] = '\0';
}

/* Every DT_NEEDED of one object, appended to the list if not already on it.
 *
 * A library named twice is loaded once. Two objects needing the same library
 * is the ordinary case, not an unusual one - it is the reason a shared library
 * is worth having. */
static int add_needed(const struct source* of, struct source* pieces,
                      int* count, int max)
{
    unsigned char ph[56];
    unsigned long dynoff = 0, dynsz = 0;
    for (unsigned i = 0; i < phdr_count(of); ++i) {
        if (phdr(of, i, ph) != 0 || ph_u32(ph, 0) != PT_DYNAMIC)
            continue;
        dynoff = ph_u64(ph, 8);
        dynsz = ph_u64(ph, 32);
        break;
    }
    if (dynsz == 0)
        return 0;                       /* nothing needs anything */
    if (dynsz > 4096)
        dynsz = 4096;                   /* far more entries than anything has */

    unsigned char dyn[4096];
    if (grab(of, dynoff, dyn, dynsz) != 0)
        return -1;

    long strtab = -1;
    for (unsigned long at = 0; at + 16 <= dynsz; at += 16)
        if (ph_u64(dyn, at) == DT_STRTAB) {
            strtab = file_offset_of(of, ph_u64(dyn, at + 8));
            break;
        }
    if (strtab < 0)
        return 0;

    for (unsigned long at = 0; at + 16 <= dynsz; at += 16) {
        const unsigned long tag = ph_u64(dyn, at);
        if (tag == DT_NULL)
            break;
        if (tag != DT_NEEDED)
            continue;

        char name[LOADER_NAME_MAX];
        const unsigned long at_name = (unsigned long)strtab + ph_u64(dyn, at + 8);
        unsigned n = 0;
        for (; n < sizeof(name) - 1; ++n) {
            if (grab(of, at_name + n, &name[n], 1) != 0 || name[n] == '\0')
                break;
        }
        name[n] = '\0';
        if (name[0] == '\0')
            continue;

        int already = 0;
        for (int i = 0; i < *count; ++i)
            if (strcmp(pieces[i].name, name) == 0)
                already = 1;
        if (already)
            continue;
        if (*count >= max) {
            errno = E2BIG;
            return -1;
        }

        char path[LOADER_NAME_MAX + 16];
        snprintf(path, sizeof(path), "%s/%s", LOADER_LIB_DIR, name);
        if (obtain(path, 0, &pieces[*count]) != 0)
            return -1;
        short_name(path, pieces[*count].name);
        ++*count;
    }
    return 0;
}

/* --- putting it together -------------------------------------------------- */

/* Copy one object's PT_LOADs into the request, with its base added. */
static int place(struct loader_request* r, const struct source* p,
                 unsigned long blob_at)
{
    unsigned char ph[56];
    for (unsigned i = 0; i < phdr_count(p); ++i) {
        if (phdr(p, i, ph) != 0 || ph_u32(ph, 0) != PT_LOAD)
            continue;
        const unsigned long memsz = ph_u64(ph, 40);
        if (memsz == 0)
            continue;
        const unsigned long filesz = ph_u64(ph, 32);
        const unsigned long offset = ph_u64(ph, 8);
        if (filesz > memsz || offset + filesz > (unsigned long)p->size) {
            errno = ENOEXEC;
            return -1;
        }
        if (r->count >= LOADER_MAX_SEGMENTS) {
            errno = E2BIG;
            return -1;
        }
        struct loader_segment* s = &r->segs[r->count++];
        s->vaddr  = p->base + ph_u64(ph, 16);
        s->filesz = filesz;
        s->memsz  = memsz;
        s->flags  = ph_u32(ph, 4);
        if (p->image >= 0) {
            s->image = p->image;
            s->offset = offset;
        } else {
            s->image = -1;
            s->offset = blob_at + offset;
        }
    }
    return 0;
}

static unsigned long dynamic_of(const struct source* p)
{
    unsigned char ph[56];
    for (unsigned i = 0; i < phdr_count(p); ++i)
        if (phdr(p, i, ph) == 0 && ph_u32(ph, 0) == PT_DYNAMIC)
            return p->base + ph_u64(ph, 16);
    return 0;
}

/* The auxiliary vector for this program. AT_PHDR comes from PT_PHDR rather
 * than from e_phoff, because the headers are only reachable at run time if
 * some segment actually maps them - and PT_PHDR is the header that says where
 * they landed. A program without one gets no AT_PHDR rather than a plausible
 * address that is not mapped. */
static void fill_aux(struct loader_request* r, const struct source* program)
{
    unsigned char ph[56];
    unsigned n = 0;

    for (unsigned i = 0; i < phdr_count(program); ++i) {
        if (phdr(program, i, ph) != 0 || ph_u32(ph, 0) != PT_PHDR)
            continue;
        r->aux[n++] = AT_PHDR;
        r->aux[n++] = program->base + ph_u64(ph, 16);
        break;
    }
    r->aux[n++] = AT_PHENT;  r->aux[n++] = 56;
    r->aux[n++] = AT_PHNUM;  r->aux[n++] = phdr_count(program);
    r->aux[n++] = AT_PAGESZ; r->aux[n++] = 4096;
    r->aux[n++] = AT_BASE;   r->aux[n++] = 0;   /* the interpreter is not moved */
    r->aux[n++] = AT_ENTRY;  r->aux[n++] = program->base + u64_of(program, 24);
    r->aux[n++] = AT_UID;    r->aux[n++] = (unsigned long)getuid();
    r->aux[n++] = AT_EUID;   r->aux[n++] = (unsigned long)getuid();
    r->aux[n++] = AT_GID;    r->aux[n++] = (unsigned long)getgid();
    r->aux[n++] = AT_EGID;   r->aux[n++] = (unsigned long)getgid();
    r->auxc = n;
}

int __loader_prepare(const char* path, struct loader_request* request,
                     void** blob, long* blob_size)
{
    request->count = 0;
    request->auxc = 0;
    request->program_image = -1;
    request->reserved_tail = 0;
    *blob = 0;
    *blob_size = 0;

    struct source pieces[LOADER_MAX_OBJECTS + 1];
    int count = 0;

    if (obtain(path, 1, &pieces[0]) != 0)
        return -1;
    {
        unsigned char ident[6];
        if (grab(&pieces[0], 0, ident, 6) != 0 || ident[0] != 0x7F ||
            ident[1] != 'E' || ident[2] != 'L' || ident[3] != 'F' ||
            ident[4] != 2 || ident[5] != 1) {
            errno = ENOEXEC;
            return -1;
        }
    }
    short_name("program", pieces[0].name);
    request->program_image = pieces[0].image;
    ++count;

    /* The interpreter, if it has one. A program with no PT_INTERP is complete
     * as it stands, and is loaded the way every program here used to be. */
    char interp[192];
    interp[0] = '\0';
    {
        unsigned char ph[56];
        for (unsigned i = 0; i < phdr_count(&pieces[0]); ++i) {
            if (phdr(&pieces[0], i, ph) != 0 || ph_u32(ph, 0) != PT_INTERP)
                continue;
            unsigned long len = ph_u64(ph, 32);
            if (len == 0 || len > sizeof(interp))
                break;
            if (grab(&pieces[0], ph_u64(ph, 8), interp, len) != 0)
                break;
            interp[len - 1] = '\0';
            break;
        }
    }

    if (interp[0] == '\0') {
        pieces[0].base = 0;
        if (place(request, &pieces[0], 0) != 0)
            return -1;
        if (request->count == 0) {
            errno = ENOEXEC;
            return -1;
        }
        request->entry = u64_of(&pieces[0], 24);
        fill_aux(request, &pieces[0]);
        return 0;
    }

    pieces[0].base =
        u16_of(&pieces[0], 16) == ET_DYN ? LOADER_BASE_PROGRAM : 0;

    if (obtain(interp, 0, &pieces[count]) != 0)
        return -1;
    /* The interpreter is linked at a fixed address and is the one object here
     * that is not moved. See user/ld.ld. */
    if (u16_of(&pieces[count], 16) != ET_EXEC) {
        errno = ENOEXEC;
        return -1;
    }
    pieces[count].base = 0;
    short_name(interp, pieces[count].name);
    const int interp_at = count;
    ++count;

    /* Breadth first from the program: everything it needs, then everything
     * those need. `count` grows underneath the loop, which is what makes this
     * the whole graph rather than one level of it. */
    for (int i = 0; i < count; ++i) {
        if (i == interp_at)
            continue;                   /* the interpreter needs nothing */
        if (add_needed(&pieces[i], pieces, &count, LOADER_MAX_OBJECTS + 1) != 0)
            return -1;
    }

    /* Where each library goes. The program and the interpreter already know. */
    unsigned long next = LOADER_BASE_LIBRARY;
    for (int i = 0; i < count; ++i) {
        if (i == 0 || i == interp_at)
            continue;
        pieces[i].base = next;
        next += LOADER_LIBRARY_STEP;
    }

    /* The table ld.so reads. It is different for every exec, so it is the one
     * thing still passed as bytes. */
    struct loader_table table;
    memset(&table, 0, sizeof(table));
    table.magic = LOADER_MAGIC;
    table.entry = pieces[0].base + u64_of(&pieces[0], 24);
    unsigned objects = 0;
    for (int i = 0; i < count; ++i) {
        if (i == interp_at)
            continue;                   /* nothing to relocate in it */
        if (objects >= LOADER_MAX_OBJECTS) {
            errno = E2BIG;
            return -1;
        }
        table.objects[objects].base = pieces[i].base;
        table.objects[objects].dynamic = dynamic_of(&pieces[i]);
        memcpy(table.objects[objects].name, pieces[i].name, LOADER_NAME_MAX);
        ++objects;
    }
    table.count = objects;

    /* The blob holds the table, and any object the cache would not take. On a
     * warm cache that is the table alone - a few hundred bytes, where an exec
     * used to hand over a megabyte. */
    long total = (long)sizeof(table);

    unsigned char* out = malloc((size_t)total);
    if (out == 0) {
        errno = ENOMEM;
        return -1;
    }

    long at = 0;
    for (int i = 0; i < count; ++i)
        if (place(request, &pieces[i], 0) != 0)
            return -1;

    memcpy(out + at, &table, sizeof(table));
    if (request->count >= LOADER_MAX_SEGMENTS) {
        errno = E2BIG;
        return -1;
    }
    struct loader_segment* s = &request->segs[request->count++];
    s->vaddr  = LOADER_TABLE_ADDR;
    s->offset = (unsigned long)at;
    s->filesz = sizeof(table);
    s->memsz  = sizeof(table);
    s->flags  = PF_R;
    s->image  = -1;

    /* Into the interpreter, not the program. It is the interpreter's job to
     * finish the program and then jump to it. */
    request->entry = u64_of(&pieces[interp_at], 24);
    fill_aux(request, &pieces[0]);

    *blob = out;
    *blob_size = total;
    return 0;
}
