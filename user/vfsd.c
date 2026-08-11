/* vfsd - the filesystem, outside the kernel.
 *
 * It reads ext4 and owns no hardware: sectors come from blockd, which owns the
 * disk. So a path lookup is this process asking that one, which asks the
 * drive - and none of the three can reach another's memory.
 *
 * Writing is the half that can lose data rather than merely fail, so it went in
 * after the read path had been proven rather than beside it. The filesystem is
 * made without metadata checksums and without the 64-bit feature, which is what
 * makes this tractable: a descriptor is 32 bytes, and updating a bitmap does
 * not mean recomputing a checksum over it as well.
 */

#include <blk.h>
#include <ipc.h>
#include <shm.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <proc.h>
#include <stdlib.h>
#include <vfsd.h>

/* --- the disk, one level down --------------------------------------------- */

static int g_blk;
static struct blk_shared* g_sectors;

static int disk_read(unsigned long lba, unsigned count, unsigned dev)
{
    struct ipc_message q, a;
    memset(&q, 0, sizeof(q));
    memset(&a, 0, sizeof(a));
    q.tag = BLK_READ;
    q.word[0] = (long)lba;
    q.word[1] = (long)count;
    q.word[2] = (long)dev;
    if (ipc_call(g_blk, &q, &a) != 0 || a.word[0] != 0)
        return -1;
    return 0;
}

/* --- ext4, as much of it as reading needs --------------------------------- */

/* --- one filesystem, of which there may be several ---------------------------
 *
 * Everything below used to be a global, which said there was one filesystem
 * and there always would be. The block size, the inode size, the group
 * descriptors, where the journal is - all of it is a property of a particular
 * disk, and a second disk means a second set rather than a second guess.
 *
 * They are gathered here and reached through a pointer to whichever mount the
 * path being resolved belongs to. The names are kept, as macros, so that the
 * two hundred places that read them did not all have to be edited into
 * something that says the same thing at greater length - and so that the
 * change that matters is visible instead of buried in the noise of that.
 */
struct fs {
    int      used;
    char     at[64];            /* where it is mounted */
    unsigned dev;               /* which disk, once the driver can say */

    unsigned block_size;
    unsigned inodes_per_group, inode_size, desc_size;
    unsigned first_data_block;
    unsigned blocks_per_group, group_count;
    unsigned long gd_block;
    unsigned char sb[1024];     /* the superblock, kept in memory */
    int      mounted;

    /* The journal is the disk's too. Its inode sits in a parallel array
     * because struct inode is not declared until further down, and the block
     * reader above here needs the block size from this struct. */
    int      has_journal;
    unsigned jrnl_first, jrnl_maxlen, jrnl_seq, jrnl_start;
};

#define FS_MAX 4
static struct fs  g_fs[FS_MAX];
static struct fs* g_cur = &g_fs[0];

#define g_block_size       (g_cur->block_size)
#define g_inodes_per_group (g_cur->inodes_per_group)
#define g_inode_size       (g_cur->inode_size)
#define g_desc_size        (g_cur->desc_size)
#define g_first_data_block (g_cur->first_data_block)
#define g_blocks_per_group (g_cur->blocks_per_group)
#define g_group_count      (g_cur->group_count)
#define g_gd_block         (g_cur->gd_block)
#define g_sb               (g_cur->sb)
#define g_mounted          (g_cur->mounted)
#define g_journal          (g_journal_inode[(unsigned)(g_cur - g_fs)])
#define g_has_journal      (g_cur->has_journal)
#define g_jrnl_first       (g_cur->jrnl_first)
#define g_jrnl_maxlen      (g_cur->jrnl_maxlen)
#define g_jrnl_seq         (g_cur->jrnl_seq)
#define g_jrnl_start       (g_cur->jrnl_start)

static unsigned rd16(const unsigned char* p, unsigned at)
{
    return (unsigned)p[at] | ((unsigned)p[at + 1] << 8);
}
static unsigned rd32(const unsigned char* p, unsigned at)
{
    return (unsigned)p[at] | ((unsigned)p[at + 1] << 8) |
           ((unsigned)p[at + 2] << 16) | ((unsigned)p[at + 3] << 24);
}

/* One filesystem block, wherever it is. Blocks are up to four sectors here,
 * which the transfer buffer has room for many times over. */
static unsigned char g_block[8192];

static int txn_peek(unsigned long block, unsigned char* out);

/* --- the block cache ---------------------------------------------------------
 *
 * Every read used to be a message to the disk server and a wait for the reply,
 * and the same handful of blocks were asked for over and over: the group
 * descriptors on every allocation, an inode table block on every stat, the
 * directory being listed once per entry in it. Walking a directory of thirty
 * files read the same block thirty-one times, each one a context switch out to
 * a driver doing programmed I/O a sector at a time.
 *
 * Direct-mapped rather than associative, and write-through rather than
 * write-back. Both are the unclever choice on purpose: a direct-mapped cache
 * needs no replacement policy and no bookkeeping on a hit - the fast path is a
 * comparison and a memcpy - and writing through means the disk is never behind
 * the cache, so the journal's ordering is exactly what it was and nothing here
 * can lose data that was already promised. The one thing that would be wrong
 * is a stale entry, and there is a single place a block is written.
 */
/* Sized from the machine rather than written down.
 *
 * Five hundred and twelve blocks was two megabytes whatever the machine had,
 * which is a lot on a small one and nothing on a large one. A sixteenth of
 * usable memory is a share rather than a number: it leaves the disk cache in
 * proportion to everything else that has to fit, and it costs nothing on a
 * machine that does not have the memory to spare because there is none to
 * take. Bounded at both ends - below, because a cache too small to hold a
 * directory and its inode table would thrash; above, because past a point the
 * hit rate stops moving and the memory is better spent on programs. */
static unsigned char* g_cache;          /* slots * block size, one run       */
static unsigned long* g_cache_block;
static unsigned*      g_cache_dev;
static unsigned char* g_cache_full;
static unsigned       g_cache_slots;

static void cache_init(void)
{
    if (g_cache_slots != 0 || g_block_size == 0)
        return;

    struct mem_info mem;
    unsigned long bytes = mem_info(&mem) == 0 ? (unsigned long)(mem.usable / 16)
                                              : (1ul << 20);
    if (bytes < (1ul << 20))  bytes = 1ul << 20;    /* a megabyte at least */
    if (bytes > (24ul << 20)) bytes = 24ul << 20;   /* and no more than this */

    const unsigned slots = (unsigned)(bytes / g_block_size);
    if (slots == 0)
        return;

    g_cache       = (unsigned char*)malloc((size_t)slots * g_block_size);
    g_cache_block = (unsigned long*)malloc((size_t)slots * sizeof(unsigned long));
    g_cache_dev   = (unsigned*)malloc((size_t)slots * sizeof(unsigned));
    g_cache_full  = (unsigned char*)malloc(slots);
    if (g_cache == 0 || g_cache_block == 0 || g_cache_dev == 0 ||
        g_cache_full == 0) {
        /* Without it everything still works, just slower - so this is a note
         * rather than a failure to start. */
        g_cache_slots = 0;
        printf("vfsd: no memory for a block cache; reading straight through\n");
        return;
    }
    memset(g_cache_full, 0, slots);
    g_cache_slots = slots;
    printf("vfsd: block cache, %u blocks (%lu KiB)\n",
           slots, (unsigned long)(slots * g_block_size) / 1024);
}

static unsigned cache_slot(unsigned long block, unsigned dev)
{
    /* The low bits of the block number, mixed with the device so that two
     * disks do not collide on every slot at once. */
    return (unsigned)((block ^ ((unsigned long)dev << 9)) % g_cache_slots);
}

static void cache_put(unsigned long block, unsigned dev,
                      const unsigned char* data)
{
    if (g_cache_slots == 0)
        return;
    const unsigned i = cache_slot(block, dev);
    memcpy(g_cache + (size_t)i * g_block_size, data, g_block_size);
    g_cache_block[i] = block;
    g_cache_dev[i] = dev;
    g_cache_full[i] = 1;
}

static int cache_get(unsigned long block, unsigned dev, unsigned char* out)
{
    if (g_cache_slots == 0)
        return 0;
    const unsigned i = cache_slot(block, dev);
    if (!g_cache_full[i] || g_cache_block[i] != block || g_cache_dev[i] != dev)
        return 0;
    memcpy(out, g_cache + (size_t)i * g_block_size, g_block_size);
    return 1;
}

static int read_block(unsigned long block, unsigned char* out)
{
    if (txn_peek(block, out))
        return 0;
    if (cache_get(block, g_cur->dev, out))
        return 0;
    /* One block, not a run of them. Reading eight at a time to save seven
     * round trips measured slower - 13.5 seconds to boot against 8.5 - and the
     * reason is that this driver moves data with programmed I/O a word at a
     * time, so the bytes cost more than the messages do. Trading round trips
     * for bytes is the wrong way round on this machine. */
    const unsigned sectors = g_block_size / BLK_SECTOR;
    if (disk_read(block * sectors, sectors, g_cur->dev) != 0)
        return -1;
    memcpy(out, g_sectors->data, g_block_size);
    cache_put(block, g_cur->dev, out);
    return 0;
}

struct inode {
    unsigned mode;
    unsigned uid, gid;
    unsigned long size;
    /* Seconds since 1970, as ext4 stores them: three 32-bit fields for when
     * the contents were last read, when the inode last changed, and when the
     * contents were last written. Every file on this filesystem had all three
     * at zero until there was a clock to fill them from. */
    unsigned atime, ctime, mtime;
    unsigned char block[60];
};

static struct inode g_journal_inode[FS_MAX];

static int read_inode(unsigned number, struct inode* out)
{
    if (number == 0 || g_inodes_per_group == 0)
        return -1;
    const unsigned group = (number - 1) / g_inodes_per_group;
    const unsigned index = (number - 1) % g_inodes_per_group;

    /* The group descriptor table follows the superblock's block. */
    static unsigned char desc[64];
    const unsigned long table = g_first_data_block + 1;
    const unsigned long byte = (unsigned long)group * g_desc_size;
    if (read_block(table + byte / g_block_size, g_block) != 0)
        return -1;
    memcpy(desc, g_block + byte % g_block_size, g_desc_size);

    unsigned long inode_table = rd32(desc, 8);
    if (g_desc_size >= 64)
        inode_table |= (unsigned long)rd32(desc, 40) << 32;

    const unsigned long ibyte = (unsigned long)index * g_inode_size;
    if (read_block(inode_table + ibyte / g_block_size, g_block) != 0)
        return -1;
    const unsigned char* raw = g_block + ibyte % g_block_size;

    out->mode = rd16(raw, 0);
    out->uid  = rd16(raw, 2);
    out->gid  = rd16(raw, 24);
    out->size = (unsigned long)rd32(raw, 4) |
                ((unsigned long)rd32(raw, 108) << 32);
    out->atime = rd32(raw, 8);
    out->ctime = rd32(raw, 12);
    out->mtime = rd32(raw, 16);
    memcpy(out->block, raw + 40, 60);
    return 0;
}

/* Where a file's Nth block lives. Extents only: every filesystem this system
 * makes has them, and the older indirect-block scheme is a second mapping to
 * keep right for images nothing here produces. */
static unsigned long map_block(const struct inode* in, unsigned long file_block)
{
    unsigned char node[8192];
    memcpy(node, in->block, 60);
    unsigned depth_guard = 0;

    for (;;) {
        if (rd16(node, 0) != 0xF30A || ++depth_guard > 8)
            return 0;
        const unsigned entries = rd16(node, 2);
        const unsigned depth = rd16(node, 6);

        if (depth == 0) {
            for (unsigned e = 0; e < entries; ++e) {
                const unsigned char* ent = node + 12 + e * 12;
                const unsigned first = rd32(ent, 0);
                unsigned len = rd16(ent, 4);
                if (len > 32768) len -= 32768;      /* not yet written to */
                if (file_block >= first && file_block < first + len) {
                    const unsigned long start =
                        ((unsigned long)rd16(ent, 6) << 32) | rd32(ent, 8);
                    return start + (file_block - first);
                }
            }
            return 0;
        }

        /* Interior: the last child whose first block is not past ours. */
        unsigned long child = 0;
        int found = 0;
        for (unsigned e = 0; e < entries; ++e) {
            const unsigned char* ent = node + 12 + e * 12;
            if (rd32(ent, 0) <= file_block) {
                child = rd32(ent, 4) | ((unsigned long)rd16(ent, 8) << 32);
                found = 1;
            } else {
                break;
            }
        }
        if (!found || read_block(child, node) != 0)
            return 0;
    }
}

/* --- writing --------------------------------------------------------------
 *
 * Every one of these has to leave the filesystem consistent on its own,
 * because there is no journal here: a counter updated without the bitmap it
 * describes is a filesystem that fsck complains about, and the order things
 * are written in is the only thing standing between the two.
 */



static void wr16w(unsigned char* p, unsigned at, unsigned v)
{
    p[at] = (unsigned char)v; p[at + 1] = (unsigned char)(v >> 8);
}
static void wr32w(unsigned char* p, unsigned at, unsigned v)
{
    p[at] = (unsigned char)v;         p[at + 1] = (unsigned char)(v >> 8);
    p[at + 2] = (unsigned char)(v >> 16); p[at + 3] = (unsigned char)(v >> 24);
}

static int disk_write(unsigned long lba, unsigned count, unsigned dev)
{
    struct ipc_message q, a;
    memset(&q, 0, sizeof(q));
    memset(&a, 0, sizeof(a));
    q.tag = BLK_WRITE;
    q.word[0] = (long)lba;
    q.word[1] = (long)count;
    q.word[2] = (long)dev;
    if (ipc_call(g_blk, &q, &a) != 0 || a.word[0] != 0)
        return -1;
    return 0;
}

/* --- transactions ------------------------------------------------------------
 *
 * A change to the tree is never one block. Creating a file touches the inode
 * bitmap, the inode table, the directory that names it, the group descriptor
 * and the superblock's free counts, and a crash between any two of those
 * leaves a filesystem that is not merely out of date but wrong: an inode
 * marked used that nothing names, or a name pointing at an inode that was
 * never written.
 *
 * So metadata writes are collected instead of issued. At the end of the
 * request they are written to the journal, then a commit block is written
 * after them, and only then are they written where they belong. A crash
 * before the commit block leaves a transaction recovery discards; a crash
 * after it leaves one recovery finishes. There is no third case, which is the
 * entire point.
 *
 * Reads have to see the collection too. Half of these writes are read back
 * within the same request - set a bit in the bitmap, then read the bitmap
 * again to find the next free one - and a read that went to the disk would
 * get the version from before the change.
 */
/* Deep enough that a batch holds several operations. Metadata repeats
 * heavily between them - the same bitmap, the same group descriptor, the same
 * directory block - and a repeat replaces its earlier copy rather than adding
 * to it, so this is many more operations than it looks like. */
#define TXN_MAX 32

/* How long a batch may stay open, and how much room is kept for the request
 * that would otherwise overflow it. */
#define TXN_MAX_AGE_MS 1000
#define TXN_HEADROOM   10

static int           g_txn_open;
static int           g_txn_bypass;   /* the journal's own writes */
static unsigned      g_txn_count;
static unsigned long g_txn_started;
static unsigned long g_txn_target[TXN_MAX];
static unsigned char g_txn_buf[TXN_MAX][8192];

static int txn_flush(void);

static int write_block(unsigned long block, const unsigned char* in)
{
    if (g_txn_open && !g_txn_bypass) {
        for (unsigned i = 0; i < g_txn_count; ++i)
            if (g_txn_target[i] == block) {
                memcpy(g_txn_buf[i], in, g_block_size);
                return 0;
            }
        /* A transaction that will not fit is split rather than dropped. The
         * halves are each atomic, which is weaker than one atomic whole but
         * is not the same as losing the guarantee. */
        if (g_txn_count == TXN_MAX && txn_flush() != 0)
            return -1;
        g_txn_target[g_txn_count] = block;
        memcpy(g_txn_buf[g_txn_count], in, g_block_size);
        ++g_txn_count;
        return 0;
    }
    const unsigned sectors = g_block_size / BLK_SECTOR;
    memcpy(g_sectors->data, in, g_block_size);
    /* Write-through: the cache is updated with what the disk is being told,
     * so a later read cannot see the version from before this. */
    cache_put(block, g_cur->dev, in);
    return disk_write(block * sectors, sectors, g_cur->dev);
}

/* Whatever the transaction is holding for this block, if it holds one. */
static int txn_peek(unsigned long block, unsigned char* out)
{
    if (!g_txn_open || g_txn_bypass)
        return 0;
    for (unsigned i = 0; i < g_txn_count; ++i)
        if (g_txn_target[i] == block) {
            memcpy(out, g_txn_buf[i], g_block_size);
            return 1;
        }
    return 0;
}

/* The superblock lives 1024 bytes into the volume, which for every block size
 * this system uses means inside a block that also holds other things - so it
 * is always a read, a patch and a write back. */
static int write_superblock(void)
{
    static unsigned char whole[8192];
    const unsigned long block = 1024 / g_block_size;
    const unsigned within = 1024 % g_block_size;
    if (read_block(block, whole) != 0)
        return -1;
    memcpy(whole + within, g_sb, 1024);
    return write_block(block, whole);
}

static int read_group_desc(unsigned group, unsigned char* desc)
{
    const unsigned long byte = (unsigned long)group * g_desc_size;
    if (read_block(g_gd_block + byte / g_block_size, g_block) != 0)
        return -1;
    memcpy(desc, g_block + byte % g_block_size, g_desc_size);
    return 0;
}

static int write_group_desc(unsigned group, const unsigned char* desc)
{
    const unsigned long byte = (unsigned long)group * g_desc_size;
    const unsigned long block = g_gd_block + byte / g_block_size;
    if (read_block(block, g_block) != 0)
        return -1;
    memcpy(g_block + byte % g_block_size, desc, g_desc_size);
    return write_block(block, g_block);
}

/* Take the first clear bit in some group's bitmap, and keep the three places
 * that count free space in step with it. */
static unsigned long alloc_from(unsigned long bitmap_block, unsigned limit,
                                unsigned desc_free_at, unsigned sb_free_at,
                                unsigned group, unsigned char* desc)
{
    static unsigned char bm[8192];
    if (read_block(bitmap_block, bm) != 0)
        return 0;
    for (unsigned bit = 0; bit < limit; ++bit) {
        if (bm[bit >> 3] & (1u << (bit & 7)))
            continue;
        bm[bit >> 3] |= (unsigned char)(1u << (bit & 7));
        if (write_block(bitmap_block, bm) != 0)
            return 0;
        wr16w(desc, desc_free_at, rd16(desc, desc_free_at) - 1);
        write_group_desc(group, desc);
        wr32w(g_sb, sb_free_at, rd32(g_sb, sb_free_at) - 1);
        write_superblock();
        return bit + 1;                 /* +1 so zero can mean failure */
    }
    return 0;
}

static unsigned long alloc_block(void)
{
    for (unsigned g = 0; g < g_group_count; ++g) {
        unsigned char desc[64];
        if (read_group_desc(g, desc) != 0 || rd16(desc, 12) == 0)
            continue;
        const unsigned long got = alloc_from(rd32(desc, 0), g_blocks_per_group,
                                             12, 12, g, desc);
        if (got != 0)
            return g_first_data_block +
                   (unsigned long)g * g_blocks_per_group + (got - 1);
    }
    return 0;
}

static unsigned alloc_inode(int is_dir)
{
    for (unsigned g = 0; g < g_group_count; ++g) {
        unsigned char desc[64];
        if (read_group_desc(g, desc) != 0 || rd16(desc, 14) == 0)
            continue;
        const unsigned long got = alloc_from(rd32(desc, 4), g_inodes_per_group,
                                             14, 16, g, desc);
        if (got == 0)
            continue;
        if (is_dir) {
            /* Directories are counted separately, at offset 16 - which is the
             * field after free inodes and before two bytes of padding. Writing
             * to 18 instead puts the count in the padding, where nothing reads
             * it and fsck reports the real one as wrong. */
            if (read_group_desc(g, desc) == 0) {
                wr16w(desc, 16, rd16(desc, 16) + 1);
                write_group_desc(g, desc);
            }
        }
        return g * g_inodes_per_group + (unsigned)got;
    }
    return 0;
}

static void free_block(unsigned long block)
{
    if (block < g_first_data_block)
        return;
    const unsigned long rel = block - g_first_data_block;
    const unsigned g = (unsigned)(rel / g_blocks_per_group);
    const unsigned bit = (unsigned)(rel % g_blocks_per_group);
    unsigned char desc[64];
    if (g >= g_group_count || read_group_desc(g, desc) != 0)
        return;
    static unsigned char bm[8192];
    const unsigned long bitmap = rd32(desc, 0);
    if (read_block(bitmap, bm) != 0)
        return;
    if ((bm[bit >> 3] & (1u << (bit & 7))) == 0)
        return;                         /* already free; do not double-count */
    bm[bit >> 3] &= (unsigned char)~(1u << (bit & 7));
    write_block(bitmap, bm);
    wr16w(desc, 12, rd16(desc, 12) + 1);
    write_group_desc(g, desc);
    wr32w(g_sb, 12, rd32(g_sb, 12) + 1);
    write_superblock();
}

/* Where an inode's 128 or 256 bytes live, so it can be written back. */
static int inode_location(unsigned number, unsigned long* block_out,
                          unsigned* offset_out)
{
    if (number == 0 || g_inodes_per_group == 0)
        return -1;
    const unsigned group = (number - 1) / g_inodes_per_group;
    const unsigned index = (number - 1) % g_inodes_per_group;
    unsigned char desc[64];
    if (read_group_desc(group, desc) != 0)
        return -1;
    const unsigned long table = rd32(desc, 8);
    const unsigned long byte = (unsigned long)index * g_inode_size;
    *block_out = table + byte / g_block_size;
    *offset_out = (unsigned)(byte % g_block_size);
    return 0;
}

/* What an inode's link count is right now, for the callers that are changing
 * something else about it and must not disturb that. */
static unsigned inode_links(unsigned number)
{
    unsigned long block; unsigned at;
    if (inode_location(number, &block, &at) != 0 ||
        read_block(block, g_block) != 0)
        return 1;
    return rd16(g_block, at + 26);
}

static unsigned dir_links(unsigned number) { return inode_links(number); }

static int write_inode(unsigned number, const struct inode* in, unsigned links)
{
    unsigned long block; unsigned at;
    if (inode_location(number, &block, &at) != 0 ||
        read_block(block, g_block) != 0)
        return -1;
    /* The inode's fields, at the offsets ext4 puts them: mode at 0, size split
     * between 4 and 108, links at 26, i_blocks at 28 in 512-byte units
     * whatever the block size is, flags at 32, and the 60 bytes of extent root
     * at 40. Getting i_blocks or the flags a few bytes out writes over the
     * extent header, which is a file that reads as empty and an fsck that
     * says so. */
    unsigned char* raw = g_block + at;
    wr16w(raw, 0, in->mode);
    wr16w(raw, 2, in->uid);
    wr16w(raw, 24, in->gid);
    wr32w(raw, 4, (unsigned)in->size);
    wr32w(raw, 108, (unsigned)(in->size >> 32));
    /* Always written, including zero. A removed inode that keeps its old link
     * count is one fsck finds in use with nothing pointing at it - the bitmap
     * bit being clear is not enough on its own. */
    wr16w(raw, 26, links);
    const unsigned used = (unsigned)((in->size + g_block_size - 1) / g_block_size);
    wr32w(raw, 28, used * (g_block_size / 512));
    wr32w(raw, 32, rd32(raw, 32) | 0x80000);            /* EXTENTS_FL */
    /* The three timestamps, at 8, 12 and 16. i_dtime at 20 is when an inode
     * was deleted and is left alone: writing a nonzero one into a live inode
     * is what makes fsck report it as "deleted inode referenced". */
    wr32w(raw, 8, in->atime);
    wr32w(raw, 12, in->ctime);
    wr32w(raw, 16, in->mtime);
    memcpy(raw + 40, in->block, 60);
    return write_block(block, g_block);
}

/* Mark an inode as written to, now. `contents` distinguishes a change to the
 * file from a change to the inode alone: chmod moves ctime and leaves mtime
 * where it was, which is what lets a backup tell "the file changed" from
 * "somebody adjusted its permissions". */
static void touch_inode(struct inode* in, int contents)
{
    const unsigned now = (unsigned)time(0);
    in->ctime = now;
    if (contents) {
        in->mtime = now;
        in->atime = now;
    }
}

/* --- names --------------------------------------------------------------- */

#define ROOT_INODE 2

/* Walk one directory looking for `name`, or hand back the `want`th entry. */
static unsigned dir_search(const struct inode* dir, const char* name,
                           unsigned want, char* name_out, unsigned* kind_out)
{
    unsigned seen = 0;
    const unsigned long blocks = (dir->size + g_block_size - 1) / g_block_size;
    for (unsigned long b = 0; b < blocks; ++b) {
        const unsigned long phys = map_block(dir, b);
        if (phys == 0 || read_block(phys, g_block) != 0)
            continue;
        unsigned at = 0;
        while (at + 8 <= g_block_size) {
            const unsigned ino = rd32(g_block, at);
            const unsigned rec = rd16(g_block, at + 4);
            const unsigned len = g_block[at + 6];
            const unsigned type = g_block[at + 7];
            if (rec < 8 || at + rec > g_block_size)
                break;
            if (ino != 0 && len > 0) {
                if (name != 0) {
                    unsigned i = 0;
                    while (i < len && name[i] != '\0' &&
                           name[i] == (char)g_block[at + 8 + i])
                        ++i;
                    if (i == len && name[len] == '\0')
                        return ino;
                } else if (!(len == 1 && g_block[at + 8] == '.') &&
                           !(len == 2 && g_block[at + 8] == '.' &&
                             g_block[at + 9] == '.') && seen++ == want) {
                    unsigned i = 0;
                    for (; i < len && i < 63; ++i)
                        name_out[i] = (char)g_block[at + 8 + i];
                    name_out[i] = '\0';
                    if (kind_out != 0)
                        *kind_out = (type == 2) ? VFS_KIND_DIR
                                  : (type == 7) ? VFS_KIND_LINK
                                  : (type == 5) ? VFS_KIND_FIFO
                                  : (type == 3) ? VFS_KIND_CHR
                                  : (type == 4) ? VFS_KIND_BLK
                                                : VFS_KIND_FILE;
                    return ino;
                }
            }
            at += rec;
        }
    }
    return 0;
}

/* Resolve an absolute path to an inode number. */
#define MODE_KIND(m)  ((m) & 0xF000u)
#define MODE_DIR      0x4000u
#define MODE_LINK     0xA000u
#define MODE_FILE     0x8000u
#define MODE_FIFO     0x1000u
#define MODE_CHR      0x2000u
#define MODE_BLK      0x6000u

/* The device number a node carries. ext4 puts a small one in the first four
 * bytes of the block pointers, which a device has no other use for - the same
 * trick as a short symlink, and for the same reason. */
static unsigned node_rdev(const struct inode* in)
{
    if (MODE_KIND(in->mode) != MODE_CHR && MODE_KIND(in->mode) != MODE_BLK)
        return 0;
    return (unsigned)in->block[0] | ((unsigned)in->block[1] << 8)
         | ((unsigned)in->block[2] << 16) | ((unsigned)in->block[3] << 24);
}

/* Where a symbolic link points.
 *
 * ext4 keeps a short target in the sixty bytes the block pointers would have
 * used - there is nothing to point at, so the space is free - and a long one
 * in an ordinary data block. Sixty bytes covers very nearly every symlink
 * there has ever been, but not all of them, so both are read here.
 */
static int read_link(const struct inode* in, char* out, unsigned max)
{
    if (in->size == 0 || in->size >= max)
        return -1;
    if (in->size < sizeof(in->block)) {
        memcpy(out, in->block, in->size);
        out[in->size] = '\0';
        return 0;
    }
    const unsigned long phys = map_block(in, 0);
    if (phys == 0 || read_block(phys, g_block) != 0)
        return -1;
    memcpy(out, g_block, in->size);
    out[in->size] = '\0';
    return 0;
}

/* How many links one resolution may go through before giving up. Every UNIX
 * has this limit and they are all small; a chain longer than a handful is a
 * loop somebody made by accident, and following it forever is how a
 * filesystem server stops answering. */
#define MAX_LINK_DEPTH 16

/* Walk a path to the inode it names.
 *
 * `follow_last` is the whole difference between stat and lstat, and between
 * reading a link's target and reading the link: every UNIX call is one or the
 * other, and the resolver is the only place that can tell them apart.
 *
 * A link found part way through is always followed - `/tmp/link/file` means
 * the file inside whatever the link leads to, and there is no reading of that
 * which does not follow it.
 */
static const char* route(const char* path);

static unsigned lookup_deep(const char* path, struct inode* out,
                            int follow_last, int depth)
{
    char work[512];
    /* Which disk this name is on, and what it is called there. Every lookup
     * starts at the root inode - of that filesystem, not of the first one. */
    path = route(path);
    unsigned ino = ROOT_INODE;

    if (depth > MAX_LINK_DEPTH)
        return 0;
    if (read_inode(ino, out) != 0)
        return 0;

    unsigned at = 0;
    while (path[at] != '\0') {
        while (path[at] == '/') ++at;
        if (path[at] == '\0')
            break;
        char part[64];
        unsigned n = 0;
        while (path[at] != '/' && path[at] != '\0' && n < sizeof(part) - 1)
            part[n++] = path[at++];
        part[n] = '\0';

        /* Anything left after this component. Empty means this was the last
         * one, which is the case follow_last is about. */
        const char* rest = path + at;
        while (*rest == '/')
            ++rest;
        const int last = *rest == '\0';

        const unsigned found = dir_search(out, part, 0, 0, 0);
        if (found == 0 || read_inode(found, out) != 0)
            return 0;
        ino = found;

        if (MODE_KIND(out->mode) == MODE_LINK && (!last || follow_last)) {
            char target[512];
            if (read_link(out, target, sizeof(target)) != 0)
                return 0;

            /* A relative target is relative to the directory the link is in,
             * which is the path so far minus this component. Rebuilt rather
             * than tracked, because the parent is only needed here. */
            if (target[0] == '/') {
                snprintf(work, sizeof(work), "%s%s%s", target,
                         last ? "" : "/", last ? "" : rest);
            } else {
                unsigned cut = at - n;      /* just before this component */
                while (cut > 0 && path[cut - 1] == '/')
                    --cut;
                snprintf(work, sizeof(work), "%.*s/%s%s%s", (int)cut, path,
                         target, last ? "" : "/", last ? "" : rest);
            }
            return lookup_deep(work, out, follow_last, depth + 1);
        }
    }
    return ino;
}

static unsigned lookup(const char* path, struct inode* out)
{
    return lookup_deep(path, out, 1, 0);
}

/* The link itself, not what it leads to. */
static unsigned lookup_nofollow(const char* path, struct inode* out)
{
    return lookup_deep(path, out, 0, 0);
}

/* Give a file one more block, appending to the extent root. Four extents fit
 * inline, which is four runs rather than four blocks - a file written straight
 * through gets one extent and grows within it. */
static unsigned long extend(struct inode* in, unsigned number)
{
    unsigned char* node = in->block;
    if (rd16(node, 0) != 0xF30A || rd16(node, 6) != 0)
        return 0;                       /* not a root we can append to */
    const unsigned entries = rd16(node, 2);
    const unsigned long want = (in->size + g_block_size - 1) / g_block_size;

    const unsigned long phys = alloc_block();
    if (phys == 0)
        return 0;
    /* A fresh block of a directory has to read as empty rather than as
     * whatever was there before it was freed. */
    memset(g_block, 0, g_block_size);
    write_block(phys, g_block);

    if (entries > 0) {
        unsigned char* last = node + 12 + (entries - 1) * 12;
        const unsigned first = rd32(last, 0);
        const unsigned len = rd16(last, 4);
        const unsigned long start = ((unsigned long)rd16(last, 6) << 32) |
                                    rd32(last, 8);
        if (start + len == phys && first + len == want) {
            wr16w(last, 4, len + 1);    /* it carried on where it left off */
            return phys;
        }
    }
    if (entries >= rd16(node, 4))
        return 0;                       /* the inline root is full */
    unsigned char* ent = node + 12 + entries * 12;
    wr32w(ent, 0, (unsigned)want);
    wr16w(ent, 4, 1);
    wr16w(ent, 6, (unsigned)(phys >> 32));
    wr32w(ent, 8, (unsigned)phys);
    wr16w(node, 2, entries + 1);
    (void)number;
    return phys;
}

/* Put a name into a directory. ext4 packs entries by letting each one claim
 * more room than it needs, so making space means finding one with slack and
 * splitting it rather than shifting everything along. */
static int dir_add(struct inode* dir, unsigned dir_ino, const char* name,
                   unsigned ino, unsigned type)
{
    unsigned len = 0;
    while (name[len] != '\0') ++len;
    const unsigned need = (8 + len + 3) & ~3u;

    const unsigned long blocks = (dir->size + g_block_size - 1) / g_block_size;
    for (unsigned long b = 0; b <= blocks; ++b) {
        unsigned long phys;
        if (b == blocks) {
            /* Out of room in what it has; give it another block, which starts
             * as one entry spanning the whole thing. */
            phys = extend(dir, dir_ino);
            if (phys == 0)
                return -1;
            dir->size += g_block_size;
            memset(g_block, 0, g_block_size);
            wr32w(g_block, 0, ino);
            wr16w(g_block, 4, g_block_size);
            g_block[6] = (unsigned char)len;
            g_block[7] = (unsigned char)type;
            memcpy(g_block + 8, name, len);
            if (write_block(phys, g_block) != 0)
                return -1;
            return write_inode(dir_ino, dir, dir_links(dir_ino));
        }
        phys = map_block(dir, b);
        if (phys == 0 || read_block(phys, g_block) != 0)
            continue;

        unsigned at = 0;
        while (at + 8 <= g_block_size) {
            const unsigned e_ino = rd32(g_block, at);
            const unsigned rec = rd16(g_block, at + 4);
            const unsigned nlen = g_block[at + 6];
            if (rec < 8 || at + rec > g_block_size)
                break;
            const unsigned actual = e_ino == 0 ? 0 : ((8 + nlen + 3) & ~3u);
            if (rec - actual >= need) {
                unsigned put = at + actual;
                if (actual != 0)
                    wr16w(g_block, at + 4, actual);
                else
                    put = at;
                wr32w(g_block, put, ino);
                wr16w(g_block, put + 4, rec - actual);
                g_block[put + 6] = (unsigned char)len;
                g_block[put + 7] = (unsigned char)type;
                memcpy(g_block + put + 8, name, len);
                return write_block(phys, g_block);
            }
            at += rec;
        }
    }
    return -1;
}

/* Take a name out. The entry before it swallows its record, which is how ext4
 * has always deleted things - nothing moves and nothing is zeroed. */
static int dir_remove(struct inode* dir, const char* name, unsigned* ino_out)
{
    const unsigned long blocks = (dir->size + g_block_size - 1) / g_block_size;
    for (unsigned long b = 0; b < blocks; ++b) {
        const unsigned long phys = map_block(dir, b);
        if (phys == 0 || read_block(phys, g_block) != 0)
            continue;
        unsigned at = 0, prev = 0xFFFFFFFFu;
        while (at + 8 <= g_block_size) {
            const unsigned rec = rd16(g_block, at + 4);
            const unsigned nlen = g_block[at + 6];
            if (rec < 8 || at + rec > g_block_size)
                break;
            if (rd32(g_block, at) != 0 && nlen > 0) {
                unsigned i = 0;
                while (i < nlen && name[i] != '\0' &&
                       name[i] == (char)g_block[at + 8 + i])
                    ++i;
                if (i == nlen && name[nlen] == '\0') {
                    if (ino_out != 0)
                        *ino_out = rd32(g_block, at);
                    if (prev != 0xFFFFFFFFu)
                        wr16w(g_block, prev + 4, rd16(g_block, prev + 4) + rec);
                    else
                        wr32w(g_block, at, 0);
                    return write_block(phys, g_block);
                }
            }
            prev = at;
            at += rec;
        }
    }
    return -1;
}

/* True when a directory holds nothing but itself and its parent. Removing one
 * that still has contents would strand every one of them: the entries would
 * still exist and nothing would reach them, which is the kind of damage fsck
 * can find and cannot undo. */
static int dir_is_empty(const struct inode* dir)
{
    const unsigned long blocks = (dir->size + g_block_size - 1) / g_block_size;
    for (unsigned long b = 0; b < blocks; ++b) {
        const unsigned long phys = map_block(dir, b);
        if (phys == 0 || read_block(phys, g_block) != 0)
            continue;
        unsigned at = 0;
        while (at + 8 <= g_block_size) {
            const unsigned ino = rd32(g_block, at);
            const unsigned rec = rd16(g_block, at + 4);
            const unsigned len = g_block[at + 6];
            if (rec < 8 || at + rec > g_block_size)
                break;
            if (ino != 0 && len > 0) {
                const int dot = len == 1 && g_block[at + 8] == '.';
                const int dotdot = len == 2 && g_block[at + 8] == '.' &&
                                   g_block[at + 9] == '.';
                if (!dot && !dotdot)
                    return 0;
            }
            at += rec;
        }
    }
    return 1;
}

/* Split a path into the directory holding it and the last component. */
static int split_path(const char* path, char* parent, char* last)
{
    int cut = -1;
    int n = 0;
    while (path[n] != '\0') {
        if (path[n] == '/') cut = n;
        ++n;
    }
    if (cut < 0 || n - cut - 1 == 0 || n - cut - 1 > 63)
        return -1;
    int i = 0;
    for (; i < cut; ++i) parent[i] = path[i];
    parent[i] = '\0';
    if (cut == 0) { parent[0] = '/'; parent[1] = '\0'; }
    for (i = 0; i < n - cut - 1; ++i) last[i] = path[cut + 1 + i];
    last[i] = '\0';
    return 0;
}

/* Make an empty file or directory and link it into its parent. */
/* Make a name. `link_to` non-null makes a symbolic link rather than a file or
 * a directory, which is a third kind of thing to create and otherwise exactly
 * the same job - the same parent lookup, the same allocation, the same entry
 * added at the end. */
#define MAKE_FILE 0
#define MAKE_DIR  1
#define MAKE_FIFO 2
#define MAKE_CHR  3
#define MAKE_BLK  4

/* `rdev` is read only for MAKE_CHR and MAKE_BLK, where it is the whole of the
 * file's contents; every other kind ignores it. */
static int create_node(const char* path, int is_dir, const char* link_to,
                       unsigned rdev)
{
    char parent[256], name[64];
    if (split_path(path, parent, name) != 0)
        return -EINVAL;
    struct inode dir;
    const unsigned dir_ino = lookup(parent, &dir);
    if (dir_ino == 0)
        return -ENOENT;                 /* the directory it would go in */
    if ((dir.mode & 0xF000) != 0x4000)
        return -ENOTDIR;
    if (dir_search(&dir, name, 0, 0, 0) != 0)
        return -EEXIST;

    const unsigned ino = alloc_inode(is_dir == MAKE_DIR);
    if (ino == 0)
        return -ENOSPC;                 /* the inode table is full */

    struct inode fresh;
    memset(&fresh, 0, sizeof(fresh));
    fresh.mode = is_dir == MAKE_DIR  ? 0040755
               : is_dir == MAKE_FIFO ? 0010644
               : is_dir == MAKE_CHR  ? (MODE_CHR | 0666u)
               : is_dir == MAKE_BLK  ? (MODE_BLK | 0660u)
               : link_to != 0        ? 0120777
                                     : 0100644;
    fresh.size = 0;
    touch_inode(&fresh, 1);
    /* An empty extent root: the magic, no entries, and room for four. */
    wr16w(fresh.block, 0, 0xF30A);
    wr16w(fresh.block, 2, 0);
    wr16w(fresh.block, 4, 4);
    wr16w(fresh.block, 6, 0);
    if (link_to != 0) {
        const unsigned long len = strlen(link_to);
        fresh.size = len;
        if (len < sizeof(fresh.block)) {
            /* Short enough to live where the block pointers would have been.
             * A link has nothing to point at, so the sixty bytes are free. */
            memset(fresh.block, 0, sizeof(fresh.block));
            memcpy(fresh.block, link_to, len);
            if (write_inode(ino, &fresh, 1u) != 0)
                return -EIO;
        } else {
            /* Long enough to need a block of its own. Written before the
             * inode, so a link never exists pointing at nothing. */
            if (write_inode(ino, &fresh, 1u) != 0)
                return -EIO;
            const unsigned long phys = extend(&fresh, ino);
            if (phys == 0)
                return -ENOSPC;
            memset(g_block, 0, g_block_size);
            memcpy(g_block, link_to, len < g_block_size ? len : g_block_size);
            if (write_block(phys, g_block) != 0 ||
                write_inode(ino, &fresh, 1u) != 0)
                return -EIO;
        }
        if (dir_add(&dir, dir_ino, name, ino, 7) != 0)
            return -EIO;
        return 0;
    }

    if (write_inode(ino, &fresh, is_dir == MAKE_DIR ? 2u : 1u) != 0)
        return -EIO;

    if (is_dir == MAKE_FIFO) {
        /* Nothing else to make. The inode is the whole file: a FIFO holds no
         * data and never grows, so there is no extent tree to build. */
        if (dir_add(&dir, dir_ino, name, ino, 5) != 0)
            return -EIO;
        return 0;
    }

    if (is_dir == MAKE_CHR || is_dir == MAKE_BLK) {
        /* The device number replaces the extent root written above: a device
         * has no blocks, so the space where they would be addressed holds the
         * one number that says which driver answers for it. */
        memset(fresh.block, 0, sizeof(fresh.block));
        fresh.block[0] = (unsigned char)(rdev & 0xFF);
        fresh.block[1] = (unsigned char)((rdev >> 8) & 0xFF);
        fresh.block[2] = (unsigned char)((rdev >> 16) & 0xFF);
        fresh.block[3] = (unsigned char)((rdev >> 24) & 0xFF);
        if (write_inode(ino, &fresh, 1u) != 0)
            return -EIO;
        if (dir_add(&dir, dir_ino, name, ino,
                    is_dir == MAKE_CHR ? 3 : 4) != 0)
            return -EIO;
        return 0;
    }

    if (is_dir == MAKE_DIR) {
        /* A directory is not empty on disk: it holds itself and its parent. */
        const unsigned long phys = extend(&fresh, ino);
        if (phys == 0)
            return -EIO;
        fresh.size = g_block_size;
        memset(g_block, 0, g_block_size);
        wr32w(g_block, 0, ino);
        wr16w(g_block, 4, 12);
        g_block[6] = 1; g_block[7] = 2; g_block[8] = '.';
        wr32w(g_block, 12, dir_ino);
        wr16w(g_block, 16, g_block_size - 12);
        g_block[18] = 2; g_block[19] = 2; g_block[20] = '.'; g_block[21] = '.';
        if (write_block(phys, g_block) != 0 ||
            write_inode(ino, &fresh, 2u) != 0)
            return -EIO;
    }

    if (dir_add(&dir, dir_ino, name, ino, is_dir == MAKE_DIR ? 2 : 1) != 0)
        return -EIO;
    if (is_dir == MAKE_DIR) {
        /* The parent gained a ".." pointing at it. */
        unsigned long block; unsigned at;
        if (inode_location(dir_ino, &block, &at) == 0 &&
            read_block(block, g_block) == 0) {
            wr16w(g_block, at + 26, rd16(g_block, at + 26) + 1);
            write_block(block, g_block);
        }
    }
    return 0;
}

static int create(const char* path, int is_dir)
{
    return create_node(path, is_dir ? MAKE_DIR : MAKE_FILE, 0, 0);
}

/* A second name for a file that already exists.
 *
 * Not a copy and not a symbolic link: one inode with two directory entries
 * pointing at it, and no way to tell which of them came first. That is what
 * the link count in the inode is for, and why unlink has to consult it rather
 * than freeing whatever it removed a name for.
 *
 * Directories are refused. A second name for a directory makes the tree a
 * graph, and every operation that walks it - find, rm -r, a resolver counting
 * its depth - has to be prepared for a cycle. Every UNIX refuses this for the
 * same reason.
 */
static int link_node(const char* existing, const char* path)
{
    struct inode target;
    /* Not followed: a link to a symbolic link is a second name for the link
     * itself, which is what every other system does with it. */
    const unsigned ino = lookup_nofollow(existing, &target);
    if (ino == 0)
        return -ENOENT;
    if (MODE_KIND(target.mode) == MODE_DIR)
        return -EPERM;

    char parent[256], name[64];
    if (split_path(path, parent, name) != 0)
        return -EINVAL;
    struct inode dir;
    const unsigned dir_ino = lookup(parent, &dir);
    if (dir_ino == 0)
        return -ENOENT;
    if (MODE_KIND(dir.mode) != MODE_DIR)
        return -ENOTDIR;
    if (dir_search(&dir, name, 0, 0, 0) != 0)
        return -EEXIST;

    const unsigned type = MODE_KIND(target.mode) == MODE_LINK ? 7 : 1;
    if (dir_add(&dir, dir_ino, name, ino, (int)type) != 0)
        return -EIO;

    /* One more name, so one more link. Written straight into the inode rather
     * than through write_inode, which would need the whole inode read back
     * and would rewrite fields nothing here is changing. */
    unsigned long block; unsigned at;
    if (inode_location(ino, &block, &at) == 0 &&
        read_block(block, g_block) == 0) {
        wr16w(g_block, at + 26, rd16(g_block, at + 26) + 1);
        write_block(block, g_block);
    }
    return 0;
}

/* --- what is mounted ----------------------------------------------------------
 *
 * A table rather than a fact, even though there is one disk on it.
 *
 * Three different things answer for parts of this tree and until now none of
 * them said so anywhere: the ext4 filesystem this server keeps, the /dev
 * entries libc answers without asking anybody, and /proc below, which is not
 * storage at all. A person asking "what is this directory" deserves an answer,
 * and `mount` with nothing to print is a worse answer than none.
 *
 * Mounting a *second* block filesystem is not here. Every one of this server's
 * superblock globals - the block size, the group descriptors, the inode size -
 * is a singleton, and a second device means all of them per-mount. The table
 * is the half that is honest to have without it.
 */
struct mount_entry {
    const char* at;
    const char* what;
    const char* kind;
    const char* how;
};



/* --- /proc --------------------------------------------------------------------
 *
 * Not on the disk and never was. These are questions about the running machine
 * - what it is doing, how long it has been doing it, how much memory is left -
 * and the answers exist in the kernel, so writing them down somewhere would
 * only make them stale.
 *
 * They are served here rather than in libc, where /dev is served, because /dev
 * is a fixed list of five names and this is not: the contents change with
 * every process that starts, and a listing has to be built when it is asked
 * for. That is a filesystem's job, and this is the filesystem.
 *
 * Everything is generated whole into a buffer and then sliced by the offset
 * the reader asked for. These are all small - a page at the very most - and a
 * seekable stream over a value that changes underneath is a worse lie than a
 * snapshot per read.
 */

#define PROC_MAX 4096

/* One task list, not one per function.
 *
 * Two hundred and fifty-six of these is eighteen kilobytes, and three of the
 * routines below wanted their own copy on the stack - which is how raising the
 * task limit turned every question about /proc/<pid> into a stack overflow in
 * this server while the fixed files carried on working. A server that answers
 * one request at a time can share one buffer. */
static struct proc_info g_tasks[256]; 

static int is_proc(const char* path)
{
    return strncmp(path, "/proc", 5) == 0 &&
           (path[5] == '\0' || path[5] == '/');
}

/* The pid a /proc path names, or 0 for one that names none. */
static unsigned proc_pid_of(const char* path, const char** rest)
{
    unsigned pid = 0, digits = 0;
    const char* at = path + 5;
    while (*at == '/')
        ++at;
    while (*at >= '0' && *at <= '9') {
        pid = pid * 10 + (unsigned)(*at++ - '0');
        ++digits;
    }
    if (digits == 0 || (*at != '\0' && *at != '/'))
        return 0;
    while (*at == '/')
        ++at;
    if (rest != 0)
        *rest = at;
    return pid;
}

static const char* state_word(unsigned state)
{
    switch (state) {
    case PROC_READY:   return "ready";
    case PROC_RUNNING: return "running";
    case PROC_BLOCKED: return "waiting";
    case PROC_STOPPED: return "stopped";
    case PROC_ZOMBIE:  return "zombie";
    default:           return "dead";
    }
}

/* Build the contents of a /proc file. Returns the length, or -1 when the path
 * is not one of them. */
static int proc_contents(const char* path, char* out, unsigned max)
{
    const char* rest = "";
    const unsigned pid = proc_pid_of(path, &rest);

    if (pid != 0) {
        
        const int n = proc_list(g_tasks, 256);
        for (int i = 0; i < n; ++i) {
            if (g_tasks[i].pid != pid)
                continue;
            if (strcmp(rest, "status") != 0)
                return -1;
            return snprintf(out, max,
                            "name\t%s\n"
                            "pid\t%u\n"
                            "ppid\t%u\n"
                            "pgid\t%u\n"
                            "sid\t%u\n"
                            "uid\t%u\n"
                            "state\t%s\n"
                            "ticks\t%lu\n"
                            "memory\t%lu kB\n",
                            g_tasks[i].name, g_tasks[i].pid, g_tasks[i].parent,
                            g_tasks[i].pgid, g_tasks[i].sid, g_tasks[i].uid,
                            state_word(g_tasks[i].state),
                            (unsigned long)g_tasks[i].ticks,
                            (unsigned long)(g_tasks[i].bytes / 1024));
        }
        return -1;
    }

    if (strcmp(path, "/proc/meminfo") == 0) {
        struct mem_info m;
        if (mem_info(&m) != 0)
            return -1;
        return snprintf(out, max,
                        "total\t%lu kB\nused\t%lu kB\nfree\t%lu kB\n",
                        (unsigned long)(m.usable / 1024),
                        (unsigned long)(m.used / 1024),
                        (unsigned long)(m.free / 1024));
    }
    if (strcmp(path, "/proc/uptime") == 0) {
        const unsigned long ms = uptime_ms();
        return snprintf(out, max, "%lu.%03lu\n", ms / 1000, ms % 1000);
    }
    if (strcmp(path, "/proc/mounts") == 0) {
        int at = 0;
        /* The filesystems actually mounted, read from the table rather than
         * from a list written down at build time and hoped to still be true. */
        for (int i = 0; i < FS_MAX && at < (int)max; ++i) {
            if (!g_fs[i].used)
                continue;
            at += snprintf(out + at, max - (unsigned)at, "/dev/sda%u %s ext4 rw\n",
                           g_fs[i].dev + 2, g_fs[i].at);
        }
        /* And the one that is not a disk at all. /dev used to be listed here
         * too, as a devfs; it is ordinary inodes on the root filesystem now. */
        if (at < (int)max)
            at += snprintf(out + at, max - (unsigned)at,
                           "procfs /proc procfs ro\n");
        return at;
    }
    if (strcmp(path, "/proc/cpuinfo") == 0) {
        struct cpu_stat cpus[32];
        const int n = cpu_info(cpus, 32);
        int at = snprintf(out, max, "processors\t%d\n", n < 0 ? 0 : n);
        for (int i = 0; i < n && at < (int)max; ++i)
            at += snprintf(out + at, max - (unsigned)at,
                           "cpu%d\tbusy %lu idle %lu\n", i,
                           (unsigned long)cpus[i].busy,
                           (unsigned long)cpus[i].idle);
        return at;
    }
    if (strcmp(path, "/proc/loadavg") == 0) {
        unsigned long load[3] = { 0, 0, 0 };
        load_average(load);
        return snprintf(out, max, "%lu.%02lu %lu.%02lu %lu.%02lu\n",
                        load[0] / 100, load[0] % 100,
                        load[1] / 100, load[1] % 100,
                        load[2] / 100, load[2] % 100);
    }
    if (strcmp(path, "/proc/version") == 0)
        return snprintf(out, max, "leahOS x86-64\n");
    return -1;
}

/* The fixed names in /proc, beside one directory per process. */
static const char* const kProcFiles[] = {
    "cpuinfo", "loadavg", "meminfo", "mounts", "uptime", "version",
};

#define PROC_FILE_COUNT (sizeof(kProcFiles) / sizeof(kProcFiles[0]))

/* What a /proc path is: a directory, a file, or nothing. */
static int proc_kind(const char* path, unsigned* kind, unsigned long* size)
{
    char scratch[PROC_MAX];

    if (strcmp(path, "/proc") == 0 || strcmp(path, "/proc/") == 0) {
        *kind = VFS_KIND_DIR;
        *size = 0;
        return 0;
    }
    {
        const char* rest = "";
        const unsigned pid = proc_pid_of(path, &rest);
        if (pid != 0 && rest[0] == '\0') {
            /* A process's own directory exists exactly while it does. */
            
            const int n = proc_list(g_tasks, 256);
            for (int i = 0; i < n; ++i)
                if (g_tasks[i].pid == pid) {
                    *kind = VFS_KIND_DIR;
                    *size = 0;
                    return 0;
                }
            return -1;
        }
    }
    {
        const int len = proc_contents(path, scratch, sizeof(scratch));
        if (len < 0)
            return -1;
        *kind = VFS_KIND_FILE;
        *size = (unsigned long)len;
        return 0;
    }
}

/* The Nth entry of a /proc directory. Processes come after the fixed names, in
 * the order the kernel reports them, which is the order they were made. */
static int proc_entry(const char* path, unsigned index, char* name,
                      unsigned* kind, unsigned long* size)
{
    char scratch[PROC_MAX];

    if (strcmp(path, "/proc") == 0 || strcmp(path, "/proc/") == 0) {
        if (index < PROC_FILE_COUNT) {
            char full[64];
            snprintf(name, 64, "%s", kProcFiles[index]);
            snprintf(full, sizeof(full), "/proc/%s", kProcFiles[index]);
            const int len = proc_contents(full, scratch, sizeof(scratch));
            *kind = VFS_KIND_FILE;
            *size = len < 0 ? 0 : (unsigned long)len;
            return 0;
        }
        
        const int n = proc_list(g_tasks, 256);
        const unsigned want = index - PROC_FILE_COUNT;
        unsigned seen = 0;
        for (int i = 0; i < n; ++i) {
            /* Processes, not threads: /proc/N is a process, and a thread is
             * not one however much it looks like a task from in here. */
            if (g_tasks[i].pid != g_tasks[i].tgid)
                continue;
            if (seen++ != want)
                continue;
            snprintf(name, 64, "%u", g_tasks[i].pid);
            *kind = VFS_KIND_DIR;
            *size = 0;
            return 0;
        }
        return -1;
    }
    {
        const char* rest = "";
        const unsigned pid = proc_pid_of(path, &rest);
        if (pid != 0 && rest[0] == '\0' && index == 0) {
            snprintf(name, 64, "status");
            char full[64];
            snprintf(full, sizeof(full), "/proc/%u/status", pid);
            const int len = proc_contents(full, scratch, sizeof(scratch));
            if (len < 0)
                return -1;
            *kind = VFS_KIND_FILE;
            *size = (unsigned long)len;
            return 0;
        }
    }
    return -1;
}

/* --- the journal -------------------------------------------------------------
 *
 * ext4 keeps a journal so that a crash in the middle of changing the tree
 * leaves the disk recoverable rather than merely broken. The idea is older
 * than the filesystem: write what you are about to do somewhere safe, then do
 * it, and on the way back up either the record is complete and the work can be
 * finished, or it is not and none of it ever happened. What must never happen
 * is half.
 *
 * The image has carried `has_journal` since it was first made, and until now
 * this server ignored it - which is worse than not having one, because the
 * flag tells every other system that the disk is safe to trust.
 *
 * This is the recovery half: at mount, any transaction that reached its commit
 * block is written out to where it belongs, and the journal is then reset. A
 * transaction without a commit block is discarded, which is the whole point of
 * the commit block.
 *
 * Everything in here is big-endian. The journal came from a machine that was,
 * and the format did not change when the filesystem moved.
 */

#define JBD2_MAGIC        0xC03B3998u
#define JBD2_DESCRIPTOR   1
#define JBD2_COMMIT       2
#define JBD2_REVOKE       5

#define JBD2_FLAG_ESCAPE    1
#define JBD2_FLAG_SAME_UUID 2
#define JBD2_FLAG_LAST_TAG  8

/* Set when the superblock says there is one and its inode could be read. */

/* Static, not automatic. Three of these nested inside each other is sixteen
 * kilobytes of stack in a ring-3 server that has nothing like that to spare -
 * which showed up as the boot stopping dead after the replay, with no fault
 * reported and every process that wanted a file waiting on one that was gone. */
static unsigned char g_jbuf[sizeof(g_block)];   /* descriptors and the sb   */
static unsigned char g_jdata[sizeof(g_block)];  /* one journalled block     */


static unsigned rd32be(const unsigned char* p, unsigned at)
{
    return ((unsigned)p[at] << 24) | ((unsigned)p[at + 1] << 16)
         | ((unsigned)p[at + 2] << 8) | (unsigned)p[at + 3];
}

static void wr32be(unsigned char* p, unsigned at, unsigned v)
{
    p[at]     = (unsigned char)(v >> 24);
    p[at + 1] = (unsigned char)(v >> 16);
    p[at + 2] = (unsigned char)(v >> 8);
    p[at + 3] = (unsigned char)v;
}

/* One block of the journal file, by its index within that file. */
static int jrnl_read(unsigned index, unsigned char* out)
{
    const unsigned long phys = map_block(&g_journal, index);
    if (phys == 0)
        return -1;
    g_txn_bypass = 1;
    const int r = read_block(phys, out);
    g_txn_bypass = 0;
    return r;
}

/* Straight to the disk, always. Collecting the journal's own blocks into the
 * transaction they are the record of would be a circle. */
static int jrnl_write(unsigned index, const unsigned char* in)
{
    const unsigned long phys = map_block(&g_journal, index);
    if (phys == 0)
        return -1;
    g_txn_bypass = 1;
    const int r = write_block(phys, in);
    g_txn_bypass = 0;
    return r;
}

/* The journal is a ring. */
static unsigned jrnl_next(unsigned at)
{
    ++at;
    return at >= g_jrnl_maxlen ? g_jrnl_first : at;
}

/* How many block tags fit in a descriptor, and how wide one is. Without
 * checksum-v3 a tag is the block number and the flags, plus a UUID on the
 * first tag of a transaction unless it says otherwise. */
static unsigned tag_width(unsigned flags, int sixty_four)
{
    unsigned n = sixty_four ? 12u : 8u;
    if (!(flags & JBD2_FLAG_SAME_UUID))
        n += 16;
    return n;
}

/* Walk one transaction's descriptor, calling back for each block it carries.
 * Returns the journal index just past the last data block, or 0 on a
 * malformed descriptor. `apply` writes; when it is zero this only counts. */
static unsigned jrnl_walk(const unsigned char* desc, unsigned at, int apply,
                          int sixty_four)
{
    unsigned off = 12;                  /* past the header */
    unsigned where = jrnl_next(at);     /* data follows the descriptor */

    for (;;) {
        if (off + 8 > g_block_size)
            return 0;
        const unsigned target = rd32be(desc, off);
        const unsigned flags  = rd32be(desc, off + 4);
        off += tag_width(flags, sixty_four);
        if (off > g_block_size)
            return 0;

        if (apply) {
            if (jrnl_read(where, g_jdata) != 0)
                return 0;
            /* An escaped block held the journal's own magic where its first
             * word goes, so it was stored with that word zeroed. Put it back
             * before the block reaches the disk it belongs on. */
            if (flags & JBD2_FLAG_ESCAPE)
                wr32be(g_jdata, 0, JBD2_MAGIC);
            if (target != 0 && write_block(target, g_jdata) != 0)
                return 0;
        }
        where = jrnl_next(where);
        if (flags & JBD2_FLAG_LAST_TAG)
            break;
    }
    return where;
}

/* Replay everything that committed, then declare the journal empty.
 *
 * Two passes, as every implementation of this does it: the first finds how far
 * the committed transactions reach, and only then does the second write
 * anything. A single pass would apply the blocks of a transaction whose commit
 * block turns out not to be there - which is exactly the torn write the
 * journal exists to prevent, performed deliberately by the recovery code.
 */
static int journal_replay(void)
{
    if (!g_has_journal || g_jrnl_start == 0)
        return 0;

    const int sixty_four = 0;   /* no 64-bit journal on an image this size */

    /* Pass one: how many transactions committed. */
    unsigned at = g_jrnl_start, seq = g_jrnl_seq, committed = 0;
    for (;;) {
        if (jrnl_read(at, g_jbuf) != 0 || rd32be(g_jbuf, 0) != JBD2_MAGIC)
            break;
        if (rd32be(g_jbuf, 8) != seq)
            break;
        const unsigned type = rd32be(g_jbuf, 4);
        if (type == JBD2_COMMIT) {
            ++committed;
            ++seq;
            at = jrnl_next(at);
            continue;
        }
        if (type == JBD2_DESCRIPTOR) {
            const unsigned past = jrnl_walk(g_jbuf, at, 0, sixty_four);
            if (past == 0)
                break;
            at = past;
            continue;
        }
        if (type == JBD2_REVOKE) {      /* nothing here ever writes one */
            at = jrnl_next(at);
            continue;
        }
        break;
    }

    if (committed == 0) {
        printf("vfsd: journal has no complete transaction; discarding it\n");
    } else {
        /* Pass two: apply exactly that many. */
        at = g_jrnl_start;
        seq = g_jrnl_seq;
        unsigned done = 0;
        while (done < committed) {
            if (jrnl_read(at, g_jbuf) != 0 || rd32be(g_jbuf, 0) != JBD2_MAGIC)
                break;
            const unsigned type = rd32be(g_jbuf, 4);
            if (type == JBD2_COMMIT) {
                ++done;
                ++seq;
                at = jrnl_next(at);
                continue;
            }
            if (type == JBD2_DESCRIPTOR) {
                const unsigned past = jrnl_walk(g_jbuf, at, 1, sixty_four);
                if (past == 0)
                    break;
                at = past;
                continue;
            }
            at = jrnl_next(at);
        }
        printf("vfsd: journal replayed %u transaction%s\n",
               done, done == 1 ? "" : "s");
    }

    /* The journal is now empty: s_start of zero says so, and the sequence
     * moves on so that anything still on disk from before reads as stale.
     * Written last, because until it is written the replay can be repeated
     * safely and after it is written it must not be. */
    if (jrnl_read(0, g_jbuf) == 0 && rd32be(g_jbuf, 0) == JBD2_MAGIC) {
        wr32be(g_jbuf, 24, seq);          /* s_sequence */
        wr32be(g_jbuf, 28, 0);            /* s_start                          */
        if (jrnl_write(0, g_jbuf) != 0)
            return -1;
    }
    g_jrnl_start = 0;
    return 0;
}

/* Write the collected blocks to the journal, commit them, and only then put
 * them where they belong.
 *
 * The order is the whole guarantee, so it is worth stating: descriptor, then
 * the blocks, then the commit block, then the journal superblock pointing at
 * the lot. A crash anywhere before that last write leaves a journal whose
 * s_start is still zero and a disk that never heard about any of this. After
 * it, recovery finds a complete transaction and finishes the job.
 *
 * One transaction is in flight at a time and it is checkpointed immediately,
 * so it always starts at the first block. A real journal batches many and
 * lets the ring fill, which is faster and much harder to be sure of.
 */
static int txn_flush(void)
{
    if (g_txn_count == 0)
        return 0;
    if (!g_has_journal) {
        /* No journal to write to. The writes still have to happen. */
        for (unsigned i = 0; i < g_txn_count; ++i) {
            g_txn_bypass = 1;
            const int r = write_block(g_txn_target[i], g_txn_buf[i]);
            g_txn_bypass = 0;
            if (r != 0)
                return -1;
        }
        g_txn_count = 0;
        return 0;
    }
    /* It has to fit between the first block and the end, with room for the
     * descriptor and the commit block. */
    if (g_txn_count + 2 > g_jrnl_maxlen - g_jrnl_first)
        return -1;

    const unsigned seq = g_jrnl_seq;

    memset(g_jbuf, 0, g_block_size);
    wr32be(g_jbuf, 0, JBD2_MAGIC);
    wr32be(g_jbuf, 4, JBD2_DESCRIPTOR);
    wr32be(g_jbuf, 8, seq);
    unsigned off = 12;
    for (unsigned i = 0; i < g_txn_count; ++i) {
        wr32be(g_jbuf, off, (unsigned)g_txn_target[i]);
        unsigned flags = JBD2_FLAG_SAME_UUID;
        /* A metadata block whose first word happens to be the journal's magic
         * would look like a descriptor to recovery, so it is stored with that
         * word zeroed and this flag says to put it back. */
        if (rd32be(g_txn_buf[i], 0) == JBD2_MAGIC)
            flags |= JBD2_FLAG_ESCAPE;
        if (i + 1 == g_txn_count)
            flags |= JBD2_FLAG_LAST_TAG;
        wr32be(g_jbuf, off + 4, flags);
        off += 8;
    }
    if (jrnl_write(g_jrnl_first, g_jbuf) != 0)
        return -1;

    for (unsigned i = 0; i < g_txn_count; ++i) {
        memcpy(g_jdata, g_txn_buf[i], g_block_size);
        if (rd32be(g_jdata, 0) == JBD2_MAGIC)
            wr32be(g_jdata, 0, 0);
        if (jrnl_write(g_jrnl_first + 1 + i, g_jdata) != 0)
            return -1;
    }

    memset(g_jbuf, 0, g_block_size);
    wr32be(g_jbuf, 0, JBD2_MAGIC);
    wr32be(g_jbuf, 4, JBD2_COMMIT);
    wr32be(g_jbuf, 8, seq);
    if (jrnl_write(g_jrnl_first + 1 + g_txn_count, g_jbuf) != 0)
        return -1;

    /* The moment it becomes real. */
    if (jrnl_read(0, g_jbuf) != 0 || rd32be(g_jbuf, 0) != JBD2_MAGIC)
        return -1;
    wr32be(g_jbuf, 24, seq);
    wr32be(g_jbuf, 28, g_jrnl_first);
    if (jrnl_write(0, g_jbuf) != 0)
        return -1;

    /* Now the disk itself. If the power goes here, recovery does this again. */
    for (unsigned i = 0; i < g_txn_count; ++i) {
        g_txn_bypass = 1;
        const int r = write_block(g_txn_target[i], g_txn_buf[i]);
        g_txn_bypass = 0;
        if (r != 0)
            return -1;
    }

    /* And it is finished, so the journal is empty again. */
    g_jrnl_seq = seq + 1;
    if (jrnl_read(0, g_jbuf) == 0 && rd32be(g_jbuf, 0) == JBD2_MAGIC) {
        wr32be(g_jbuf, 24, g_jrnl_seq);
        wr32be(g_jbuf, 28, 0);
        jrnl_write(0, g_jbuf);
    }
    g_txn_count = 0;
    return 0;
}

/* A batch spans requests rather than being one per request.
 *
 * Committing every request meant a descriptor, a commit block and two
 * journal superblock writes for every three blocks of actual change, which
 * roughly tripled the traffic to the disk server and was felt on every boot.
 * Several operations in a row touch the same handful of metadata blocks, so a
 * batch of them costs very little more than one of them did.
 *
 * What this gives up is that a reply no longer means "on the disk". It means
 * what it means on every other filesystem: the change is in the tree, and it
 * will be on the disk at the next commit. sync forces one.
 */
static void txn_begin(void)
{
    if (g_txn_open)
        return;                 /* already collecting */
    g_txn_count = 0;
    g_txn_started = uptime_ms();
    g_txn_open = 1;
}

static int txn_end(int force)
{
    if (!g_txn_open)
        return 0;
    /* Kept open unless it is nearly full, old, or somebody asked. The room
     * left is for one more request: a batch is only ever split at a request
     * boundary, so a single operation is still all-or-nothing. */
    const int full = g_txn_count + TXN_HEADROOM > TXN_MAX;
    const int old  = uptime_ms() - g_txn_started >= TXN_MAX_AGE_MS;
    if (!force && !full && !old)
        return 0;
    const int r = txn_flush();
    g_txn_open = 0;
    return r;
}

/* Find the journal and read its superblock. Failure here is not fatal: a
 * filesystem without one still mounts, it is just no safer than it was. */
static void journal_open(const unsigned char* sb)
{
    g_has_journal = 0;
    if (!(rd32(sb, 92) & 0x0004))       /* COMPAT_HAS_JOURNAL */
        return;
    const unsigned inum = rd32(sb, 224);
    if (inum == 0 || read_inode(inum, &g_journal) != 0)
        return;

    const unsigned long phys = map_block(&g_journal, 0);
    if (phys == 0 || read_block(phys, g_jbuf) != 0)
        return;
    if (rd32be(g_jbuf, 0) != JBD2_MAGIC)
        return;

    /* The journal keeps its own block size, and a journal whose blocks are a
     * different size from the filesystem's is one this cannot walk. */
    if (rd32be(g_jbuf, 12) != g_block_size)
        return;

    g_jrnl_maxlen = rd32be(g_jbuf, 16);
    g_jrnl_first  = rd32be(g_jbuf, 20);
    g_jrnl_seq    = rd32be(g_jbuf, 24);
    g_jrnl_start  = rd32be(g_jbuf, 28);
    if (g_jrnl_maxlen == 0 || g_jrnl_first == 0)
        return;
    g_has_journal = 1;
    printf("vfsd: journal, %u blocks from %u, sequence %u%s\n",
           g_jrnl_maxlen, g_jrnl_first, g_jrnl_seq,
           g_jrnl_start != 0 ? ", needs recovery" : "");
}

/* Which mount a path belongs to, and what it is called inside that mount.
 *
 * The longest mount point that is a prefix of the path wins, which is what
 * makes /mnt/x belong to /mnt rather than to /. Today there is one mount and
 * this always answers the same, but the answer is now looked up rather than
 * assumed, which is the part that had to change.
 */
static const char* route(const char* path)
{
    struct fs* best = &g_fs[0];
    unsigned best_len = 0;

    for (int i = 0; i < FS_MAX; ++i) {
        if (!g_fs[i].used)
            continue;
        const unsigned n = (unsigned)strlen(g_fs[i].at);
        if (n == 1)                     /* "/" matches everything, weakly */
            continue;
        if (strncmp(path, g_fs[i].at, n) != 0)
            continue;
        if (path[n] != '\0' && path[n] != '/')
            continue;                   /* /mnternal is not under /mnt */
        if (n > best_len) {
            best = &g_fs[i];
            best_len = n;
        }
    }

    /* A batch belongs to the disk it was collected for: the block numbers in
     * it mean nothing on another one. Crossing mounts commits what is held. */
    if (best != g_cur && g_txn_open) {
        txn_flush();
        g_txn_started = uptime_ms();
    }
    g_cur = best;
    return best_len == 0 ? path
                         : (path[best_len] == '\0' ? "/" : path + best_len);
}

/* Read a disk's superblock into a slot and make it a filesystem.
 *
 * The root goes in slot 0 at startup; anything else finds a free one. The
 * slot is filled in before anything reads it because everything that reads a
 * disk reads it through the slot - including, immediately below, the journal.
 */
static int mount_read_sb(void);

static int mount_at(unsigned dev, const char* at)
{
    struct fs* slot = 0;
    for (int i = 0; i < FS_MAX; ++i) {
        if (g_fs[i].used && strcmp(g_fs[i].at, at) == 0)
            return -EBUSY;              /* something is already there */
        if (g_fs[i].used && g_fs[i].dev == dev)
            return -EBUSY;              /* and a disk is mounted once */
        if (!g_fs[i].used && slot == 0)
            slot = &g_fs[i];
    }
    if (slot == 0)
        return -ENOSPC;
    if (strlen(at) >= sizeof(slot->at))
        return -ENAMETOOLONG;

    struct fs* const was = g_cur;
    g_cur = slot;
    memset(slot, 0, sizeof(*slot));
    slot->dev = dev;
    slot->block_size = 1024;
    slot->inode_size = 128;
    slot->desc_size = 32;

    if (mount_read_sb() != 0) {
        memset(slot, 0, sizeof(*slot));
        g_cur = was;
        return -EIO;
    }
    unsigned n = 0;
    while (at[n] != '\0') { slot->at[n] = at[n]; ++n; }
    slot->at[n] = '\0';
    slot->used = 1;
    return 0;
}

static int mount_read_sb(void)
{
    if (disk_read(0, 8, g_cur->dev) != 0)
        return -1;
    const unsigned char* sb = g_sectors->data + 1024;
    if (rd16(sb, 56) != 0xEF53)
        return -1;

    g_block_size = 1024u << rd32(sb, 24);
    if (g_block_size > sizeof(g_block))
        return -1;
    g_inodes_per_group = rd32(sb, 40);
    g_first_data_block = rd32(sb, 20);
    g_inode_size = rd16(sb, 88);
    if (g_inode_size == 0) g_inode_size = 128;
    /* The 64-bit feature is what makes a group descriptor bigger than 32. */
    g_desc_size = (rd32(sb, 96) & 0x80) ? rd16(sb, 254) : 32;
    if (g_desc_size < 32) g_desc_size = 32;
    g_blocks_per_group = rd32(sb, 32);
    const unsigned inodes = rd32(sb, 0);
    g_group_count = g_inodes_per_group ?
        (inodes + g_inodes_per_group - 1) / g_inodes_per_group : 0;
    g_gd_block = g_first_data_block + 1;
    memcpy(g_sb, sb, 1024);

    /* Before anything else reads the tree: a transaction that committed but
     * was not yet written out is part of this filesystem, and every lookup
     * from here on would otherwise be reading a version of it that the last
     * machine to touch it had already moved past. */
    cache_init();               /* now that the block size is known */
    journal_open(g_sb);
    if (journal_replay() != 0)
        printf("vfsd: the journal could not be replayed\n");

    g_mounted = 1;
    return 0;
}

/* The root, at startup. */
static int mount(void)
{
    return mount_at(0, "/") == 0 ? 0 : -1;
}


/* --- checking the filesystem --------------------------------------------------
 *
 * The check lives here rather than in a program of its own because everything
 * it needs is here: the extent walk, the bitmaps, the group descriptors, the
 * block cache. A separate fsck would be a second implementation of the ext4
 * reader, and two readers that disagree is a worse problem than the one the
 * checker is for.
 *
 * What it costs is that this cannot check a filesystem that failed to mount -
 * the case a real fsck exists for. That is a genuine limit and is written down
 * in the manual page rather than papered over.
 *
 * The question a checker answers is whether two accounts of the same thing
 * agree: the tree says which blocks are spoken for, the bitmaps say which are
 * taken, and every disagreement is either a block that will be handed out
 * twice or one that is lost until the disk is emptied.
 */

#define FSCK_MAX_BLOCKS 262144u         /* a 1 GiB disk at 4 KiB blocks */
#define FSCK_MAX_INODES 65536u

static unsigned char g_seen_block[FSCK_MAX_BLOCKS / 8];
static unsigned char g_seen_inode[FSCK_MAX_INODES / 8];

static void fsck_mark(unsigned char* map, unsigned long n, unsigned long max)
{
    if (n < max)
        map[n >> 3] |= (unsigned char)(1u << (n & 7));
}

static int fsck_test(const unsigned char* map, unsigned long n, unsigned long max)
{
    return n < max && (map[n >> 3] & (1u << (n & 7))) != 0;
}

/* Every block an inode's data occupies. The extent tree's own index blocks are
 * not counted: this walks the mapping, not the structure that holds it, so a
 * file large enough to need a second level will show its index block as unused
 * rather than as missing. Noted in the manual page. */
static void fsck_claim_inode(const struct inode* in, unsigned long total)
{
    const unsigned long blocks =
        (in->size + g_block_size - 1) / g_block_size;
    for (unsigned long i = 0; i < blocks; ++i) {
        const unsigned long phys = map_block(in, i);
        if (phys != 0)
            fsck_mark(g_seen_block, phys, total);
    }
}

/* Walk from a directory, claiming everything below it. Depth is bounded
 * because a corrupt tree is exactly where a cycle would be, and a checker that
 * loops forever on a broken disk is not a checker. */
static void fsck_walk(unsigned ino, int depth, unsigned long total,
                      unsigned long inodes, unsigned* files, unsigned* dirs)
{
    if (depth > 24)
        return;
    struct inode in;
    if (read_inode(ino, &in) != 0)
        return;
    fsck_mark(g_seen_inode, ino, inodes);
    fsck_claim_inode(&in, total);

    if (MODE_KIND(in.mode) != MODE_DIR) {
        ++*files;
        return;
    }
    ++*dirs;

    for (unsigned want = 0; ; ++want) {
        char name[64];
        unsigned kind = VFS_KIND_FILE;
        struct inode dir;
        if (read_inode(ino, &dir) != 0)
            return;
        const unsigned child = dir_search(&dir, 0, want, name, &kind);
        if (child == 0)
            return;
        if (fsck_test(g_seen_inode, child, inodes)) {
            /* A second name for one file is normal; a second visit is not a
             * reason to walk it twice. */
            continue;
        }
        fsck_walk(child, depth + 1, total, inodes, files, dirs);
    }
}

/* Append to the report, which rides back in the reply's data. */
static void fsck_say(char* out, unsigned max, unsigned* at, const char* text)
{
    while (*text != '\0' && *at + 1 < max)
        out[(*at)++] = *text++;
    if (*at + 1 < max)
        out[(*at)++] = '\n';
    out[*at] = '\0';
}

static void fsck_say_num(char* out, unsigned max, unsigned* at,
                         const char* before, unsigned long n, const char* after)
{
    char line[128];
    snprintf(line, sizeof(line), "%s%lu%s", before, n, after);
    fsck_say(out, max, at, line);
}

/* Returns the number of problems found; `fixed` counts those put right. */
static long fsck_run(int repair, char* out, unsigned max, unsigned* used,
                     unsigned* fixed)
{
    unsigned at = 0;
    long problems = 0;
    *fixed = 0;
    out[0] = '\0';

    const unsigned long total  = rd32(g_sb, 4);
    const unsigned long inodes = rd32(g_sb, 0);
    if (total > FSCK_MAX_BLOCKS || inodes > FSCK_MAX_INODES) {
        fsck_say(out, max, &at, "fsck: this filesystem is larger than the "
                                "checker can hold a map of");
        *used = at;
        return -1;
    }

    memset(g_seen_block, 0, sizeof(g_seen_block));
    memset(g_seen_inode, 0, sizeof(g_seen_inode));

    /* The metadata first: none of it is reachable from the tree, and all of it
     * is legitimately marked in the bitmaps. Leaving it out would report every
     * bitmap and inode table as a lost block. */
    const unsigned long gd_blocks =
        ((unsigned long)g_group_count * g_desc_size + g_block_size - 1)
        / g_block_size;
    const unsigned long reserved_gdt = rd16(g_sb, 206);
    for (unsigned long b = 0; b < g_gd_block + gd_blocks + reserved_gdt; ++b)
        fsck_mark(g_seen_block, b, total);

    /* And the backup copies. sparse_super keeps one in group 0, group 1, and
     * every group that is a power of three, five or seven; each is a copy of
     * the superblock followed by the descriptors and the room left for them to
     * grow. Nothing points at any of it, which is exactly why it survives the
     * thing that would have destroyed the original. */
    for (unsigned g = 1; g < g_group_count; ++g) {
        unsigned n = g;
        int backup = (g == 1);
        for (unsigned base = 3; base <= 7 && !backup; base += 2) {
            n = base;
            while (n < g)
                n *= base;
            if (n == g)
                backup = 1;
        }
        if (!backup)
            continue;
        const unsigned long at =
            g_first_data_block + (unsigned long)g * g_blocks_per_group;
        for (unsigned long b = 0; b <= gd_blocks + reserved_gdt; ++b)
            fsck_mark(g_seen_block, at + b, total);
    }
    const unsigned long itable_blocks =
        ((unsigned long)g_inodes_per_group * g_inode_size + g_block_size - 1)
        / g_block_size;
    for (unsigned g = 0; g < g_group_count; ++g) {
        unsigned char desc[64];
        if (read_group_desc(g, desc) != 0)
            continue;
        fsck_mark(g_seen_block, rd32(desc, 0), total);   /* block bitmap */
        fsck_mark(g_seen_block, rd32(desc, 4), total);   /* inode bitmap */
        const unsigned long table = rd32(desc, 8);
        for (unsigned long i = 0; i < itable_blocks; ++i)
            fsck_mark(g_seen_block, table + i, total);
    }

    /* The reserved inodes, which the tree does not lead to. The journal is one
     * of them and is thirty-two megabytes; forgetting it would report the
     * whole journal as lost. */
    for (unsigned ino = 1; ino <= 11 && ino <= inodes; ++ino) {
        struct inode in;
        if (read_inode(ino, &in) != 0)
            continue;
        fsck_mark(g_seen_inode, ino, inodes);
        if (ino != 2)                   /* root is walked properly below */
            fsck_claim_inode(&in, total);
    }

    unsigned files = 0, dirs = 0;
    fsck_walk(2, 0, total, inodes, &files, &dirs);
    fsck_say_num(out, max, &at, "reachable: ", dirs, " directories");
    fsck_say_num(out, max, &at, "reachable: ", files, " files");

    /* Now the two accounts, block by block. */
    unsigned long in_use = 0, unmarked = 0, lost = 0;
    for (unsigned g = 0; g < g_group_count; ++g) {
        unsigned char desc[64];
        if (read_group_desc(g, desc) != 0)
            continue;
        if (read_block(rd32(desc, 0), g_block) != 0)
            continue;
        int dirty = 0;
        for (unsigned i = 0; i < g_blocks_per_group; ++i) {
            const unsigned long b =
                g_first_data_block + (unsigned long)g * g_blocks_per_group + i;
            if (b >= total)
                break;
            const int marked = (g_block[i >> 3] & (1u << (i & 7))) != 0;
            const int wanted = fsck_test(g_seen_block, b, total);
            if (marked)
                ++in_use;
            if (wanted && !marked) {
                /* The dangerous direction: this block is part of a file and
                 * the allocator believes it is free, so it will be handed out
                 * again and two files will share it. */
                ++unmarked;
                if (repair) {
                    g_block[i >> 3] |= (unsigned char)(1u << (i & 7));
                    dirty = 1;
                    ++*fixed;
                    ++in_use;
                }
            } else if (marked && !wanted) {
                ++lost;                 /* merely wasted, not unsafe */
            }
        }
        if (dirty)
            write_block(rd32(desc, 0), g_block);
    }

    if (unmarked != 0) {
        fsck_say_num(out, max, &at, "PROBLEM: ", unmarked,
                     repair ? " blocks were in a file but marked free (fixed)"
                            : " blocks are in a file but marked free");
        problems += (long)unmarked;
    }
    if (lost != 0) {
        fsck_say_num(out, max, &at, "note: ", lost,
                     " blocks are marked used but reachable from nothing");
    }

    /* And the superblock's own idea of how much is left. */
    const unsigned long free_now = total - in_use;
    const unsigned long free_said = rd32(g_sb, 12);
    if (free_now != free_said) {
        fsck_say_num(out, max, &at, "PROBLEM: the superblock says ", free_said,
                     " free blocks");
        fsck_say_num(out, max, &at, "         the bitmaps say ", free_now, "");
        ++problems;
        if (repair) {
            wr32w(g_sb, 12, (unsigned)free_now);
            ++*fixed;
            fsck_say(out, max, &at, "         corrected");
        }
    }

    if (problems == 0)
        fsck_say(out, max, &at, "clean");

    *used = at;
    return problems;
}

/* --- the caller's own transfer buffer ----------------------------------------
 *
 * There used to be one shared segment, and one was enough because there used to
 * be one client: the kernel, holding its big lock across the whole operation.
 * With every process a client that breaks, and quietly - the reply is what
 * unblocks the caller, so between replying to one and it copying its bytes out,
 * another can arrive and overwrite them. Nobody would see an error; they would
 * see the wrong file's contents.
 *
 * So the buffer belongs to whoever asked. The message says which segment to use
 * - the field was always there for this - and vfsd maps it on demand and keeps
 * the last few, because a process that reads one block usually reads the next.
 * The global segment stays for the kernel, which still loads programs through
 * here and has no shm of its own to offer. */
#define BUFCACHE 8

static struct {
    unsigned key;
    void*    mapped;
} g_bufs[BUFCACHE];
static unsigned g_buf_next;

static struct vfs_shared* g_global;      /* the kernel's, by the well-known key */

static struct vfs_shared* transfer_buffer(unsigned key)
{
    unsigned i;
    int id;
    void* p;

    if (key == 0 || key == VFS_SHM_KEY)
        return g_global;

    for (i = 0; i < BUFCACHE; ++i)
        if (g_bufs[i].key == key && g_bufs[i].mapped != 0)
            return (struct vfs_shared*)g_bufs[i].mapped;

    id = shm_open(key, sizeof(struct vfs_shared), 0);
    if (id < 0)
        return g_global;                 /* no segment offered; do no harm */
    p = shm_map(id);
    if (p == 0)
        return g_global;

    /* Round-robin eviction. Forgetting a mapping only costs the next mapping;
     * it cannot lose data, because nothing is cached in it. */
    g_bufs[g_buf_next].key = key;
    g_bufs[g_buf_next].mapped = p;
    g_buf_next = (g_buf_next + 1) % BUFCACHE;
    return (struct vfs_shared*)p;
}

/* --- who is asking, and whether they may -------------------------------------
 *
 * This used to be the kernel's job, back when the kernel held the file
 * descriptors: it checked once, at open, and handed out a descriptor that
 * carried the answer. With the table in libc there is nothing trustworthy on
 * that side of the boundary - a process asking for its own file is asking with
 * its own code - so the check belongs here, where the mode bits already are.
 *
 * It lands per operation rather than once per open, which is stricter than
 * what it replaces: a descriptor can no longer outlive the permission that
 * justified it.
 *
 * The uid and gid come from the kernel, about the pid the kernel itself
 * reported as the sender. Nothing in the message is trusted for this, because
 * a uid inside a message is a uid the sender chose. */
static int may_access(const struct inode* in, unsigned caller, int want_write)
{
    const unsigned long creds = (unsigned long)__syscall(SYS_credsof, caller,
                                                         0, 0, 0, 0);
    const unsigned uid = (unsigned)creds;
    const unsigned gid = (unsigned)(creds >> 32);
    unsigned read_bit, write_bit;

    if (uid == 0)
        return 1;                       /* root bypasses the mode bits */

    if (in->uid == uid) {
        read_bit = 0400; write_bit = 0200;
    } else if (in->gid == gid) {
        read_bit = 0040; write_bit = 0020;
    } else {
        read_bit = 0004; write_bit = 0002;
    }
    return (in->mode & (want_write ? write_bit : read_bit)) != 0;
}

static unsigned caller_uid(unsigned caller)
{
    return (unsigned)__syscall(SYS_credsof, caller, 0, 0, 0, 0);
}

int main(void)
{
    for (int i = 0; i < 400 && g_blk <= 0; ++i) {
        g_blk = port_open(IPC_PORT_BLOCK);
        if (g_blk < 0) msleep(10);
    }
    if (g_blk < 0) {
        printf("vfsd: no disk driver is running\n");
        return 1;
    }
    const int shm = shm_open(BLK_SHM_KEY, sizeof(struct blk_shared), 0);
    g_sectors = shm < 0 ? 0 : (struct blk_shared*)shm_map(shm);
    if (g_sectors == 0) {
        printf("vfsd: cannot reach the driver's buffer\n");
        return 1;
    }

    const int vshm = shm_open(VFS_SHM_KEY, sizeof(struct vfs_shared), SHM_PUBLIC);
    g_global = vshm < 0 ? 0 : (struct vfs_shared*)shm_map(vshm);
    if (g_global == 0) {
        printf("vfsd: cannot publish a transfer buffer\n");
        return 1;
    }

    if (mount() != 0) {
        printf("vfsd: no ext4 filesystem on that disk\n");
        return 1;
    }

    const int port = port_create(IPC_PORT_VFS);
    if (port < 0) {
        printf("vfsd: a filesystem is already served\n");
        return 1;
    }
    printf("vfsd[%d]: ext4 mounted, %u-byte blocks, in ring 3\n",
           getpid(), g_block_size);

    for (;;) {
        struct ipc_message m, r;
        unsigned from = 0;
        const int handle = ipc_recv(port, &m, &from);
        struct vfs_shared* out;
        if (handle < 0)
            continue;           /* a failed receive is not a reason to die */

        /* Whose buffer this exchange uses. Chosen per request, because the
         * caller changes per request. */
        out = transfer_buffer((unsigned)m.shm_key);

        memset(&r, 0, sizeof(r));
        r.tag = m.tag;
        /* Every request is a transaction. A read collects nothing and commits
         * nothing, so this costs those exactly one comparison. */
        txn_begin();
        /* The default is "something went wrong and this handler did not say
         * what", which is what -1 used to mean for everything. Handlers that
         * know better set a real code, and most of them do - the difference
         * between "no such file" and "permission denied" is the whole reason
         * for having numbers at all. */
        r.word[0] = -EIO;

        /* /proc first, and before anything touches the disk: none of it is on
         * the disk, and the empty directory that is there only exists so the
         * name is not a lie to `ls /`. */
        if (is_proc((const char*)m.data) &&
            (m.tag == VFS_STAT || m.tag == VFS_LSTAT || m.tag == VFS_READ ||
             m.tag == VFS_LIST)) {
            const char* path = (const char*)m.data;
            unsigned kind = VFS_KIND_FILE;
            unsigned long size = 0;

            if (m.tag == VFS_STAT || m.tag == VFS_LSTAT) {
                r.word[0] = -ENOENT;
                if (proc_kind(path, &kind, &size) == 0) {
                    r.word[0] = (long)size;
                    r.word[1] = (long)kind;
                    /* Readable by everyone and writable by nobody, which is
                     * the truth: these are answers, not storage. */
                    r.word[2] = kind == VFS_KIND_DIR ? 0555 : 0444;
                    r.word[3] = 0;
                }
            } else if (m.tag == VFS_LIST) {
                char name[64];
                r.word[0] = -ENOENT;
                if (proc_entry(path, (unsigned)m.word[1], name,
                               &kind, &size) == 0) {
                    unsigned n = 0;
                    while (name[n] != '\0' && n < sizeof(r.data) - 1) {
                        r.data[n] = name[n];
                        ++n;
                    }
                    r.data[n] = '\0';
                    r.bytes = n;
                    r.word[0] = (long)kind;
                    r.word[1] = (long)size;
                    r.word[2] = kind == VFS_KIND_DIR ? 0555 : 0444;
                    r.word[3] = 0;
                }
            } else {
                char text[PROC_MAX];
                const int len = proc_contents(path, text, sizeof(text));
                r.word[0] = len < 0 ? -ENOENT : 0;
                if (len >= 0 && out != 0) {
                    /* Generated whole and then sliced, because it is a
                     * snapshot: a reader that came back for the second half
                     * of a value that has since changed would get two halves
                     * of two different answers. */
                    unsigned long offset = (unsigned long)m.word[1];
                    unsigned long want = (unsigned long)m.word[2];
                    if (offset >= (unsigned long)len) {
                        r.word[0] = 0;
                    } else {
                        if (want > (unsigned long)len - offset)
                            want = (unsigned long)len - offset;
                        if (want > VFS_CHUNK)
                            want = VFS_CHUNK;
                        memcpy(out->data, text + offset, want);
                        r.word[0] = (long)want;
                    }
                }
            }
        } else if (m.tag == VFS_STATFS) {
            /* Straight out of the superblock, which is held in memory anyway.
             *
             * A mount point that is not storage gets zeros, and df prints a
             * dash for those: /proc has no size, and /dev is a set of names
             * libc answers without asking anybody, so reporting the disk's
             * figures under either would be counting the same megabytes twice
             * under three headings. */
            const char* p = (const char*)m.data;
            const int sized = !is_proc(p) &&
                              !(strncmp(p, "/dev", 4) == 0 &&
                                (p[4] == '\0' || p[4] == '/'));
            r.word[0] = g_block_size;
            r.word[1] = sized ? (long)rd32(g_sb, 4) : 0;
            r.word[2] = sized ? (long)rd32(g_sb, 12) : 0;
            r.word[3] = 0;
        } else if (m.tag == VFS_MOUNTED) {
            r.word[0] = g_mounted;
            r.word[1] = g_block_size;
        } else if (m.tag == VFS_READLINK) {
            struct inode in;
            char target[512];
            r.word[0] = -ENOENT;
            if (lookup_nofollow((const char*)m.data, &in) != 0) {
                if (MODE_KIND(in.mode) != MODE_LINK) {
                    r.word[0] = -EINVAL;        /* not a link, so no target */
                } else if (read_link(&in, target, sizeof(target)) != 0) {
                    r.word[0] = -EIO;
                } else {
                    unsigned n = 0;
                    while (target[n] != '\0' && n < sizeof(r.data) - 1) {
                        r.data[n] = target[n];
                        ++n;
                    }
                    r.data[n] = '\0';
                    r.bytes = n;
                    r.word[0] = (long)n;
                }
            }
        } else if (m.tag == VFS_MKFIFO) {
            r.word[0] = create_node((const char*)m.data, MAKE_FIFO, 0, 0);
        } else if (m.tag == VFS_FSCK) {
            /* Repairing is root's business; looking is anybody's. */
            const int repair = m.word[1] != 0;
            if (repair && caller_uid(from) != 0) {
                r.word[0] = -EPERM;
            } else {
                unsigned used = 0, fixed = 0;
                r.word[0] = fsck_run(repair, (char*)r.data, sizeof(r.data),
                                     &used, &fixed);
                r.word[1] = (long)fixed;
                r.bytes = used;
            }
        } else if (m.tag == VFS_MOUNT) {
            /* Root only: attaching a disk puts somebody else's idea of who
             * owns which file into this tree. */
            if (caller_uid(from) != 0) {
                r.word[0] = -EPERM;
            } else {
                const char* at = (const char*)m.data;
                /* The batch belongs to whichever disk it was collected for,
                 * and this is about to change which that is. */
                txn_flush();
                struct fs* const was = g_cur;
                r.word[0] = mount_at((unsigned)m.word[1], at);
                g_cur = was;
            }
        } else if (m.tag == VFS_UMOUNT) {
            if (caller_uid(from) != 0) {
                r.word[0] = -EPERM;
            } else {
                const char* at = (const char*)m.data;
                r.word[0] = -EINVAL;
                for (int i = 1; i < FS_MAX; ++i) {   /* never the root */
                    if (!g_fs[i].used || strcmp(g_fs[i].at, at) != 0)
                        continue;
                    /* Anything it is still holding goes to its own disk
                     * before the slot stops describing that disk. */
                    struct fs* const was = g_cur;
                    g_cur = &g_fs[i];
                    txn_flush();
                    memset(&g_fs[i], 0, sizeof(g_fs[i]));
                    g_cur = was == &g_fs[i] ? &g_fs[0] : was;
                    r.word[0] = 0;
                    break;
                }
            }
        } else if (m.tag == VFS_MKNOD) {
            /* Root only. The four bytes are harmless; the claim they make
             * about which driver answers is not. */
            r.word[0] = caller_uid(from) != 0 ? -EPERM
                      : create_node((const char*)m.data,
                                    m.word[1] == VFS_KIND_BLK ? MAKE_BLK
                                                              : MAKE_CHR,
                                    0, (unsigned)m.word[2]);
        } else if (m.tag == VFS_LINK) {
            /* Two strings in the data, the existing name first - the same
             * shape as SYMLINK, and for the same reason. */
            const char* existing = (const char*)m.data;
            const char* where = existing + strlen(existing) + 1;
            r.word[0] = link_node(existing, where);
        } else if (m.tag == VFS_SYMLINK) {
            /* Two strings in the data, the target first: it is the one that
             * may be anything at all, including a path to nowhere, so the one
             * that must not be parsed. */
            const char* target = (const char*)m.data;
            const char* where  = target + strlen(target) + 1;
            r.word[0] = create_node(where, 0, target, 0);
        } else if (m.tag == VFS_STAT || m.tag == VFS_LSTAT) {
            struct inode in;
            const int follow = m.tag == VFS_STAT;
            r.word[0] = -ENOENT;
            const unsigned lookup_ino =
                follow ? lookup((const char*)m.data, &in)
                       : lookup_nofollow((const char*)m.data, &in);
            if (lookup_ino != 0) {
                r.word[0] = (long)in.size;
                r.word[1] = MODE_KIND(in.mode) == MODE_DIR  ? VFS_KIND_DIR
                          : MODE_KIND(in.mode) == MODE_LINK ? VFS_KIND_LINK
                          : MODE_KIND(in.mode) == MODE_FIFO ? VFS_KIND_FIFO
                          : MODE_KIND(in.mode) == MODE_CHR  ? VFS_KIND_CHR
                          : MODE_KIND(in.mode) == MODE_BLK  ? VFS_KIND_BLK
                                                            : VFS_KIND_FILE;
                r.word[2] = in.mode & 0777;
                /* Both owners in one word: they are always wanted together
                 * and there are only four to spend. */
                r.word[3] = (long)((in.uid << 16) | in.gid);
                /* The timestamps go in the data, which stat has no other use
                 * for - the four words are spent. */
                {
                    /* And the inode number after them: it is the one name for
                     * a file that is unique and already agreed by both sides,
                     * which is what the two ends of a FIFO need to find each
                     * other - the pipe is in the kernel and has no name. */
                    unsigned extra[5] = { in.mtime, in.ctime, in.atime,
                                          lookup_ino, node_rdev(&in) };
                    memcpy(r.data, extra, sizeof(extra));
                    r.bytes = sizeof(extra);
                }
            }
        } else if (m.tag == VFS_LIST) {
            struct inode in;
            char name[64];
            unsigned kind = VFS_KIND_FILE;
            const unsigned child = lookup((const char*)m.data, &in) != 0
                ? dir_search(&in, 0, (unsigned)m.word[1], name, &kind) : 0;
            if (child != 0) {
                struct inode ci;
                unsigned n = 0;
                while (name[n] != '\0' && n < sizeof(r.data) - 1) {
                    r.data[n] = name[n];
                    ++n;
                }
                r.data[n] = '\0';
                r.bytes = n;
                r.word[0] = (long)kind;
                /* The size as well. dir_search already hands back the child's
                 * inode, so this costs one read that was going to happen
                 * anyway the moment anyone wanted to show a listing - and the
                 * alternative is every caller stat-ing every name it was just
                 * told about, which doubles the round trips for a directory. */
                r.word[1] = read_inode(child, &ci) == 0 ? (long)ci.size : 0;
                /* And the permission bits, from the inode that was just read.
                 * Whether a file is a program is a property of the file, and
                 * the execute bits are where UNIX keeps it - the alternative
                 * is guessing from the name, which is what a .ELF suffix was
                 * doing. */
                r.word[2] = (long)(ci.mode & 0777u);
                r.word[3] = (long)ci.mtime;
            }
        } else if (m.tag == VFS_READ) {
            struct inode in;
            const int found = lookup((const char*)m.data, &in) != 0;
            const int is_dir = found && (in.mode & 0xF000) == 0x4000;
            r.word[0] = !found ? -ENOENT : is_dir ? -EISDIR : -EACCES;
            /* A directory is refused rather than read. Its blocks are entries,
             * not contents, and handing them over as data meant `cat` on a
             * directory printed the raw on-disk records - which looked like a
             * corrupt file rather than a question that should not have been
             * asked. Listing one goes through VFS_LIST. */
            if (found && !is_dir && may_access(&in, from, 0)) {
                unsigned long offset = (unsigned long)m.word[1];
                unsigned long want = (unsigned long)m.word[2];
                if (offset >= in.size) {
                    r.word[0] = 0;              /* the end of the file */
                } else {
                    if (want > in.size - offset) want = in.size - offset;
                    if (want > VFS_CHUNK) want = VFS_CHUNK;
                    unsigned long done = 0;
                    while (done < want) {
                        const unsigned long fb = (offset + done) / g_block_size;
                        const unsigned within = (offset + done) % g_block_size;
                        unsigned long n = g_block_size - within;
                        if (n > want - done) n = want - done;
                        const unsigned long phys = map_block(&in, fb);
                        if (phys == 0) {
                            /* A hole. ext4 does not have to allocate a block
                             * that is entirely zeros and mke2fs does not, so
                             * an unmapped block inside the file is data - all
                             * of it zero - and not the end of anything. Left
                             * as an error this reads short by exactly the
                             * hole, which is how a binary whose last eight
                             * bytes happen to be zero fails to load while
                             * every other binary is fine. */
                            memset(out->data + done, 0, n);
                            done += n;
                            continue;
                        }
                        if (read_block(phys, g_block) != 0)
                            break;
                        memcpy(out->data + done, g_block + within, n);
                        done += n;
                    }
                    r.word[0] = (long)done;
                }
            }
        } else if (m.tag == VFS_CREATE || m.tag == VFS_MKDIR) {
            r.word[0] = create((const char*)m.data, m.tag == VFS_MKDIR);
        } else if (m.tag == VFS_UNLINK) {
            char parent[256], name[64];
            struct inode dir, target;
            unsigned ino = 0, dir_ino = 0, ino_probe = 0;
            int removing_dir = 0;

            /* Look before removing: a directory has to be empty, and finding
             * that out afterwards is too late to put the entry back. */
            /* A path that will not split at all is malformed; one whose
             * parent directory is absent is simply not there. Both used to be
             * "cannot remove", and conflating them here made `rm` of a missing
             * file report an invalid argument. */
            const int split_ok =
                split_path((const char*)m.data, parent, name) == 0;
            int allowed = split_ok && (dir_ino = lookup(parent, &dir)) != 0;
            r.word[0] = split_ok ? -ENOENT : -EINVAL;
            if (allowed) {
                struct inode probe;
                /* Not lookup: removing a name removes *that name*, and a
                 * symbolic link is a name. Following it here would ask
                 * whether the target is an empty directory - a question about
                 * something that is not being removed at all. */
                const unsigned pino = lookup_nofollow((const char*)m.data, &probe);
                ino_probe = pino;
                if (pino == 0) {
                    allowed = 0;
                    r.word[0] = -ENOENT;
                } else if ((probe.mode & 0xF000) == 0x4000) {
                    removing_dir = 1;
                    allowed = dir_is_empty(&probe);
                    /* Not "cannot remove": a directory with things in it is a
                     * different problem from one that is not there, and the
                     * person is about to be told which. */
                    if (!allowed)
                        r.word[0] = -ENOTEMPTY;
                }
                target = probe;
            }
            (void)target;

            /* How many names this inode had before one was taken away. A
             * file with two is not freed by removing one of them - that is
             * the whole point of a hard link, and freeing it anyway leaves
             * the other name pointing at reallocated blocks. */
            const unsigned had = (allowed && ino_probe != 0)
                                     ? inode_links(ino_probe) : 1;

            const int removed = allowed &&
                                dir_remove(&dir, name, &ino) == 0 && ino != 0;

            /* A name went, so the inode has one fewer. Done whether or not the
             * inode is about to be freed, because an fsck reading a count that
             * disagrees with the number of entries pointing at it reports
             * exactly that, and it would be right. */
            if (removed && !removing_dir && had > 1) {
                unsigned long lb; unsigned lat;
                if (inode_location(ino, &lb, &lat) == 0 &&
                    read_block(lb, g_block) == 0) {
                    wr16w(g_block, lat + 26, had - 1);
                    write_block(lb, g_block);
                }
                r.word[0] = 0;          /* the name is gone; the file is not */
            } else if (removed) {
                /* The blocks go back, then the inode. The other order would
                 * leave a freed inode still owning blocks if this stopped
                 * half way, which is the shape of leak fsck cannot repair. */
                struct inode victim;
                if (read_inode(ino, &victim) == 0) {
                    /* A short symbolic link has no blocks: its target lives in
                     * the sixty bytes the extent root would have used. Walking
                     * that as an extent tree would be reading the target text
                     * as block numbers, and freeing whatever they came to. */
                    const int inline_link =
                        MODE_KIND(victim.mode) == MODE_LINK &&
                        victim.size < sizeof(victim.block);
                    const unsigned long n = inline_link ? 0 :
                        (victim.size + g_block_size - 1) / g_block_size;
                    for (unsigned long b = 0; b < n; ++b) {
                        const unsigned long phys = map_block(&victim, b);
                        if (phys != 0)
                            free_block(phys);
                    }
                    memset(&victim, 0, sizeof(victim));
                    write_inode(ino, &victim, 0);
                    /* And a deletion time, which is what marks an inode as
                     * freed rather than never used.
                     *
                     * It has to look like a time. The same field doubles as
                     * the link in the orphan list - the chain of inodes that
                     * were deleted while still open - and fsck tells the two
                     * apart by whether the value could be an inode number. A
                     * small one is read as a link into a list that does not
                     * exist, and reported as a corrupted orphan chain. There
                     * is no clock in this process, so this is a fixed stamp
                     * that is unambiguously not an inode. */
                    unsigned long ib; unsigned io;
                    if (inode_location(ino, &ib, &io) == 0 &&
                        read_block(ib, g_block) == 0) {
                        wr32w(g_block, io + 20, 0x60000000u);
                        write_block(ib, g_block);
                    }
                    /* And the inode bitmap bit, which nothing else clears. */
                    const unsigned g = (ino - 1) / g_inodes_per_group;
                    const unsigned bit = (ino - 1) % g_inodes_per_group;
                    unsigned char desc[64];
                    if (read_group_desc(g, desc) == 0) {
                        static unsigned char bm[8192];
                        const unsigned long bitmap = rd32(desc, 4);
                        if (read_block(bitmap, bm) == 0 &&
                            (bm[bit >> 3] & (1u << (bit & 7))) != 0) {
                            bm[bit >> 3] &= (unsigned char)~(1u << (bit & 7));
                            write_block(bitmap, bm);
                            wr16w(desc, 14, rd16(desc, 14) + 1);
                            write_group_desc(g, desc);
                            wr32w(g_sb, 16, rd32(g_sb, 16) + 1);
                            write_superblock();
                        }
                    }
                }
                if (removing_dir) {
                    /* The parent loses the ".." that pointed back at it, and
                     * the group holds one directory fewer. Both are counts
                     * fsck checks and nothing else reads. */
                    unsigned long pb; unsigned po;
                    if (inode_location(dir_ino, &pb, &po) == 0 &&
                        read_block(pb, g_block) == 0) {
                        const unsigned links = rd16(g_block, po + 26);
                        if (links > 0)
                            wr16w(g_block, po + 26, links - 1);
                        write_block(pb, g_block);
                    }
                    const unsigned g = (ino - 1) / g_inodes_per_group;
                    unsigned char gd[64];
                    if (read_group_desc(g, gd) == 0 && rd16(gd, 16) > 0) {
                        wr16w(gd, 16, rd16(gd, 16) - 1);
                        write_group_desc(g, gd);
                    }
                }
                r.word[0] = 0;
            }
        } else if (m.tag == VFS_WRITE) {
            struct inode in;
            const unsigned ino = lookup((const char*)m.data, &in);
            r.word[0] = ino == 0 ? -ENOENT
                      : (in.mode & 0xF000) == 0x4000 ? -EISDIR : -EACCES;
            if (ino != 0 && may_access(&in, from, 1)) {
                unsigned long offset = (unsigned long)m.word[1];
                unsigned long want = (unsigned long)m.word[2];
                if (want > VFS_CHUNK) want = VFS_CHUNK;
                unsigned long done = 0;
                while (done < want) {
                    const unsigned long fb = (offset + done) / g_block_size;
                    const unsigned within = (offset + done) % g_block_size;
                    unsigned long n = g_block_size - within;
                    if (n > want - done) n = want - done;

                    unsigned long phys = map_block(&in, fb);
                    if (phys == 0) {
                        /* Growing: the file has to be as long as the block
                         * being added before extend() knows where to put it. */
                        const unsigned long was = in.size;
                        in.size = fb * g_block_size;
                        phys = extend(&in, ino);
                        in.size = was;
                        if (phys == 0)
                            break;
                    }
                    if (read_block(phys, g_block) != 0)
                        break;
                    memcpy(g_block + within, out->data + done, n);
                    if (write_block(phys, g_block) != 0)
                        break;
                    done += n;
                    if (offset + done > in.size)
                        in.size = offset + done;
                }
                touch_inode(&in, 1);
                write_inode(ino, &in, inode_links(ino));
                r.word[0] = (long)done;
            }
        } else if (m.tag == VFS_CHMOD || m.tag == VFS_CHOWN) {
            struct inode in;
            const unsigned ino = lookup((const char*)m.data, &in);
            const unsigned who = caller_uid(from);
            /* Changing permissions is the owner's or root's. Giving a file
             * away is root's alone: otherwise a user could dodge a quota, or
             * plant a file owned by someone else. */
            const int allowed = m.tag == VFS_CHMOD ? (who == 0 || in.uid == who)
                                                   : (who == 0);
            r.word[0] = ino == 0 ? -ENOENT : (allowed ? -EIO : -EPERM);
            if (ino != 0 && allowed) {
                if (m.tag == VFS_CHMOD)
                    in.mode = (in.mode & 0xF000) |
                              ((unsigned)m.word[1] & 0777);
                else {
                    in.uid = (unsigned)m.word[1];
                    in.gid = (unsigned)m.word[2];
                }
                r.word[0] = write_inode(ino, &in, inode_links(ino));
            }
        } else if (m.tag == VFS_TRUNC) {
            /* Nothing truncated before this existed, anywhere: O_TRUNC was a
             * no-op and the kernel's "write the whole file" returned early on
             * zero bytes. Writing a shorter file over a longer one therefore
             * left the old tail in place, and what came back was the new
             * contents followed by whatever used to be there - which reads
             * exactly like corruption and is the likeliest thing behind a file
             * that looks garbled after being saved over.
             *
             * The blocks go back before the size does. The other order would
             * leave a file claiming to be empty while still owning them. */
            struct inode in;
            const unsigned ino = lookup((const char*)m.data, &in);
            r.word[0] = ino == 0 ? -ENOENT : -EACCES;
            if (ino != 0 && may_access(&in, from, 1)) {
                const unsigned long n =
                    (in.size + g_block_size - 1) / g_block_size;
                unsigned long b;
                for (b = 0; b < n; ++b) {
                    const unsigned long phys = map_block(&in, b);
                    if (phys != 0)
                        free_block(phys);
                }
                /* An empty extent tree: the header stays, the entries go, or
                 * the next write would follow pointers to freed blocks. */
                memset(in.block, 0, 60);
                touch_inode(&in, 1);
                wr16w(in.block, 0, 0xF30A);     /* magic */
                wr16w(in.block, 2, 0);          /* entries */
                wr16w(in.block, 4, 4);          /* max */
                wr16w(in.block, 6, 0);          /* depth */
                in.size = 0;
                r.word[0] = write_inode(ino, &in, inode_links(ino));
            }
        } else if (m.tag == VFS_RENAME) {
            /* A real rename: the directory entry moves and the inode stays
             * where it is. The kernel used to emulate this by reading the
             * whole file, writing it somewhere else and deleting the original
             * - correct, and honest about being slow, but it also meant a move
             * could half-succeed and it could not move a directory at all.
             * Moving the entry can do neither of those things.
             *
             * The two paths arrive packed as consecutive strings. */
            char oldp[256], oldn[64], newp[256], newn[64];
            const char* second = (const char*)m.data;
            struct inode from_dir, to_dir, victim;
            unsigned from_ino = 0, to_ino = 0, ino = 0;

            while (*second != '\0') ++second;
            ++second;                       /* past the first NUL */

            if (split_path((const char*)m.data, oldp, oldn) == 0 &&
                split_path(second, newp, newn) == 0 &&
                (from_ino = lookup(oldp, &from_dir)) != 0 &&
                (to_ino = lookup(newp, &to_dir)) != 0 &&
                lookup((const char*)m.data, &victim) != 0 &&
                may_access(&victim, from, 1) &&
                dir_search(&to_dir, newn, 0, 0, 0) == 0) {

                const unsigned type =
                    (victim.mode & 0xF000) == 0x4000 ? 2u : 1u;

                /* Same directory is the common case and needs care: the two
                 * inodes are copies of one on-disk directory, so writing the
                 * stale one back afterwards would undo the other's edit. */
                if (from_ino == to_ino) {
                    if (dir_remove(&from_dir, oldn, &ino) == 0 && ino != 0 &&
                        dir_add(&from_dir, from_ino, newn, ino, type) == 0)
                        r.word[0] = write_inode(from_ino, &from_dir,
                                                dir_links(from_ino));
                } else if (dir_remove(&from_dir, oldn, &ino) == 0 && ino != 0) {
                    if (dir_add(&to_dir, to_ino, newn, ino, type) == 0 &&
                        write_inode(to_ino, &to_dir, dir_links(to_ino)) == 0)
                        r.word[0] = write_inode(from_ino, &from_dir,
                                                dir_links(from_ino));
                }
            }
        } else if (m.tag == VFS_SYNC) {
            r.word[0] = write_superblock();
        }
        /* Before the reply, not after: a caller told its write succeeded and
         * then finding it did not is the one outcome worth ruling out. */
        if (txn_end(m.tag == VFS_SYNC) != 0 && r.word[0] >= 0)
            r.word[0] = -EIO;

        ipc_reply(handle, &r);
    }
}
