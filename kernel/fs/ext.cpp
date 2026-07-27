#include <leah/ext.hpp>
#include <leah/heap.hpp>
#include <leah/string.hpp>

namespace fs {
namespace {

// Little-endian field reads. ext is little-endian on disk and x86 is too, but
// going through these keeps the access byte-exact and the offsets explicit,
// rather than trusting a packed struct's padding.
inline u16 rd16(const u8* b, usize o)
{
    return static_cast<u16>(b[o] | b[o + 1] << 8);
}
inline u32 rd32(const u8* b, usize o)
{
    return static_cast<u32>(b[o]) | static_cast<u32>(b[o + 1]) << 8 |
           static_cast<u32>(b[o + 2]) << 16 | static_cast<u32>(b[o + 3]) << 24;
}
inline void wr16(u8* b, usize o, u16 v)
{
    b[o] = static_cast<u8>(v);
    b[o + 1] = static_cast<u8>(v >> 8);
}
inline void wr32(u8* b, usize o, u32 v)
{
    b[o] = static_cast<u8>(v);
    b[o + 1] = static_cast<u8>(v >> 8);
    b[o + 2] = static_cast<u8>(v >> 16);
    b[o + 3] = static_cast<u8>(v >> 24);
}

constexpr u16 kMagic     = 0xEF53;
constexpr u32 kRootInode = 2;

constexpr u32 kIncompatFiletype = 0x0002;
constexpr u32 kIncompat64Bit    = 0x0080;

constexpr u32 kInodeExtentsFlag = 0x00080000;   // i_flags: file uses extents
constexpr u16 kExtentMagic      = 0xF30A;

constexpr u16 kModeFmt = 0xF000;
constexpr u16 kModeDir = 0x4000;
constexpr u16 kModeReg = 0x8000;

constexpr u8 kDirTypeDir = 2;

inline u32 min32(u32 a, u32 b) { return a < b ? a : b; }

} // namespace

Ext::~Ext() = default;

Ext* Ext::probe(block::Device* device)
{
    Ext* filesystem = new Ext();
    if (!filesystem->mount(device)) {
        delete filesystem;
        return nullptr;
    }
    return filesystem;
}

bool Ext::mount(block::Device* device)
{
    m_device = device;

    // The superblock always sits at byte offset 1024, whatever the block size,
    // so read it as two 512-byte sectors regardless. Cache it so free-count
    // updates can be flushed cheaply.
    u8* sb = m_sb;
    if (!device->read(2, 2, sb))
        return false;
    if (rd16(sb, 56) != kMagic)
        return false;

    const u32 log_block_size = rd32(sb, 24);
    m_block_size = 1024u << log_block_size;
    if (m_block_size < 1024 || m_block_size > 4096)
        return false;                       // 1 KiB - 4 KiB blocks supported
    m_sectors_per_block = m_block_size / block::kSectorSize;

    m_inodes_per_group = rd32(sb, 40);
    m_blocks_per_group = rd32(sb, 32);
    m_first_data_block = rd32(sb, 20);
    m_inode_size       = rd16(sb, 88);
    if (m_inode_size == 0)
        m_inode_size = 128;
    m_feature_incompat = rd32(sb, 96);
    m_has_filetype     = (m_feature_incompat & kIncompatFiletype) != 0;
    m_desc_size        = (m_feature_incompat & kIncompat64Bit) ? rd16(sb, 254) : 32;
    if (m_desc_size < 32)
        m_desc_size = 32;
    m_first_ino = rd32(sb, 84);
    if (m_first_ino == 0)
        m_first_ino = 11;

    if (m_inodes_per_group == 0)
        return false;
    const u32 inodes_count = rd32(sb, 0);
    m_group_count = (inodes_count + m_inodes_per_group - 1) / m_inodes_per_group;

    memcpy(m_label, sb + 120, 16);
    m_label[16] = '\0';
    return true;
}

bool Ext::read_block(u64 block, void* buffer) const
{
    return m_device->read(block * m_sectors_per_block, m_sectors_per_block, buffer);
}

bool Ext::write_block(u64 block, const void* buffer) const
{
    return m_device->write(block * m_sectors_per_block, m_sectors_per_block, buffer);
}

bool Ext::read_group_desc(u32 group, u8* desc) const
{
    if (group >= m_group_count)
        return false;
    // The descriptor table begins in the block after the superblock's block.
    const u64 table = m_first_data_block + 1;
    const u64 byte  = static_cast<u64>(group) * m_desc_size;
    const u64 block = table + byte / m_block_size;

    u8* buf = static_cast<u8*>(kmalloc(m_block_size));
    if (buf == nullptr)
        return false;
    bool ok = read_block(block, buf);
    if (ok)
        memcpy(desc, buf + byte % m_block_size, m_desc_size);
    kfree(buf);
    return ok;
}

bool Ext::read_inode(u32 number, Inode& out) const
{
    if (number == 0)
        return false;
    const u32 group = (number - 1) / m_inodes_per_group;
    const u32 index = (number - 1) % m_inodes_per_group;

    u8 desc[64];
    if (!read_group_desc(group, desc))
        return false;
    // bg_inode_table_lo at offset 8; _hi at 40 when descriptors are 64 bytes.
    u64 inode_table = rd32(desc, 8);
    if (m_desc_size >= 64)
        inode_table |= static_cast<u64>(rd32(desc, 40)) << 32;

    const u64 byte  = static_cast<u64>(index) * m_inode_size;
    const u64 block = inode_table + byte / m_block_size;
    const u32 boff  = byte % m_block_size;

    u8* buf = static_cast<u8*>(kmalloc(m_block_size));
    if (buf == nullptr)
        return false;
    if (!read_block(block, buf)) {
        kfree(buf);
        return false;
    }
    const u8* i = buf + boff;

    out.number = number;
    out.mode   = rd16(i, 0);
    out.uid    = rd16(i, 2);
    out.size   = rd32(i, 4);
    out.gid    = rd16(i, 24);
    out.links    = rd16(i, 26);
    out.i_blocks = rd32(i, 28);
    out.flags    = rd32(i, 32);
    // i_size_high (offset 108) holds the high 32 bits of a regular file's size.
    if ((out.mode & kModeFmt) == kModeReg)
        out.size |= static_cast<u64>(rd32(i, 108)) << 32;
    for (int k = 0; k < 15; ++k)
        out.block[k] = rd32(i, 40 + k * 4);

    kfree(buf);
    return true;
}

u64 Ext::map_block(const Inode& inode, u64 file_block) const
{
    if (inode.flags & kInodeExtentsFlag)
        return map_extent(inode, file_block);
    return map_indirect(inode, file_block);
}

u64 Ext::map_extent(const Inode& inode, u64 file_block) const
{
    // The root of the extent tree lives inline in the 60-byte i_block area;
    // interior nodes are read into `disk`.
    u8 inline_node[60];
    memcpy(inline_node, inode.block, 60);
    u8* disk = nullptr;
    const u8* node = inline_node;

    u64 result = 0;
    for (;;) {
        if (rd16(node, 0) != kExtentMagic)
            break;
        const u16 entries = rd16(node, 2);
        const u16 depth   = rd16(node, 6);

        if (depth == 0) {
            for (u16 e = 0; e < entries; ++e) {
                const u8* ent = node + 12 + e * 12;
                const u32 ee_block = rd32(ent, 0);
                u16 ee_len         = rd16(ent, 4);
                const u16 start_hi = rd16(ent, 6);
                const u32 start_lo = rd32(ent, 8);
                if (ee_len > 32768)             // uninitialised extent
                    ee_len = static_cast<u16>(ee_len - 32768);
                if (file_block >= ee_block && file_block < ee_block + ee_len) {
                    const u64 start = static_cast<u64>(start_hi) << 32 | start_lo;
                    result = start + (file_block - ee_block);
                    break;
                }
            }
            break;
        }

        // Interior node: pick the last index whose ei_block <= file_block.
        u64 child = 0;
        bool found = false;
        for (u16 e = 0; e < entries; ++e) {
            const u8* ent = node + 12 + e * 12;
            const u32 ei_block = rd32(ent, 0);
            if (ei_block <= file_block) {
                child = rd32(ent, 4);
                child |= static_cast<u64>(rd16(ent, 8)) << 32;
                found = true;
            } else {
                break;
            }
        }
        if (!found)
            break;
        if (disk == nullptr) {
            disk = static_cast<u8*>(kmalloc(m_block_size));
            if (disk == nullptr)
                break;
        }
        if (!read_block(child, disk))
            break;
        node = disk;
    }

    if (disk != nullptr)
        kfree(disk);
    return result;
}

u32 Ext::indirect_lookup(u64 block, u64 index) const
{
    if (block == 0)
        return 0;
    u8* buf = static_cast<u8*>(kmalloc(m_block_size));
    if (buf == nullptr)
        return 0;
    u32 value = 0;
    if (read_block(block, buf))
        value = rd32(buf, index * 4);
    kfree(buf);
    return value;
}

u64 Ext::map_indirect(const Inode& inode, u64 file_block) const
{
    const u64 ppb = m_block_size / 4;       // pointers per block

    if (file_block < 12)
        return inode.block[file_block];
    file_block -= 12;

    if (file_block < ppb)
        return indirect_lookup(inode.block[12], file_block);
    file_block -= ppb;

    if (file_block < ppb * ppb) {
        const u32 mid = indirect_lookup(inode.block[13], file_block / ppb);
        return indirect_lookup(mid, file_block % ppb);
    }
    file_block -= ppb * ppb;

    const u32 l1 = indirect_lookup(inode.block[14], file_block / (ppb * ppb));
    const u64 rem = file_block % (ppb * ppb);
    const u32 l2 = indirect_lookup(l1, rem / ppb);
    return indirect_lookup(l2, rem % ppb);
}

u32 Ext::lookup(const Inode& dir, const char* name) const
{
    if ((dir.mode & kModeFmt) != kModeDir)
        return 0;
    const usize want = strlen(name);

    u8* blk = static_cast<u8*>(kmalloc(m_block_size));
    if (blk == nullptr)
        return 0;

    u32 found = 0;
    const u64 blocks = (dir.size + m_block_size - 1) / m_block_size;
    for (u64 fb = 0; fb < blocks && found == 0; ++fb) {
        const u64 phys = map_block(dir, fb);
        if (phys == 0 || !read_block(phys, blk))
            continue;
        u32 off = 0;
        while (off + 8 <= m_block_size) {
            const u32 e_inode  = rd32(blk, off);
            const u16 rec_len  = rd16(blk, off + 4);
            const u8  name_len = blk[off + 6];
            if (rec_len < 8)
                break;
            if (e_inode != 0 && name_len == want &&
                memcmp(blk + off + 8, name, want) == 0) {
                found = e_inode;
                break;
            }
            off += rec_len;
        }
    }
    kfree(blk);
    return found;
}

bool Ext::resolve(const char* path, Inode& out) const
{
    Inode cur;
    if (!read_inode(kRootInode, cur))
        return false;

    const char* p = path;
    while (*p == '/')
        ++p;

    while (*p != '\0') {
        char comp[vfs::kMaxName];
        usize n = 0;
        while (*p != '\0' && *p != '/' && n < sizeof(comp) - 1)
            comp[n++] = *p++;
        comp[n] = '\0';
        while (*p == '/')
            ++p;
        if (n == 0)
            break;

        const u32 child = lookup(cur, comp);
        if (child == 0)
            return false;
        if (!read_inode(child, cur))
            return false;
    }

    out = cur;
    return true;
}

bool Ext::stat(const char* path, vfs::Stat& out)
{
    Inode inode;
    if (!resolve(path, inode))
        return false;
    out.type = (inode.mode & kModeFmt) == kModeDir ? vfs::Type::Directory
                                                   : vfs::Type::File;
    out.size = inode.size;
    return true;
}

isize Ext::read(const char* path, u64 offset, void* buffer, usize bytes)
{
    Inode inode;
    if (!resolve(path, inode))
        return -1;
    if ((inode.mode & kModeFmt) != kModeReg)
        return -1;
    if (offset >= inode.size)
        return 0;
    if (offset + bytes > inode.size)
        bytes = static_cast<usize>(inode.size - offset);

    u8* blk = static_cast<u8*>(kmalloc(m_block_size));
    if (blk == nullptr)
        return -1;

    usize done = 0;
    while (done < bytes) {
        const u64 pos    = offset + done;
        const u64 fblock = pos / m_block_size;
        const u32 boff   = pos % m_block_size;
        const u32 chunk  = min32(m_block_size - boff, static_cast<u32>(bytes - done));

        const u64 phys = map_block(inode, fblock);
        if (phys == 0) {
            memset(static_cast<u8*>(buffer) + done, 0, chunk);   // sparse hole
        } else {
            if (!read_block(phys, blk)) {
                kfree(blk);
                return -1;
            }
            memcpy(static_cast<u8*>(buffer) + done, blk + boff, chunk);
        }
        done += chunk;
    }
    kfree(blk);
    return static_cast<isize>(done);
}

bool Ext::list(const char* path, vfs::Entry* out, usize max, usize& count)
{
    count = 0;
    Inode dir;
    if (!resolve(path, dir))
        return false;
    if ((dir.mode & kModeFmt) != kModeDir)
        return false;

    u8* blk = static_cast<u8*>(kmalloc(m_block_size));
    if (blk == nullptr)
        return false;

    const u64 blocks = (dir.size + m_block_size - 1) / m_block_size;
    for (u64 fb = 0; fb < blocks && count < max; ++fb) {
        const u64 phys = map_block(dir, fb);
        if (phys == 0 || !read_block(phys, blk))
            continue;
        u32 off = 0;
        while (off + 8 <= m_block_size && count < max) {
            const u32 e_inode  = rd32(blk, off);
            const u16 rec_len  = rd16(blk, off + 4);
            const u8  name_len = blk[off + 6];
            const u8  type     = blk[off + 7];
            if (rec_len < 8)
                break;

            const bool dot = e_inode != 0 &&
                ((name_len == 1 && blk[off + 8] == '.') ||
                 (name_len == 2 && blk[off + 8] == '.' && blk[off + 9] == '.'));
            if (e_inode != 0 && name_len > 0 && !dot) {
                vfs::Entry& en = out[count];
                usize n = name_len < vfs::kMaxName - 1 ? name_len : vfs::kMaxName - 1;
                memcpy(en.name, blk + off + 8, n);
                en.name[n] = '\0';

                Inode child;
                const bool have = read_inode(e_inode, child);
                if (m_has_filetype)
                    en.type = type == kDirTypeDir ? vfs::Type::Directory
                                                  : vfs::Type::File;
                else
                    en.type = have && (child.mode & kModeFmt) == kModeDir
                                  ? vfs::Type::Directory : vfs::Type::File;
                en.size = have ? child.size : 0;
                ++count;
            }
            off += rec_len;
        }
    }
    kfree(blk);
    return true;
}

// --- write path ------------------------------------------------------------

bool Ext::inode_location(u32 number, u64& block, u32& offset) const
{
    if (number == 0)
        return false;
    const u32 group = (number - 1) / m_inodes_per_group;
    const u32 index = (number - 1) % m_inodes_per_group;
    u8 desc[64];
    if (!read_group_desc(group, desc))
        return false;
    u64 table = rd32(desc, 8);
    if (m_desc_size >= 64)
        table |= static_cast<u64>(rd32(desc, 40)) << 32;
    const u64 byte = static_cast<u64>(index) * m_inode_size;
    block  = table + byte / m_block_size;
    offset = static_cast<u32>(byte % m_block_size);
    return true;
}

bool Ext::write_inode(const Inode& inode) const
{
    u64 block;
    u32 boff;
    if (!inode_location(inode.number, block, boff))
        return false;

    u8* buf = static_cast<u8*>(kmalloc(m_block_size));
    if (buf == nullptr)
        return false;
    if (!read_block(block, buf)) {
        kfree(buf);
        return false;
    }
    u8* i = buf + boff;
    wr16(i, 0, inode.mode);
    wr16(i, 2, inode.uid);
    wr32(i, 4, static_cast<u32>(inode.size));
    // i_dtime (offset 20): nonzero exactly when the inode has been deleted.
    wr32(i, 20, inode.links == 0 ? 1u : 0u);
    wr16(i, 24, inode.gid);
    wr16(i, 26, inode.links);
    wr32(i, 28, inode.i_blocks);
    wr32(i, 32, inode.flags);
    if ((inode.mode & kModeFmt) == kModeReg)
        wr32(i, 108, static_cast<u32>(inode.size >> 32));
    for (int k = 0; k < 15; ++k)
        wr32(i, 40 + k * 4, inode.block[k]);
    // A 256-byte inode carries i_extra_isize at offset 128; mke2fs uses 32.
    if (m_inode_size >= 256)
        wr16(i, 128, 32);

    bool ok = write_block(block, buf);
    kfree(buf);
    return ok;
}

bool Ext::write_superblock() const
{
    return m_device->write(2, 2, m_sb);
}

bool Ext::write_group_desc(u32 group, const u8* desc) const
{
    if (group >= m_group_count)
        return false;
    const u64 table = m_first_data_block + 1;
    const u64 byte  = static_cast<u64>(group) * m_desc_size;
    const u64 block = table + byte / m_block_size;

    u8* buf = static_cast<u8*>(kmalloc(m_block_size));
    if (buf == nullptr)
        return false;
    bool ok = read_block(block, buf);
    if (ok) {
        memcpy(buf + byte % m_block_size, desc, m_desc_size);
        ok = write_block(block, buf);
    }
    kfree(buf);
    return ok;
}

u64 Ext::alloc_block()
{
    for (u32 g = 0; g < m_group_count; ++g) {
        u8 desc[64];
        if (!read_group_desc(g, desc))
            continue;
        const u16 freeb = rd16(desc, 12);
        if (freeb == 0)
            continue;
        u64 bitmap = rd32(desc, 0);
        if (m_desc_size >= 64)
            bitmap |= static_cast<u64>(rd32(desc, 32)) << 32;

        u8* bm = static_cast<u8*>(kmalloc(m_block_size));
        if (bm == nullptr)
            return 0;
        if (!read_block(bitmap, bm)) {
            kfree(bm);
            continue;
        }
        for (u32 bit = 0; bit < m_blocks_per_group; ++bit) {
            if (bm[bit >> 3] & (1u << (bit & 7)))
                continue;
            bm[bit >> 3] |= static_cast<u8>(1u << (bit & 7));
            if (!write_block(bitmap, bm)) {
                kfree(bm);
                return 0;
            }
            kfree(bm);
            wr16(desc, 12, static_cast<u16>(freeb - 1));
            write_group_desc(g, desc);
            wr32(m_sb, 12, rd32(m_sb, 12) - 1);     // s_free_blocks_count_lo
            write_superblock();
            return static_cast<u64>(m_first_data_block) +
                   static_cast<u64>(g) * m_blocks_per_group + bit;
        }
        kfree(bm);
    }
    return 0;
}

void Ext::free_block(u64 block)
{
    if (block < m_first_data_block)
        return;
    const u64 rel = block - m_first_data_block;
    const u32 g   = static_cast<u32>(rel / m_blocks_per_group);
    const u32 bit = static_cast<u32>(rel % m_blocks_per_group);
    if (g >= m_group_count)
        return;

    u8 desc[64];
    if (!read_group_desc(g, desc))
        return;
    u64 bitmap = rd32(desc, 0);
    if (m_desc_size >= 64)
        bitmap |= static_cast<u64>(rd32(desc, 32)) << 32;

    u8* bm = static_cast<u8*>(kmalloc(m_block_size));
    if (bm == nullptr)
        return;
    if (read_block(bitmap, bm) && (bm[bit >> 3] & (1u << (bit & 7)))) {
        bm[bit >> 3] &= static_cast<u8>(~(1u << (bit & 7)));
        write_block(bitmap, bm);
        wr16(desc, 12, static_cast<u16>(rd16(desc, 12) + 1));
        write_group_desc(g, desc);
        wr32(m_sb, 12, rd32(m_sb, 12) + 1);
        write_superblock();
    }
    kfree(bm);
}

u32 Ext::alloc_inode(bool is_dir)
{
    for (u32 g = 0; g < m_group_count; ++g) {
        u8 desc[64];
        if (!read_group_desc(g, desc))
            continue;
        const u16 freei = rd16(desc, 14);
        if (freei == 0)
            continue;
        u64 bitmap = rd32(desc, 4);
        if (m_desc_size >= 64)
            bitmap |= static_cast<u64>(rd32(desc, 36)) << 32;

        u8* bm = static_cast<u8*>(kmalloc(m_block_size));
        if (bm == nullptr)
            return 0;
        if (!read_block(bitmap, bm)) {
            kfree(bm);
            continue;
        }
        for (u32 bit = 0; bit < m_inodes_per_group; ++bit) {
            if (bm[bit >> 3] & (1u << (bit & 7)))
                continue;
            const u32 number = g * m_inodes_per_group + bit + 1;
            if (number < m_first_ino)
                continue;                           // never the reserved inodes
            bm[bit >> 3] |= static_cast<u8>(1u << (bit & 7));
            if (!write_block(bitmap, bm)) {
                kfree(bm);
                return 0;
            }
            kfree(bm);
            wr16(desc, 14, static_cast<u16>(freei - 1));
            if (is_dir)
                wr16(desc, 16, static_cast<u16>(rd16(desc, 16) + 1));
            write_group_desc(g, desc);
            wr32(m_sb, 16, rd32(m_sb, 16) - 1);     // s_free_inodes_count
            write_superblock();

            // Hand back a clean inode slot: a previously deleted inode may have
            // left stale bytes behind.
            u64 iblock;
            u32 ioff;
            if (inode_location(number, iblock, ioff)) {
                u8* buf = static_cast<u8*>(kmalloc(m_block_size));
                if (buf != nullptr && read_block(iblock, buf)) {
                    memset(buf + ioff, 0, m_inode_size);
                    write_block(iblock, buf);
                }
                kfree(buf);
            }
            return number;
        }
        kfree(bm);
    }
    return 0;
}

void Ext::free_inode(u32 number, bool was_dir)
{
    if (number == 0)
        return;
    const u32 g   = (number - 1) / m_inodes_per_group;
    const u32 bit = (number - 1) % m_inodes_per_group;
    if (g >= m_group_count)
        return;

    u8 desc[64];
    if (!read_group_desc(g, desc))
        return;
    u64 bitmap = rd32(desc, 4);
    if (m_desc_size >= 64)
        bitmap |= static_cast<u64>(rd32(desc, 36)) << 32;

    u8* bm = static_cast<u8*>(kmalloc(m_block_size));
    if (bm == nullptr)
        return;
    if (read_block(bitmap, bm) && (bm[bit >> 3] & (1u << (bit & 7)))) {
        bm[bit >> 3] &= static_cast<u8>(~(1u << (bit & 7)));
        write_block(bitmap, bm);
        wr16(desc, 14, static_cast<u16>(rd16(desc, 14) + 1));
        if (was_dir)
            wr16(desc, 16, static_cast<u16>(rd16(desc, 16) - 1));
        write_group_desc(g, desc);
        wr32(m_sb, 16, rd32(m_sb, 16) + 1);
        write_superblock();

        // Leave the inode slot pristine, exactly as mke2fs leaves unused ones.
        // A half-cleared "deleted" inode (links 0 but extent/size intact) is
        // what e2fsck's orphan recovery would otherwise mistake for an orphan.
        u64 iblock;
        u32 ioff;
        if (inode_location(number, iblock, ioff)) {
            u8* buf = static_cast<u8*>(kmalloc(m_block_size));
            if (buf != nullptr && read_block(iblock, buf)) {
                memset(buf + ioff, 0, m_inode_size);
                write_block(iblock, buf);
            }
            kfree(buf);
        }
    }
    kfree(bm);
}

bool Ext::add_extent(Inode& inode, u32 logical, u64 phys) const
{
    u8* eb = reinterpret_cast<u8*>(inode.block);
    if (rd16(eb, 0) != kExtentMagic || rd16(eb, 6) != 0)
        return false;                               // inline depth-0 tree only
    u16 entries = rd16(eb, 2);
    const u16 max_entries = rd16(eb, 4);

    if (entries > 0) {
        u8* last = eb + 12 + (entries - 1) * 12;
        const u32 ee_block = rd32(last, 0);
        const u16 ee_len   = rd16(last, 4);
        const u64 start = static_cast<u64>(rd16(last, 6)) << 32 | rd32(last, 8);
        if (logical == ee_block + ee_len && phys == start + ee_len &&
            ee_len < 32768) {
            wr16(last, 4, static_cast<u16>(ee_len + 1));    // extend in place
            return true;
        }
    }
    if (entries >= max_entries)
        return false;                               // inline extents exhausted

    u8* e = eb + 12 + entries * 12;
    wr32(e, 0, logical);
    wr16(e, 4, 1);
    wr16(e, 6, static_cast<u16>(phys >> 32));
    wr32(e, 8, static_cast<u32>(phys));
    wr16(eb, 2, static_cast<u16>(entries + 1));
    return true;
}

u64 Ext::ensure_block(Inode& inode, u64 logical)
{
    const u64 existing = map_block(inode, logical);
    if (existing != 0)
        return existing;

    const u64 phys = alloc_block();
    if (phys == 0)
        return 0;
    if (!(inode.flags & kInodeExtentsFlag) ||
        !add_extent(inode, static_cast<u32>(logical), phys)) {
        free_block(phys);
        return 0;
    }
    inode.i_blocks += m_sectors_per_block;

    // Zero the fresh block so any slack left by a partial write reads back clean.
    u8* z = static_cast<u8*>(kmalloc(m_block_size));
    if (z != nullptr) {
        memset(z, 0, m_block_size);
        write_block(phys, z);
        kfree(z);
    }
    return phys;
}

void Ext::free_inode_blocks(const Inode& inode)
{
    const u64 blocks = (inode.size + m_block_size - 1) / m_block_size;
    for (u64 fb = 0; fb < blocks; ++fb) {
        const u64 phys = map_block(inode, fb);
        if (phys != 0)
            free_block(phys);
    }
}

bool Ext::dir_insert(Inode& dir, const char* name, u32 child, u8 type)
{
    const usize nlen = strlen(name);
    if (nlen == 0 || nlen > 255)
        return false;
    const u32 needed = static_cast<u32>(8 + ((nlen + 3) & ~3u));

    u8* blk = static_cast<u8*>(kmalloc(m_block_size));
    if (blk == nullptr)
        return false;

    const u64 blocks = (dir.size + m_block_size - 1) / m_block_size;
    for (u64 fb = 0; fb < blocks; ++fb) {
        const u64 phys = map_block(dir, fb);
        if (phys == 0 || !read_block(phys, blk))
            continue;
        u32 off = 0;
        while (off + 8 <= m_block_size) {
            const u32 e_inode  = rd32(blk, off);
            const u16 rec_len  = rd16(blk, off + 4);
            const u8  name_len = blk[off + 6];
            if (rec_len < 8)
                break;
            const u32 used = e_inode == 0 ? 0u
                                          : static_cast<u32>(8 + ((name_len + 3) & ~3u));
            if (rec_len - used >= needed) {
                u32 slot;
                if (e_inode == 0) {
                    slot = off;                     // reuse a voided entry whole
                } else {
                    wr16(blk, off + 4, static_cast<u16>(used));
                    slot = off + used;
                    wr16(blk, slot + 4, static_cast<u16>(rec_len - used));
                }
                wr32(blk, slot, child);
                blk[slot + 6] = static_cast<u8>(nlen);
                blk[slot + 7] = type;
                memcpy(blk + slot + 8, name, nlen);
                const bool ok = write_block(phys, blk);
                kfree(blk);
                return ok;
            }
            off += rec_len;
        }
    }

    // No slack anywhere: append a fresh directory block.
    const u64 phys = ensure_block(dir, blocks);
    if (phys == 0) {
        kfree(blk);
        return false;
    }
    dir.size += m_block_size;
    memset(blk, 0, m_block_size);
    wr32(blk, 0, child);
    wr16(blk, 4, static_cast<u16>(m_block_size));
    blk[6] = static_cast<u8>(nlen);
    blk[7] = type;
    memcpy(blk + 8, name, nlen);
    const bool ok = write_block(phys, blk) && write_inode(dir);
    kfree(blk);
    return ok;
}

u32 Ext::dir_remove(Inode& dir, const char* name)
{
    const usize nlen = strlen(name);
    u8* blk = static_cast<u8*>(kmalloc(m_block_size));
    if (blk == nullptr)
        return 0;

    u32 removed = 0;
    const u64 blocks = (dir.size + m_block_size - 1) / m_block_size;
    for (u64 fb = 0; fb < blocks && removed == 0; ++fb) {
        const u64 phys = map_block(dir, fb);
        if (phys == 0 || !read_block(phys, blk))
            continue;
        u32 off = 0;
        u32 prev = 0xFFFFFFFF;
        while (off + 8 <= m_block_size) {
            const u32 e_inode  = rd32(blk, off);
            const u16 rec_len  = rd16(blk, off + 4);
            const u8  name_len = blk[off + 6];
            if (rec_len < 8)
                break;
            if (e_inode != 0 && name_len == nlen &&
                memcmp(blk + off + 8, name, nlen) == 0) {
                removed = e_inode;
                if (prev != 0xFFFFFFFF) {
                    wr16(blk, prev + 4,
                         static_cast<u16>(rd16(blk, prev + 4) + rec_len));
                } else {
                    wr32(blk, off, 0);              // first entry: void it
                }
                write_block(phys, blk);
                break;
            }
            prev = off;
            off += rec_len;
        }
    }
    kfree(blk);
    return removed;
}

bool Ext::dir_is_empty(const Inode& dir) const
{
    u8* blk = static_cast<u8*>(kmalloc(m_block_size));
    if (blk == nullptr)
        return false;

    bool empty = true;
    const u64 blocks = (dir.size + m_block_size - 1) / m_block_size;
    for (u64 fb = 0; fb < blocks && empty; ++fb) {
        const u64 phys = map_block(dir, fb);
        if (phys == 0 || !read_block(phys, blk))
            continue;
        u32 off = 0;
        while (off + 8 <= m_block_size) {
            const u32 e_inode  = rd32(blk, off);
            const u16 rec_len  = rd16(blk, off + 4);
            const u8  name_len = blk[off + 6];
            if (rec_len < 8)
                break;
            if (e_inode != 0 && name_len > 0) {
                const bool dot =
                    (name_len == 1 && blk[off + 8] == '.') ||
                    (name_len == 2 && blk[off + 8] == '.' && blk[off + 9] == '.');
                if (!dot) {
                    empty = false;
                    break;
                }
            }
            off += rec_len;
        }
    }
    kfree(blk);
    return empty;
}

bool Ext::resolve_parent(const char* path, Inode& parent, char* name,
                         usize name_cap) const
{
    usize len = strlen(path);
    while (len > 1 && path[len - 1] == '/')
        --len;                                      // ignore trailing slashes

    usize slash = 0;
    bool found = false;
    for (usize i = 0; i < len; ++i) {
        if (path[i] == '/') {
            slash = i;
            found = true;
        }
    }
    if (!found)
        return false;

    const usize nlen = len - (slash + 1);
    if (nlen == 0 || nlen >= name_cap)
        return false;
    memcpy(name, path + slash + 1, nlen);
    name[nlen] = '\0';

    char pbuf[vfs::kMaxPath];
    if (slash == 0) {
        pbuf[0] = '/';
        pbuf[1] = '\0';
    } else {
        if (slash >= sizeof(pbuf))
            return false;
        memcpy(pbuf, path, slash);
        pbuf[slash] = '\0';
    }
    if (!resolve(pbuf, parent))
        return false;
    return (parent.mode & kModeFmt) == kModeDir;
}

bool Ext::create(const char* path, vfs::Type type)
{
    Inode parent;
    char name[vfs::kMaxName];
    if (!resolve_parent(path, parent, name, sizeof(name)))
        return false;
    if (lookup(parent, name) != 0)
        return false;                               // already exists

    const bool is_dir = type == vfs::Type::Directory;
    const u32 number = alloc_inode(is_dir);
    if (number == 0)
        return false;

    Inode inode{};
    inode.number   = number;
    inode.mode     = is_dir ? 0x41EDu : 0x81A4u;    // drwxr-xr-x / -rw-r--r--
    inode.uid      = 0;
    inode.gid      = 0;
    inode.links    = is_dir ? 2 : 1;
    inode.size     = 0;
    inode.i_blocks = 0;
    inode.flags    = kInodeExtentsFlag;
    // Inline, empty, depth-0 extent header with room for four extents.
    u8* eb = reinterpret_cast<u8*>(inode.block);
    wr16(eb, 0, kExtentMagic);
    wr16(eb, 2, 0);
    wr16(eb, 4, 4);
    wr16(eb, 6, 0);

    if (is_dir) {
        const u64 phys = ensure_block(inode, 0);
        if (phys == 0) {
            free_inode(number, true);
            return false;
        }
        inode.size = m_block_size;
        u8* blk = static_cast<u8*>(kmalloc(m_block_size));
        if (blk == nullptr) {
            free_inode_blocks(inode);
            free_inode(number, true);
            return false;
        }
        memset(blk, 0, m_block_size);
        wr32(blk, 0, number);                       // "."
        wr16(blk, 4, 12);
        blk[6] = 1;
        blk[7] = kDirTypeDir;
        blk[8] = '.';
        wr32(blk, 12, parent.number);               // ".."
        wr16(blk, 16, static_cast<u16>(m_block_size - 12));
        blk[18] = 2;
        blk[19] = kDirTypeDir;
        blk[20] = '.';
        blk[21] = '.';
        write_block(phys, blk);
        kfree(blk);
    }

    if (!write_inode(inode)) {
        free_inode_blocks(inode);
        free_inode(number, is_dir);
        return false;
    }

    const u8 dtype = is_dir ? kDirTypeDir : 1;
    if (!dir_insert(parent, name, number, dtype)) {
        free_inode_blocks(inode);
        free_inode(number, is_dir);
        return false;
    }
    if (is_dir) {
        parent.links += 1;                          // the new dir's ".." backref
        write_inode(parent);
    }
    return true;
}

isize Ext::write(const char* path, u64 offset, const void* buffer, usize bytes)
{
    Inode inode;
    if (!resolve(path, inode))
        return -1;
    if ((inode.mode & kModeFmt) != kModeReg)
        return -1;
    if (!(inode.flags & kInodeExtentsFlag))
        return -1;                                  // only extent files writable

    const u8* src = static_cast<const u8*>(buffer);
    u8* blk = static_cast<u8*>(kmalloc(m_block_size));
    if (blk == nullptr)
        return -1;

    usize done = 0;
    while (done < bytes) {
        const u64 pos   = offset + done;
        const u64 fb    = pos / m_block_size;
        const u32 boff  = pos % m_block_size;
        const u32 chunk = min32(m_block_size - boff, static_cast<u32>(bytes - done));

        const u64 phys = ensure_block(inode, fb);
        if (phys == 0) {
            kfree(blk);
            return -1;
        }
        if (chunk != m_block_size) {                // partial: read-modify-write
            if (!read_block(phys, blk)) {
                kfree(blk);
                return -1;
            }
        }
        memcpy(blk + boff, src + done, chunk);
        if (!write_block(phys, blk)) {
            kfree(blk);
            return -1;
        }
        done += chunk;
    }
    kfree(blk);

    if (offset + bytes > inode.size)
        inode.size = offset + bytes;
    if (!write_inode(inode))
        return -1;
    return static_cast<isize>(done);
}

bool Ext::remove(const char* path)
{
    Inode parent;
    char name[vfs::kMaxName];
    if (!resolve_parent(path, parent, name, sizeof(name)))
        return false;
    const u32 number = lookup(parent, name);
    if (number == 0)
        return false;

    Inode inode;
    if (!read_inode(number, inode))
        return false;
    const bool is_dir = (inode.mode & kModeFmt) == kModeDir;
    if (is_dir && !dir_is_empty(inode))
        return false;

    if (dir_remove(parent, name) == 0)
        return false;

    free_inode_blocks(inode);
    free_inode(number, is_dir);                     // frees and zeros the slot

    if (is_dir) {
        parent.links -= 1;
        write_inode(parent);
    }
    return true;
}

bool Ext::rename(const char* old_path, const char* new_path)
{
    Inode old_parent;
    char old_name[vfs::kMaxName];
    if (!resolve_parent(old_path, old_parent, old_name, sizeof(old_name)))
        return false;
    const u32 number = lookup(old_parent, old_name);
    if (number == 0)
        return false;

    Inode inode;
    if (!read_inode(number, inode))
        return false;
    const bool is_dir = (inode.mode & kModeFmt) == kModeDir;

    Inode new_parent;
    char new_name[vfs::kMaxName];
    if (!resolve_parent(new_path, new_parent, new_name, sizeof(new_name)))
        return false;
    if (lookup(new_parent, new_name) != 0)
        return false;                               // target exists: unsupported

    const u8 dtype = is_dir ? kDirTypeDir : 1;
    if (!dir_insert(new_parent, new_name, number, dtype))
        return false;

    // dir_insert may have grown the parent's on-disk inode; re-read the source
    // parent so removing from it works on current state (it may be the same dir).
    Inode source_parent;
    if (!read_inode(old_parent.number, source_parent))
        return false;
    if (dir_remove(source_parent, old_name) == 0)
        return false;

    // Moving a directory to a different parent rewrites its ".." and shifts the
    // subdirectory link count from the old parent to the new one.
    if (is_dir && new_parent.number != old_parent.number) {
        const u64 phys = map_block(inode, 0);
        u8* blk = static_cast<u8*>(kmalloc(m_block_size));
        if (blk != nullptr && phys != 0 && read_block(phys, blk)) {
            const u32 dotdot = rd16(blk, 4);        // just past the "." entry
            wr32(blk, dotdot, new_parent.number);
            write_block(phys, blk);
        }
        kfree(blk);

        Inode np;
        if (read_inode(new_parent.number, np)) {
            np.links += 1;
            write_inode(np);
        }
        Inode op;
        if (read_inode(old_parent.number, op)) {
            op.links -= 1;
            write_inode(op);
        }
    }
    return true;
}

} // namespace fs
