#pragma once

#include <leah/blockdev.hpp>
#include <leah/vfs.hpp>

namespace fs {

// Read support for FAT32, including long filenames.
//
// FAT32 first because it is the format every other system can already write:
// an image can be built and inspected with ordinary tools, which makes the
// kernel's reader checkable against something other than itself.

class Fat32 final : public vfs::FileSystem {
public:
    // Returns nullptr if the device does not hold a FAT32 volume.
    static Fat32* probe(block::Device* device);

    ~Fat32() override;

    const char* type_name() const override { return "fat32"; }
    const char* volume_label() const override { return m_label; }

    bool stat(const char* path, vfs::Stat& out) override;
    isize read(const char* path, u64 offset, void* buffer, usize bytes) override;
    bool list(const char* path, vfs::Entry* out, usize max, usize& count) override;

    u32 cluster_size() const { return m_cluster_bytes; }
    u32 cluster_count() const { return m_cluster_count; }

private:
    // What a directory entry resolves to.
    struct Located {
        bool found;
        bool directory;
        u32  first_cluster;
        u32  size;
    };

    Fat32() = default;

    bool mount(block::Device* device);

    u64 cluster_to_lba(u32 cluster) const;
    u32 next_cluster(u32 cluster) const;
    bool read_cluster(u32 cluster, void* buffer) const;

    Located find_in_directory(u32 directory_cluster, const char* name) const;
    Located resolve(const char* path) const;

    block::Device* m_device = nullptr;

    u16 m_bytes_per_sector = 0;
    u8  m_sectors_per_cluster = 0;
    u16 m_reserved_sectors = 0;
    u8  m_fat_count = 0;
    u32 m_sectors_per_fat = 0;
    u32 m_root_cluster = 0;

    u32 m_fat_start = 0;
    u32 m_data_start = 0;
    u32 m_cluster_bytes = 0;
    u32 m_cluster_count = 0;

    char m_label[12]{};

    // Scratch space for one cluster, allocated once at mount. Every read goes
    // through it, so nothing here is reentrant - fine while there is one thread.
    u8* m_scratch = nullptr;
};

} // namespace fs
