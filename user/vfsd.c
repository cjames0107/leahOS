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
#include <time.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vfsd.h>

/* --- the disk, one level down --------------------------------------------- */

static int g_blk;
static struct blk_shared* g_sectors;

static int disk_read(unsigned long lba, unsigned count)
{
    struct ipc_message q, a;
    memset(&q, 0, sizeof(q));
    memset(&a, 0, sizeof(a));
    q.tag = BLK_READ;
    q.word[0] = (long)lba;
    q.word[1] = (long)count;
    if (ipc_call(g_blk, &q, &a) != 0 || a.word[0] != 0)
        return -1;
    return 0;
}

/* --- ext4, as much of it as reading needs --------------------------------- */

static unsigned g_block_size = 1024;
static unsigned g_inodes_per_group, g_inode_size = 128, g_desc_size = 32;
static unsigned g_first_data_block;
static int g_mounted;

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

static int read_block(unsigned long block, unsigned char* out)
{
    const unsigned sectors = g_block_size / BLK_SECTOR;
    if (disk_read(block * sectors, sectors) != 0)
        return -1;
    memcpy(out, g_sectors->data, g_block_size);
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

static unsigned char g_sb[1024];        /* the superblock, kept in memory */
static unsigned g_blocks_per_group, g_group_count;
static unsigned long g_gd_block;

static void wr16w(unsigned char* p, unsigned at, unsigned v)
{
    p[at] = (unsigned char)v; p[at + 1] = (unsigned char)(v >> 8);
}
static void wr32w(unsigned char* p, unsigned at, unsigned v)
{
    p[at] = (unsigned char)v;         p[at + 1] = (unsigned char)(v >> 8);
    p[at + 2] = (unsigned char)(v >> 16); p[at + 3] = (unsigned char)(v >> 24);
}

static int disk_write(unsigned long lba, unsigned count)
{
    struct ipc_message q, a;
    memset(&q, 0, sizeof(q));
    memset(&a, 0, sizeof(a));
    q.tag = BLK_WRITE;
    q.word[0] = (long)lba;
    q.word[1] = (long)count;
    if (ipc_call(g_blk, &q, &a) != 0 || a.word[0] != 0)
        return -1;
    return 0;
}

static int write_block(unsigned long block, const unsigned char* in)
{
    const unsigned sectors = g_block_size / BLK_SECTOR;
    memcpy(g_sectors->data, in, g_block_size);
    return disk_write(block * sectors, sectors);
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
                        *kind_out = (type == 2) ? VFS_KIND_DIR : VFS_KIND_FILE;
                    return ino;
                }
            }
            at += rec;
        }
    }
    return 0;
}

/* Resolve an absolute path to an inode number. */
static unsigned lookup(const char* path, struct inode* out)
{
    unsigned ino = ROOT_INODE;
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
        ino = dir_search(out, part, 0, 0, 0);
        if (ino == 0 || read_inode(ino, out) != 0)
            return 0;
    }
    return ino;
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
static int create(const char* path, int is_dir)
{
    char parent[256], name[64];
    if (split_path(path, parent, name) != 0)
        return -1;
    struct inode dir;
    const unsigned dir_ino = lookup(parent, &dir);
    if (dir_ino == 0)
        return -1;
    if (dir_search(&dir, name, 0, 0, 0) != 0)
        return -1;                      /* it is already there */

    const unsigned ino = alloc_inode(is_dir);
    if (ino == 0)
        return -1;

    struct inode fresh;
    memset(&fresh, 0, sizeof(fresh));
    fresh.mode = is_dir ? (0040755) : (0100644);
    fresh.size = 0;
    touch_inode(&fresh, 1);
    /* An empty extent root: the magic, no entries, and room for four. */
    wr16w(fresh.block, 0, 0xF30A);
    wr16w(fresh.block, 2, 0);
    wr16w(fresh.block, 4, 4);
    wr16w(fresh.block, 6, 0);
    if (write_inode(ino, &fresh, is_dir ? 2u : 1u) != 0)
        return -1;

    if (is_dir) {
        /* A directory is not empty on disk: it holds itself and its parent. */
        const unsigned long phys = extend(&fresh, ino);
        if (phys == 0)
            return -1;
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
            return -1;
    }

    if (dir_add(&dir, dir_ino, name, ino, is_dir ? 2 : 1) != 0)
        return -1;
    if (is_dir) {
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

static int mount(void)
{
    if (disk_read(0, 8) != 0)
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
    g_mounted = 1;
    return 0;
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
        r.word[0] = -1;

        if (m.tag == VFS_MOUNTED) {
            r.word[0] = g_mounted;
            r.word[1] = g_block_size;
        } else if (m.tag == VFS_STAT) {
            struct inode in;
            if (lookup((const char*)m.data, &in) != 0) {
                r.word[0] = (long)in.size;
                r.word[1] = (in.mode & 0xF000) == 0x4000 ? VFS_KIND_DIR
                                                         : VFS_KIND_FILE;
                r.word[2] = in.mode & 0777;
                /* Both owners in one word: they are always wanted together
                 * and there are only four to spend. */
                r.word[3] = (long)((in.uid << 16) | in.gid);
                /* The timestamps go in the data, which stat has no other use
                 * for - the four words are spent. */
                {
                    unsigned stamps[3] = { in.mtime, in.ctime, in.atime };
                    memcpy(r.data, stamps, sizeof(stamps));
                    r.bytes = sizeof(stamps);
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
            if (lookup((const char*)m.data, &in) != 0 &&
                may_access(&in, from, 0)) {
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
            unsigned ino = 0, dir_ino = 0;
            int removing_dir = 0;

            /* Look before removing: a directory has to be empty, and finding
             * that out afterwards is too late to put the entry back. */
            int allowed = split_path((const char*)m.data, parent, name) == 0 &&
                          (dir_ino = lookup(parent, &dir)) != 0;
            if (allowed) {
                struct inode probe;
                const unsigned pino = lookup((const char*)m.data, &probe);
                if (pino == 0)
                    allowed = 0;
                else if ((probe.mode & 0xF000) == 0x4000) {
                    removing_dir = 1;
                    allowed = dir_is_empty(&probe);
                }
                target = probe;
            }
            (void)target;

            if (allowed && dir_remove(&dir, name, &ino) == 0 && ino != 0) {
                /* The blocks go back, then the inode. The other order would
                 * leave a freed inode still owning blocks if this stopped
                 * half way, which is the shape of leak fsck cannot repair. */
                struct inode victim;
                if (read_inode(ino, &victim) == 0) {
                    const unsigned long n =
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
        ipc_reply(handle, &r);
    }
}
