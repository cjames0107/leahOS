#pragma once

#include <leah/blockdev.hpp>
#include <leah/vfs.hpp>

namespace fs {

// A driver for the ext2/ext3/ext4 family, which share one on-disk format. It is
// feature-gated rather than three separate drivers: ext3 is ext2 plus a journal
// (which we skip), and ext4 adds extent-mapped files on top. The reader handles
// both extent and classic indirect block maps; the writer targets the tamed
// feature set the root image is built with (see tools/mkext.sh).
//
// Unlike FAT32, ext inodes carry real ownership and permission bits, which is
// why this is the filesystem leahOS mounts as root.

class Ext final : public vfs::FileSystem {
public:
    // Returns nullptr if the device does not hold an ext2/3/4 volume.
    static Ext* probe(block::Device* device);

    ~Ext() override;

    const char* type_name() const override { return "ext4"; }
    const char* volume_label() const override { return m_label; }

    bool stat(const char* path, vfs::Stat& out) override;
    isize read(const char* path, u64 offset, void* buffer, usize bytes) override;
    bool list(const char* path, vfs::Entry* out, usize max, usize& count) override;
    isize write(const char* path, u64 offset, const void* buffer, usize bytes) override;
    bool create(const char* path, vfs::Type type) override;
    bool remove(const char* path) override;
    bool rename(const char* old_path, const char* new_path) override;

    u32 block_size() const { return m_block_size; }

private:
    // A decoded inode plus its number, carried together because writing a file
    // back (size, block map) means writing the inode, and re-resolving it would
    // be slower and racier - the same reasoning as FAT32's Located.
    struct Inode {
        u32 number;
        u16 mode;
        u16 uid;
        u16 gid;
        u16 links;
        u64 size;
        u32 flags;
        u32 i_blocks;       // 512-byte sectors the file occupies (e2fsck checks it)
        u32 block[15];      // raw i_block: direct/indirect pointers or extents
    };

    Ext() = default;

    bool mount(block::Device* device);

    // Block I/O in filesystem-block units (m_block_size), over the device's
    // 512-byte sectors.
    bool read_block(u64 block, void* buffer) const;
    bool write_block(u64 block, const void* buffer) const;

    bool read_inode(u32 number, Inode& out) const;

    // Where inode `number` sits: its containing block and byte offset within it.
    bool inode_location(u32 number, u64& block, u32& offset) const;

    // Copy a block-group descriptor (m_desc_size bytes) into desc.
    bool read_group_desc(u32 group, u8* desc) const;

    // Map a file-relative block index to its physical block (0 = sparse hole),
    // via the extent tree when EXTENTS_FL is set, otherwise the indirect scheme.
    u64 map_block(const Inode& inode, u64 file_block) const;
    u64 map_extent(const Inode& inode, u64 file_block) const;
    u64 map_indirect(const Inode& inode, u64 file_block) const;
    u32 indirect_lookup(u64 block, u64 index) const;

    // Find `name` in the directory `dir`, returning its inode number (0 = none).
    u32 lookup(const Inode& dir, const char* name) const;

    // Resolve an absolute path to its inode. Returns false if any component is
    // missing.
    bool resolve(const char* path, Inode& out) const;

    // --- write path --------------------------------------------------------

    bool write_inode(const Inode& inode) const;    // patch the fields we manage

    // Superblock and group-descriptor free-count bookkeeping. e2fsck's pass 5
    // recomputes these from the bitmaps, so they must stay exact.
    bool write_superblock() const;
    bool write_group_desc(u32 group, const u8* desc) const;

    u64  alloc_block();
    void free_block(u64 block);
    u32  alloc_inode(bool is_dir);                 // returns inode number, 0 on fail
    void free_inode(u32 number, bool was_dir);

    // Append a physical block to an extent-mapped inode's inline extent list.
    bool add_extent(Inode& inode, u32 logical, u64 phys) const;
    // Map `logical`, allocating and mapping a block if it is not present yet.
    u64  ensure_block(Inode& inode, u64 logical);
    void free_inode_blocks(const Inode& inode);    // free every data block

    // Linear directory maintenance.
    bool dir_insert(Inode& dir, const char* name, u32 child, u8 type);
    u32  dir_remove(Inode& dir, const char* name); // returns the removed inode
    bool dir_is_empty(const Inode& dir) const;

    // Split a path into its parent directory (resolved) and final component.
    bool resolve_parent(const char* path, Inode& parent, char* name, usize name_cap) const;

    block::Device* m_device = nullptr;
    u32  m_block_size = 0;
    u32  m_sectors_per_block = 0;
    u32  m_inode_size = 0;
    u32  m_inodes_per_group = 0;
    u32  m_blocks_per_group = 0;
    u32  m_first_data_block = 0;
    u32  m_group_count = 0;
    u32  m_desc_size = 0;           // block-group descriptor size (32 or 64)
    u32  m_first_ino = 0;           // first non-reserved inode
    u32  m_feature_incompat = 0;
    bool m_has_filetype = false;    // dir entries carry a file-type byte
    char m_label[17] = {};

    // The superblock, cached so the free-count fields can be updated and flushed
    // without a read-modify-write of the on-disk copy each time.
    u8   m_sb[1024] = {};
};

} // namespace fs
