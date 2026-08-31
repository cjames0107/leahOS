#ifndef _LOADER_H
#define _LOADER_H

/* The contract between execve and the dynamic linker.
 *
 * Every other system splits this differently. There, the kernel maps the
 * program and the interpreter, and the interpreter opens and maps the
 * libraries itself with mmap. Here the kernel does not know what an ELF is -
 * execve reads the program in userland and hands over a list of segments (see
 * user/libc/unistd.c, and the comment in kernel/proc/process.cpp that explains
 * why). Loading a library is the same act as loading a program, so the same
 * side does it: execve reads the interpreter and the libraries too, and hands
 * the kernel one list with all of them on it.
 *
 * That leaves ld.so with exactly one job - resolving symbols and applying
 * relocations - and it does that in the new address space, which is the only
 * place it can be done. It needs no mmap, no open, and no filesystem: by the
 * time it runs, everything it works on is already mapped.
 *
 * What it does need is to be told where everything landed, and this table is
 * that. execve writes it as one more segment at a fixed address, because the
 * two ends of this contract are built from the same tree and ship together.
 *
 * The addresses below are a map of the user half. Nothing is randomised: there
 * is no ASLR here yet, and pretending otherwise by scattering objects would
 * make a fault report harder to read without making anything harder to
 * attack.
 */

/* Where a position-independent executable is placed. The same 0x400000 that
 * static programs were linked at, so a fault address still reads the way it
 * always has. */
#define LOADER_BASE_PROGRAM  0x00400000UL

/* Libraries, 16 MiB apart. The step is generous because the cost of a gap in a
 * 128 TiB address space is nothing and the cost of two objects overlapping is
 * a fault in whichever one is unlucky. */
#define LOADER_BASE_LIBRARY  0x04000000UL
#define LOADER_LIBRARY_STEP  0x01000000UL

/* The interpreter, and the table describing what was loaded. Both below the
 * heap, which starts at 256 MiB (memory::kUserBrkBase). */
#define LOADER_BASE_INTERP   0x08000000UL
#define LOADER_TABLE_ADDR    0x08800000UL

#define LOADER_MAGIC        0x3144414FUL     /* "OAD1" */
#define LOADER_MAX_OBJECTS  8
#define LOADER_NAME_MAX     40

/* One loaded object. `dynamic` is the address of its PT_DYNAMIC with the base
 * already added, so ld.so never has to convert anything. */
struct loader_object {
    unsigned long base;
    unsigned long dynamic;
    char          name[LOADER_NAME_MAX];
};

/* objects[0] is always the program. The interpreter is not in the list: it is
 * linked at a fixed address and needs no relocation, which is the reason it is
 * the one object here that is not position-independent. */
struct loader_table {
    unsigned long magic;
    unsigned long count;
    unsigned long entry;                /* the program's entry, already based */
    unsigned long reserved;
    struct loader_object objects[LOADER_MAX_OBJECTS];
};

/* Where libraries are looked for. One directory, because a search path is a
 * thing to get wrong and there is nothing here that needs two. */
#define LOADER_LIB_DIR "/lib"
#define LOADER_INTERP  "/lib/ld.so"

/* --- what execve builds ----------------------------------------------------
 *
 * The kernel takes a list of segments and a blob of bytes for them to be cut
 * from, and maps what it is told. These are that request; the shapes have to
 * match kernel/proc/process.cpp exactly, because it copies them across the
 * boundary as bytes.
 */
#define LOADER_MAX_SEGMENTS 16

struct loader_segment {
    unsigned long vaddr;
    unsigned long offset;               /* into the image, or into the blob */
    unsigned long filesz;
    unsigned long memsz;
    unsigned      flags;                /* 1 execute, 2 write, 4 read */
    int           image;                /* which held image, or -1 */
};

/* The auxiliary vector, as type/value pairs, which the kernel copies onto the
 * new stack above the environment.
 *
 * The loader table below is this system's own way of telling ld.so where
 * things went, and it is simpler for that job. auxv is here as well because it
 * is what every program written anywhere else looks for - a language runtime
 * finding its own program headers, a libc wanting the page size - and none of
 * them will ever know about the table. Two mechanisms, because they have two
 * audiences. */
#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_PAGESZ 6
#define AT_BASE   7
#define AT_ENTRY  9
#define AT_UID    11
#define AT_EUID   12
#define AT_GID    13
#define AT_EGID   14

#define LOADER_MAX_AUX 32

struct loader_request {
    unsigned long entry;
    unsigned      count;
    unsigned      auxc;
    struct loader_segment segs[LOADER_MAX_SEGMENTS];
    unsigned long aux[LOADER_MAX_AUX];
    /* The handle on the program itself, last so that everything before it
     * stays where the kernel already reads it.
     *
     * Separate from the segments because "may be run" is a fact about the
     * program and not about a segment: a shared library's text is executable
     * memory in every process that maps it, and is not a program. The kernel
     * refuses the exec unless this handle carries the right to run. */
    int           program_image;
    int           reserved_tail;
};

/* Work out what running `path` means: its own segments, and - if it names an
 * interpreter - the interpreter's, every library it needs, and the table
 * telling ld.so where they all went.
 *
 * Each object is held by the kernel as an image (see <leah/image.hpp>), keyed
 * by its path and a version taken from its size and modification time. A
 * program that has been run before is not read at all: its headers are parsed
 * out of the pages the kernel already has, and its code is mapped from them
 * rather than copied. Only the loader table is passed as bytes, which is what
 * `blob` and `size` are for.
 *
 * Returns 0, or -1 with errno set.
 */
int __loader_prepare(const char* path, struct loader_request* request,
                     void** blob, long* blob_size);

/* The three calls that hold and read images. Thin wrappers on syscalls, here
 * rather than in a header of their own because nothing but the loader has any
 * business with them. */
int  __image_read(int handle, unsigned long offset, void* into,
                  unsigned long bytes);

/* An image of `path` from the filesystem server, and the rights that came with
 * it. See user/libc/fs.c. */
int  __vfs_image(const char* path, int running, long* size);

/* The path a descriptor names, or 0 for one that names no file. */
const char* __fd_path(int fd);

#endif /* _LOADER_H */
