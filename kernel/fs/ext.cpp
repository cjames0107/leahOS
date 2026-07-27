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
    // so read it as two 512-byte sectors regardless.
    u8 sb[1024];
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
    out.links  = rd16(i, 26);
    out.flags  = rd32(i, 32);
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

// --- write path: implemented in the next checkpoint ------------------------

isize Ext::write(const char*, u64, const void*, usize) { return -1; }
bool Ext::create(const char*, vfs::Type) { return false; }
bool Ext::remove(const char*) { return false; }
bool Ext::rename(const char*, const char*) { return false; }

} // namespace fs
