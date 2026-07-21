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
    isize write(const char* path, u64 offset, const void* buffer, usize bytes) override;
    bool create(const char* path, vfs::Type type) override;
    bool remove(const char* path) override;

    u32 cluster_size() const { return m_cluster_bytes; }
    u32 cluster_count() const { return m_cluster_count; }

private:
    // What a directory entry resolves to. The entry's own location is carried
    // along because updating a file's size means writing that entry back, and
    // searching for it a second time would be both slower and racier.
    struct Located {
        bool found;
        bool directory;
        u32  first_cluster;
        u32  size;

        u32  entry_cluster;     // cluster holding the 8.3 entry
        u32  entry_index;       // its index within that cluster
        u32  first_slot_index;  // index of the first LFN entry, for deletion
    };

    Fat32() = default;

    bool mount(block::Device* device);

    u64 cluster_to_lba(u32 cluster) const;
    u32 next_cluster(u32 cluster) const;
    bool read_cluster(u32 cluster, void* buffer) const;
    bool write_cluster(u32 cluster, const void* buffer) const;

    Located find_in_directory(u32 directory_cluster, const char* name) const;
    Located resolve(const char* path) const;

    // --- allocation ---------------------------------------------------------
    bool set_fat_entry(u32 cluster, u32 value);
    u32  allocate_cluster();                    // 0 when the volume is full
    u32  extend_chain(u32 last_cluster);        // append one cluster
    void free_chain(u32 first_cluster);
    bool flush_fsinfo();
    u32  count_free_clusters() const;

    // --- directory editing --------------------------------------------------
    bool update_entry(const Located& located, u32 first_cluster, u32 size);
    bool find_free_slots(u32 directory_cluster, usize needed,
                         u32& out_cluster, u32& out_index);
    bool add_entry(u32 directory_cluster, const char* name,
                   bool directory, u32 first_cluster, u32 size);
    bool split_path(const char* path, char* parent, char* name) const;

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

    // Cached FSInfo hints. These are advisory by spec - a correct driver must
    // never trust them for correctness, only use them to avoid rescanning.
    u32 m_next_free = 2;
    u32 m_free_clusters = 0;
    bool m_fsinfo_dirty = false;

    // Scratch space for one cluster, allocated once at mount. Every read goes
    // through it, so nothing here is reentrant - fine while there is one thread.
    u8* m_scratch = nullptr;
};

} // namespace fs
