/* vfsd - the filesystem, outside the kernel.
 *
 * It reads ext4 and owns no hardware: sectors come from blockd, which owns the
 * disk. So a path lookup is this process asking that one, which asks the
 * drive - and none of the three can reach another's memory.
 *
 * Read-only so far. Everything a filesystem does that can lose data - block
 * allocation, growing an extent, inserting into a directory - is the half that
 * has to be right rather than merely working, and putting it in before the
 * read path has been proven against a real disk would mean debugging both at
 * once.
 */

#include <blk.h>
#include <ipc.h>
#include <shm.h>
#include <stdio.h>
#include <string.h>
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
    unsigned long size;
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
    out->size = (unsigned long)rd32(raw, 4) |
                ((unsigned long)rd32(raw, 108) << 32);
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
                } else if (seen++ == want) {
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
    g_mounted = 1;
    return 0;
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
    struct vfs_shared* out = vshm < 0 ? 0 : (struct vfs_shared*)shm_map(vshm);
    if (out == 0) {
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
        if (handle < 0)
            return 1;

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
            }
        } else if (m.tag == VFS_LIST) {
            struct inode in;
            char name[64];
            unsigned kind = VFS_KIND_FILE;
            if (lookup((const char*)m.data, &in) != 0 &&
                dir_search(&in, 0, (unsigned)m.word[1], name, &kind) != 0) {
                unsigned n = 0;
                while (name[n] != '\0' && n < sizeof(r.data) - 1) {
                    r.data[n] = name[n];
                    ++n;
                }
                r.data[n] = '\0';
                r.bytes = n;
                r.word[0] = (long)kind;
            }
        } else if (m.tag == VFS_READ) {
            struct inode in;
            if (lookup((const char*)m.data, &in) != 0) {
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
                        if (phys == 0 || read_block(phys, g_block) != 0)
                            break;
                        memcpy(out->data + done, g_block + within, n);
                        done += n;
                    }
                    r.word[0] = (long)done;
                }
            }
        }
        ipc_reply(handle, &r);
    }
}
