#include <leah/fat32.hpp>
#include <leah/heap.hpp>
#include <leah/string.hpp>

namespace fs {
namespace {

constexpr u8 kAttrReadOnly  = 0x01;
constexpr u8 kAttrVolumeId  = 0x08;
constexpr u8 kAttrDirectory = 0x10;
// An LFN entry is marked by every one of the low four attribute bits, a value
// no real file can carry. That is exactly why it was chosen: systems that
// predate long filenames skip these entries instead of showing garbage.
constexpr u8 kAttrLongName  = 0x0F;

constexpr u8 kEntryFree     = 0xE5;
constexpr u8 kEntryEndOfDir = 0x00;

constexpr u32 kEndOfChain   = 0x0FFFFFF8;
constexpr u32 kEntryMask    = 0x0FFFFFFF;

constexpr usize kDirEntrySize = 32;
constexpr usize kLfnCharsPerEntry = 13;

// The sequence field is six bits but the format caps a name at 255 characters,
// which is 20 entries. Bounding by this rather than by the field width stops a
// corrupt sequence number from indexing past the assembly buffer.
constexpr usize kMaxLfnEntries = 20;
constexpr usize kMaxLfnLength  = kMaxLfnEntries * kLfnCharsPerEntry + 1;

struct [[gnu::packed]] DirEntry {
    u8  name[11];
    u8  attributes;
    u8  nt_reserved;
    u8  create_time_tenth;
    u16 create_time;
    u16 create_date;
    u16 access_date;
    u16 cluster_high;
    u16 write_time;
    u16 write_date;
    u16 cluster_low;
    u32 size;
};

static_assert(sizeof(DirEntry) == kDirEntrySize);

// An LFN entry scatters its 13 UTF-16 units across three runs, wrapped around
// fields inherited from the 8.3 layout it has to impersonate. Described as byte
// offsets rather than a packed struct: taking the address of a misaligned
// member is undefined even on x86, where the load itself would have worked.
constexpr usize kLfnRunOffset[3] = { 1, 14, 28 };
constexpr usize kLfnRunCount[3]  = { 5, 6, 2 };

// Expand an 8.3 field into "NAME.EXT". The stored form is space padded and has
// no dot, so it has to be rebuilt rather than copied.
void format_short_name(const u8 raw[11], char* out)
{
    usize length = 0;
    for (usize i = 0; i < 8 && raw[i] != ' '; ++i)
        out[length++] = static_cast<char>(raw[i]);

    if (raw[8] != ' ') {
        out[length++] = '.';
        for (usize i = 8; i < 11 && raw[i] != ' '; ++i)
            out[length++] = static_cast<char>(raw[i]);
    }
    out[length] = '\0';
}

// Copy the 13 UTF-16 units an LFN entry carries into its slot in the name.
// Only the low byte is kept: everything above Latin-1 needs a real UTF-8
// encoder, which is a job for when there is a userland to care.
void copy_lfn_chunk(const u8* raw, char* out)
{
    usize index = 0;
    for (usize run = 0; run < 3; ++run) {
        for (usize i = 0; i < kLfnRunCount[run]; ++i) {
            u16 unit;
            memcpy(&unit, raw + kLfnRunOffset[run] + i * 2, sizeof(unit));

            if (unit == 0x0000 || unit == 0xFFFF)
                out[index] = '\0';
            else
                out[index] = static_cast<char>(unit & 0xFF);
            ++index;
        }
    }
}

bool names_equal_fold(const char* a, const char* b)
{
    // FAT is case-insensitive; comparing case-folded keeps "README.MD" and
    // "readme.md" the same file, which is what every other FAT reader does.
    for (;; ++a, ++b) {
        char x = *a;
        char y = *b;
        if (x >= 'a' && x <= 'z') x = static_cast<char>(x - 32);
        if (y >= 'a' && y <= 'z') y = static_cast<char>(y - 32);
        if (x != y)
            return false;
        if (x == '\0')
            return true;
    }
}

} // namespace

Fat32* Fat32::probe(block::Device* device)
{
    auto* filesystem = new Fat32();
    if (filesystem == nullptr)
        return nullptr;

    if (!filesystem->mount(device)) {
        delete filesystem;
        return nullptr;
    }
    return filesystem;
}

Fat32::~Fat32()
{
    kfree(m_scratch);
}

bool Fat32::mount(block::Device* device)
{
    m_device = device;

    auto* boot = static_cast<u8*>(kmalloc(block::kSectorSize));
    if (boot == nullptr)
        return false;

    if (!device->read(0, 1, boot)) {
        kfree(boot);
        return false;
    }

    if (boot[510] != 0x55 || boot[511] != 0xAA) {
        kfree(boot);
        return false;
    }

    memcpy(&m_bytes_per_sector, boot + 11, 2);
    m_sectors_per_cluster = boot[13];
    memcpy(&m_reserved_sectors, boot + 14, 2);
    m_fat_count = boot[16];
    memcpy(&m_sectors_per_fat, boot + 36, 4);
    memcpy(&m_root_cluster, boot + 44, 4);

    u32 total_sectors = 0;
    memcpy(&total_sectors, boot + 32, 4);

    // FAT12 and FAT16 put a non-zero value in the 16-bit FAT size field and a
    // non-zero root entry count; FAT32 leaves both at zero. That, rather than
    // the "FAT32" string further down, is the documented way to tell them
    // apart - the string is advisory and some formatters get it wrong.
    u16 root_entries = 0;
    u16 fat_size_16 = 0;
    memcpy(&root_entries, boot + 17, 2);
    memcpy(&fat_size_16, boot + 22, 2);

    const bool plausible =
        m_bytes_per_sector == block::kSectorSize &&
        m_sectors_per_cluster != 0 &&
        (m_sectors_per_cluster & (m_sectors_per_cluster - 1)) == 0 &&
        m_reserved_sectors != 0 &&
        m_fat_count != 0 &&
        root_entries == 0 &&
        fat_size_16 == 0 &&
        m_sectors_per_fat != 0 &&
        m_root_cluster >= 2;

    if (!plausible) {
        kfree(boot);
        return false;
    }

    memcpy(m_label, boot + 71, 11);
    m_label[11] = '\0';
    for (isize i = 10; i >= 0 && m_label[i] == ' '; --i)
        m_label[i] = '\0';

    m_fat_start  = m_reserved_sectors;
    m_data_start = m_reserved_sectors + m_fat_count * m_sectors_per_fat;
    m_cluster_bytes = m_sectors_per_cluster * m_bytes_per_sector;
    m_cluster_count = (total_sectors - m_data_start) / m_sectors_per_cluster;

    kfree(boot);

    m_scratch = static_cast<u8*>(kmalloc(m_cluster_bytes));
    return m_scratch != nullptr;
}

u64 Fat32::cluster_to_lba(u32 cluster) const
{
    // Cluster numbering starts at 2: entries 0 and 1 of the FAT hold the media
    // descriptor and end-of-chain marker rather than describing any storage.
    return m_data_start + static_cast<u64>(cluster - 2) * m_sectors_per_cluster;
}

u32 Fat32::next_cluster(u32 cluster) const
{
    const u32 offset = cluster * 4;
    const u32 sector = m_fat_start + offset / m_bytes_per_sector;
    const u32 index  = offset % m_bytes_per_sector;

    u8 buffer[block::kSectorSize];
    if (!m_device->read(sector, 1, buffer))
        return kEndOfChain;

    u32 entry = 0;
    memcpy(&entry, buffer + index, 4);

    // The top four bits are reserved and must be ignored, not compared.
    return entry & kEntryMask;
}

bool Fat32::read_cluster(u32 cluster, void* buffer) const
{
    return m_device->read(cluster_to_lba(cluster), m_sectors_per_cluster, buffer);
}

Fat32::Located Fat32::find_in_directory(u32 directory_cluster, const char* name) const
{
    Located result{};

    // Long filename pieces arrive before the 8.3 entry they belong to, in
    // reverse order, so they are accumulated here until that entry shows up.
    char long_name[kMaxLfnLength]{};
    bool have_long_name = false;

    u32 cluster = directory_cluster;
    while (cluster >= 2 && cluster < kEndOfChain) {
        if (!read_cluster(cluster, m_scratch))
            return result;

        const usize entries = m_cluster_bytes / kDirEntrySize;
        for (usize i = 0; i < entries; ++i) {
            const u8* raw = m_scratch + i * kDirEntrySize;

            if (raw[0] == kEntryEndOfDir)
                return result;
            if (raw[0] == kEntryFree) {
                have_long_name = false;
                continue;
            }

            const u8 attributes = raw[11];

            if ((attributes & kAttrLongName) == kAttrLongName) {
                const u8 sequence = raw[0] & 0x3F;
                if (sequence >= 1 && sequence <= kMaxLfnEntries) {
                    copy_lfn_chunk(raw, long_name + (sequence - 1) * kLfnCharsPerEntry);
                    have_long_name = true;
                }
                continue;
            }

            if ((attributes & kAttrVolumeId) != 0) {
                have_long_name = false;
                continue;
            }

            DirEntry entry;
            memcpy(&entry, raw, sizeof(entry));

            char short_name[13];
            format_short_name(entry.name, short_name);

            const bool matched =
                (have_long_name && names_equal_fold(long_name, name)) ||
                names_equal_fold(short_name, name);

            if (matched) {
                result.found = true;
                result.directory = (entry.attributes & kAttrDirectory) != 0;
                result.first_cluster =
                    static_cast<u32>(entry.cluster_high) << 16 | entry.cluster_low;
                result.size = entry.size;
                return result;
            }

            have_long_name = false;
        }

        cluster = next_cluster(cluster);
    }
    return result;
}

Fat32::Located Fat32::resolve(const char* path) const
{
    Located current{};
    current.found = true;
    current.directory = true;
    current.first_cluster = m_root_cluster;
    current.size = 0;

    if (path == nullptr)
        return current;

    usize offset = 0;
    while (path[offset] == '/')
        ++offset;

    while (path[offset] != '\0') {
        char component[vfs::kMaxName];
        usize length = 0;
        while (path[offset] != '\0' && path[offset] != '/' &&
               length + 1 < sizeof(component)) {
            component[length++] = path[offset++];
        }
        component[length] = '\0';

        while (path[offset] == '/')
            ++offset;

        if (length == 0)
            continue;

        if (!current.directory)
            return Located{};                   // a path component under a file

        current = find_in_directory(current.first_cluster, component);
        if (!current.found)
            return Located{};
    }
    return current;
}

bool Fat32::stat(const char* path, vfs::Stat& out)
{
    const Located located = resolve(path);
    if (!located.found)
        return false;

    out.type = located.directory ? vfs::Type::Directory : vfs::Type::File;
    out.size = located.directory ? 0 : located.size;
    return true;
}

isize Fat32::read(const char* path, u64 offset, void* buffer, usize bytes)
{
    const Located located = resolve(path);
    if (!located.found || located.directory)
        return -1;

    if (offset >= located.size)
        return 0;
    if (offset + bytes > located.size)
        bytes = located.size - offset;

    // Walk the chain to the cluster the read starts in. Without a per-file
    // cursor this is O(offset) on every call, which is why sequential reads
    // should pull large blocks rather than a byte at a time.
    u32 cluster = located.first_cluster;
    u64 skip = offset / m_cluster_bytes;
    while (skip-- > 0) {
        cluster = next_cluster(cluster);
        if (cluster < 2 || cluster >= kEndOfChain)
            return -1;
    }

    auto* out = static_cast<u8*>(buffer);
    usize done = 0;
    u32 within = offset % m_cluster_bytes;

    while (done < bytes) {
        if (cluster < 2 || cluster >= kEndOfChain)
            break;
        if (!read_cluster(cluster, m_scratch))
            return -1;

        usize available = m_cluster_bytes - within;
        if (available > bytes - done)
            available = bytes - done;

        memcpy(out + done, m_scratch + within, available);
        done += available;
        within = 0;

        if (done < bytes)
            cluster = next_cluster(cluster);
    }

    return static_cast<isize>(done);
}

bool Fat32::list(const char* path, vfs::Entry* out, usize max, usize& count)
{
    count = 0;

    const Located located = resolve(path);
    if (!located.found || !located.directory)
        return false;

    char long_name[kMaxLfnLength]{};
    bool have_long_name = false;

    u32 cluster = located.first_cluster;
    while (cluster >= 2 && cluster < kEndOfChain && count < max) {
        if (!read_cluster(cluster, m_scratch))
            return false;

        const usize entries = m_cluster_bytes / kDirEntrySize;
        for (usize i = 0; i < entries && count < max; ++i) {
            const u8* raw = m_scratch + i * kDirEntrySize;

            if (raw[0] == kEntryEndOfDir)
                return true;
            if (raw[0] == kEntryFree) {
                have_long_name = false;
                continue;
            }

            const u8 attributes = raw[11];

            if ((attributes & kAttrLongName) == kAttrLongName) {
                const u8 sequence = raw[0] & 0x3F;
                if (sequence >= 1 && sequence <= kMaxLfnEntries) {
                    copy_lfn_chunk(raw, long_name + (sequence - 1) * kLfnCharsPerEntry);
                    have_long_name = true;
                }
                continue;
            }

            if ((attributes & kAttrVolumeId) != 0) {
                have_long_name = false;
                continue;
            }

            DirEntry entry;
            memcpy(&entry, raw, sizeof(entry));

            // "." and ".." are real entries on disk; hide them the way every
            // other listing does.
            if (entry.name[0] == '.') {
                have_long_name = false;
                continue;
            }

            vfs::Entry& slot = out[count];
            if (have_long_name) {
                usize n = 0;
                while (n + 1 < vfs::kMaxName && long_name[n] != '\0') {
                    slot.name[n] = long_name[n];
                    ++n;
                }
                slot.name[n] = '\0';
            } else {
                format_short_name(entry.name, slot.name);
            }

            slot.type = (entry.attributes & kAttrDirectory) != 0
                      ? vfs::Type::Directory : vfs::Type::File;
            slot.size = entry.size;
            ++count;

            have_long_name = false;
        }

        cluster = next_cluster(cluster);
    }
    return true;
}

} // namespace fs
