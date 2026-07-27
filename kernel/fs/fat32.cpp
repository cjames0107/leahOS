#include <leah/fat32.hpp>
#include <leah/heap.hpp>
#include <leah/string.hpp>

namespace fs {
namespace {

constexpr u8 kAttrReadOnly  = 0x01;
constexpr u8 kAttrVolumeId  = 0x08;
constexpr u8 kAttrArchive   = 0x20;
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
// nt_flags carries the VFAT lowercase hints: bit 3 means display the stem
// lowered, bit 4 the extension. They are how "wrote.txt" survives without long
// filename entries, so ignoring them here would show every such file shouting.
void format_short_name(const u8 raw[11], char* out, u8 nt_flags = 0)
{
    const bool lower_stem = (nt_flags & 0x08) != 0;
    const bool lower_ext  = (nt_flags & 0x10) != 0;

    auto emit = [](char c, bool lower) {
        if (lower && c >= 'A' && c <= 'Z')
            return static_cast<char>(c + 32);
        return c;
    };

    usize length = 0;
    for (usize i = 0; i < 8 && raw[i] != ' '; ++i)
        out[length++] = emit(static_cast<char>(raw[i]), lower_stem);

    if (raw[8] != ' ') {
        out[length++] = '.';
        for (usize i = 8; i < 11 && raw[i] != ' '; ++i)
            out[length++] = emit(static_cast<char>(raw[i]), lower_ext);
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

u8 short_name_checksum(const u8 name[11])
{
    u8 sum = 0;
    for (usize i = 0; i < 11; ++i)
        sum = static_cast<u8>(((sum & 1) << 7) + (sum >> 1) + name[i]);
    return sum;
}

bool is_short_name_char(char c)
{
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= '0' && c <= '9') return true;
    for (const char* p = "$%'-_@~`!(){}^#&"; *p != '\0'; ++p) {
        if (*p == c)
            return true;
    }
    return false;
}

// Try to express a name as pure 8.3.
//
// The lowercase flags in the NT-reserved byte are what let "notes.txt" round
// trip without long filename entries: the name is stored uppercase and the
// flags say to display it lowered. It only works when the stem and extension
// are each entirely one case, which is why mixed case still needs LFN.
bool try_short_name(const char* name, u8 out[11], u8& nt_flags)
{
    nt_flags = 0;
    for (usize i = 0; i < 11; ++i)
        out[i] = ' ';

    usize dot = 0;
    usize length = 0;
    bool seen_dot = false;
    for (const char* p = name; *p != '\0'; ++p, ++length) {
        if (*p == '.') {
            if (seen_dot)
                return false;           // more than one dot needs LFN
            seen_dot = true;
            dot = length;
        }
    }

    const usize stem_length = seen_dot ? dot : length;
    const usize ext_length  = seen_dot ? length - dot - 1 : 0;
    if (stem_length == 0 || stem_length > 8 || ext_length > 3)
        return false;

    bool stem_lower = false, stem_upper = false;
    for (usize i = 0; i < stem_length; ++i) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') { stem_lower = true; c = static_cast<char>(c - 32); }
        else if (c >= 'A' && c <= 'Z') stem_upper = true;
        if (!is_short_name_char(c))
            return false;
        out[i] = static_cast<u8>(c);
    }

    bool ext_lower = false, ext_upper = false;
    for (usize i = 0; i < ext_length; ++i) {
        char c = name[dot + 1 + i];
        if (c >= 'a' && c <= 'z') { ext_lower = true; c = static_cast<char>(c - 32); }
        else if (c >= 'A' && c <= 'Z') ext_upper = true;
        if (!is_short_name_char(c))
            return false;
        out[8 + i] = static_cast<u8>(c);
    }

    if (stem_lower && stem_upper) return false;
    if (ext_lower && ext_upper) return false;

    if (stem_lower) nt_flags |= 0x08;
    if (ext_lower)  nt_flags |= 0x10;
    return true;
}

// Fallback for names 8.3 cannot hold: NAME~1.EXT alongside LFN entries.
void mangle_short_name(const char* name, u8 out[11], u32 sequence)
{
    for (usize i = 0; i < 11; ++i)
        out[i] = ' ';

    // The extension is whatever follows the final dot.
    const char* ext = nullptr;
    for (const char* p = name; *p != '\0'; ++p) {
        if (*p == '.')
            ext = p + 1;
    }

    usize written = 0;
    for (const char* p = name; *p != '\0' && written < 6; ++p) {
        if (ext != nullptr && p == ext - 1)
            break;
        char c = *p;
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
        if (!is_short_name_char(c))
            continue;
        out[written++] = static_cast<u8>(c);
    }
    if (written == 0)
        out[written++] = 'F';

    out[written++] = '~';
    out[written] = static_cast<u8>('0' + (sequence % 10));

    if (ext != nullptr) {
        usize e = 0;
        for (const char* p = ext; *p != '\0' && e < 3; ++p) {
            char c = *p;
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
            if (!is_short_name_char(c))
                continue;
            out[8 + e++] = static_cast<u8>(c);
        }
    }
}

void write_lfn_entry(u8* raw, const char* name, usize name_length,
                     u8 sequence, bool last, u8 checksum)
{
    memset(raw, 0, kDirEntrySize);
    raw[0] = static_cast<u8>(sequence | (last ? 0x40 : 0x00));
    raw[11] = kAttrLongName;
    raw[12] = 0;
    raw[13] = checksum;

    usize index = (sequence - 1) * kLfnCharsPerEntry;
    for (usize run = 0; run < 3; ++run) {
        for (usize i = 0; i < kLfnRunCount[run]; ++i, ++index) {
            u16 unit;
            if (index < name_length)
                unit = static_cast<u16>(static_cast<u8>(name[index]));
            else if (index == name_length)
                unit = 0x0000;                  // the terminator
            else
                unit = 0xFFFF;                  // padding
            memcpy(raw + kLfnRunOffset[run] + i * 2, &unit, sizeof(unit));
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
    if (m_scratch == nullptr)
        return false;

    // FSInfo carries the free-cluster count. It is only a hint, but writing a
    // wrong one back is real corruption as far as a repair tool is concerned,
    // so it has to be seeded from disk rather than counted up from zero.
    m_free_clusters = 0xFFFFFFFF;
    m_next_free = 2;

    u8 info[block::kSectorSize];
    if (device->read(1, 1, info)) {
        u32 lead = 0, structure = 0;
        memcpy(&lead, info, 4);
        memcpy(&structure, info + 484, 4);
        if (lead == 0x41615252 && structure == 0x61417272) {
            memcpy(&m_free_clusters, info + 488, 4);
            memcpy(&m_next_free, info + 492, 4);
        }
    }

    // 0xFFFFFFFF is the documented "unknown" value, and anything larger than
    // the volume is simply wrong. Either way, count for real.
    if (m_free_clusters == 0xFFFFFFFF || m_free_clusters > m_cluster_count)
        m_free_clusters = count_free_clusters();
    if (m_next_free < 2 || m_next_free >= m_cluster_count + 2)
        m_next_free = 2;

    return true;
}

u32 Fat32::count_free_clusters() const
{
    // Reads the FAT a sector at a time rather than calling next_cluster() per
    // entry, which would be one device read per cluster.
    u32 free = 0;
    u32 cluster = 0;
    const u32 total = m_cluster_count + 2;
    const u32 per_sector = m_bytes_per_sector / 4;

    u8 buffer[block::kSectorSize];
    for (u32 sector = 0; sector < m_sectors_per_fat && cluster < total; ++sector) {
        if (!m_device->read(m_fat_start + sector, 1, buffer))
            break;
        for (u32 i = 0; i < per_sector && cluster < total; ++i, ++cluster) {
            if (cluster < 2)
                continue;
            u32 entry = 0;
            memcpy(&entry, buffer + i * 4, 4);
            if ((entry & kEntryMask) == 0)
                ++free;
        }
    }
    return free;
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
    usize first_slot = 0;

    u32 cluster = directory_cluster;
    while (cluster >= 2 && cluster < kEndOfChain) {
        if (!read_cluster(cluster, m_scratch))
            return result;

        const usize entries = m_cluster_bytes / kDirEntrySize;
        first_slot = 0;
        for (usize i = 0; i < entries; ++i) {
            const u8* raw = m_scratch + i * kDirEntrySize;

            if (raw[0] == kEntryEndOfDir)
                return result;
            if (raw[0] == kEntryFree) {
                have_long_name = false;
                first_slot = i + 1;
                continue;
            }

            const u8 attributes = raw[11];

            if ((attributes & kAttrLongName) == kAttrLongName) {
                const u8 sequence = raw[0] & 0x3F;
                if (!have_long_name)
                    first_slot = i;
                if (sequence >= 1 && sequence <= kMaxLfnEntries) {
                    copy_lfn_chunk(raw, long_name + (sequence - 1) * kLfnCharsPerEntry);
                    have_long_name = true;
                }
                continue;
            }

            if ((attributes & kAttrVolumeId) != 0) {
                have_long_name = false;
                first_slot = i + 1;
                continue;
            }

            if (!have_long_name)
                first_slot = i;

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
                result.entry_cluster = cluster;
                result.entry_index = static_cast<u32>(i);
                result.first_slot_index = static_cast<u32>(first_slot);
                return result;
            }

            have_long_name = false;
            first_slot = i + 1;
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
    // FAT32 stores no ownership or permission bits, so report a fixed,
    // permissive default rather than inventing something that looks enforced.
    out.mode = located.directory ? 0755 : 0644;
    out.uid  = 0;
    out.gid  = 0;
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
                format_short_name(entry.name, slot.name, entry.nt_reserved);
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

// ============================================================================
// Writing
// ============================================================================

bool Fat32::write_cluster(u32 cluster, const void* buffer) const
{
    return m_device->write(cluster_to_lba(cluster), m_sectors_per_cluster, buffer);
}

bool Fat32::set_fat_entry(u32 cluster, u32 value)
{
    if (cluster < 2 || cluster >= m_cluster_count + 2)
        return false;

    const u32 offset = cluster * 4;
    const u32 sector = offset / m_bytes_per_sector;
    const u32 index  = offset % m_bytes_per_sector;

    u8 buffer[block::kSectorSize];

    // Every copy of the FAT has to be updated. A repair tool comparing them is
    // entitled to treat a mismatch as corruption, and some systems read from
    // the second copy.
    for (u32 copy = 0; copy < m_fat_count; ++copy) {
        const u32 lba = m_fat_start + copy * m_sectors_per_fat + sector;
        if (!m_device->read(lba, 1, buffer))
            return false;

        u32 existing = 0;
        memcpy(&existing, buffer + index, 4);

        // The top four bits are reserved and must be preserved, not overwritten.
        const u32 updated = (existing & 0xF0000000) | (value & kEntryMask);
        memcpy(buffer + index, &updated, 4);

        if (!m_device->write(lba, 1, buffer))
            return false;
    }
    return true;
}

u32 Fat32::allocate_cluster()
{
    const u32 total = m_cluster_count + 2;

    // Start from the FSInfo hint, then wrap. The hint is advisory, so a wrong
    // value costs a slower scan rather than a corrupt filesystem.
    for (u32 pass = 0; pass < 2; ++pass) {
        const u32 begin = pass == 0 ? m_next_free : 2;
        const u32 end   = pass == 0 ? total : m_next_free;

        for (u32 cluster = begin; cluster < end; ++cluster) {
            if (next_cluster(cluster) != 0)
                continue;
            if (!set_fat_entry(cluster, kEndOfChain))
                return 0;

            // Hand back a zeroed cluster. Otherwise a partial write would let
            // whatever the last owner stored leak into the new file.
            memset(m_scratch, 0, m_cluster_bytes);
            if (!write_cluster(cluster, m_scratch))
                return 0;

            m_next_free = cluster + 1;
            if (m_free_clusters > 0)
                --m_free_clusters;
            m_fsinfo_dirty = true;
            return cluster;
        }
    }
    return 0;
}

u32 Fat32::extend_chain(u32 last_cluster)
{
    const u32 fresh = allocate_cluster();
    if (fresh == 0)
        return 0;
    if (!set_fat_entry(last_cluster, fresh)) {
        set_fat_entry(fresh, 0);
        return 0;
    }
    return fresh;
}

void Fat32::free_chain(u32 first_cluster)
{
    u32 cluster = first_cluster;
    while (cluster >= 2 && cluster < kEndOfChain) {
        const u32 next = next_cluster(cluster);
        set_fat_entry(cluster, 0);
        ++m_free_clusters;
        if (cluster < m_next_free)
            m_next_free = cluster;
        cluster = next;
    }
    m_fsinfo_dirty = true;
}

bool Fat32::flush_fsinfo()
{
    if (!m_fsinfo_dirty)
        return true;

    u8 sector[block::kSectorSize];
    if (!m_device->read(1, 1, sector))
        return false;

    memcpy(sector + 488, &m_free_clusters, 4);
    memcpy(sector + 492, &m_next_free, 4);

    if (!m_device->write(1, 1, sector))
        return false;

    m_fsinfo_dirty = false;
    return true;
}

bool Fat32::update_entry(const Located& located, u32 first_cluster, u32 size)
{
    if (!read_cluster(located.entry_cluster, m_scratch))
        return false;

    u8* raw = m_scratch + located.entry_index * kDirEntrySize;
    const u16 high = static_cast<u16>(first_cluster >> 16);
    const u16 low  = static_cast<u16>(first_cluster & 0xFFFF);

    memcpy(raw + 20, &high, 2);
    memcpy(raw + 26, &low, 2);
    memcpy(raw + 28, &size, 4);

    return write_cluster(located.entry_cluster, m_scratch);
}

bool Fat32::find_free_slots(u32 directory_cluster, usize needed,
                            u32& out_cluster, u32& out_index)
{
    const usize per_cluster = m_cluster_bytes / kDirEntrySize;
    if (needed > per_cluster)
        return false;       // a run spanning clusters is not handled

    u32 cluster = directory_cluster;
    u32 last = cluster;

    while (cluster >= 2 && cluster < kEndOfChain) {
        if (!read_cluster(cluster, m_scratch))
            return false;

        usize run = 0;
        for (usize i = 0; i < per_cluster; ++i) {
            const u8 first_byte = m_scratch[i * kDirEntrySize];
            if (first_byte == kEntryEndOfDir || first_byte == kEntryFree) {
                if (run == 0)
                    out_index = static_cast<u32>(i);
                if (++run == needed) {
                    out_cluster = cluster;
                    return true;
                }
            } else {
                run = 0;
            }
        }

        last = cluster;
        cluster = next_cluster(cluster);
    }

    // Directory is full: give it another cluster. allocate_cluster() zeroes it,
    // which is exactly the end-of-directory marker we need.
    const u32 fresh = extend_chain(last);
    if (fresh == 0)
        return false;

    out_cluster = fresh;
    out_index = 0;
    return true;
}

bool Fat32::add_entry(u32 directory_cluster, const char* name,
                      bool directory, u32 first_cluster, u32 size)
{
    u8 short_name[11];
    u8 nt_flags = 0;
    usize lfn_count = 0;

    usize name_length = 0;
    while (name[name_length] != '\0')
        ++name_length;

    if (!try_short_name(name, short_name, nt_flags)) {
        // Pick a ~n suffix that is not already taken in this directory.
        for (u32 sequence = 1; sequence <= 9; ++sequence) {
            mangle_short_name(name, short_name, sequence);
            char probe[13];
            format_short_name(short_name, probe);
            if (!find_in_directory(directory_cluster, probe).found)
                break;
        }
        nt_flags = 0;
        lfn_count = (name_length + kLfnCharsPerEntry - 1) / kLfnCharsPerEntry;
        if (lfn_count > kMaxLfnEntries)
            return false;
    }

    u32 cluster = 0;
    u32 index = 0;
    if (!find_free_slots(directory_cluster, lfn_count + 1, cluster, index))
        return false;

    if (!read_cluster(cluster, m_scratch))
        return false;

    const u8 checksum = short_name_checksum(short_name);

    // LFN entries are stored in descending sequence order ahead of the 8.3
    // entry, so the highest sequence - the one flagged last - comes first.
    for (usize i = 0; i < lfn_count; ++i) {
        const u8 sequence = static_cast<u8>(lfn_count - i);
        write_lfn_entry(m_scratch + (index + i) * kDirEntrySize,
                        name, name_length, sequence, i == 0, checksum);
    }

    u8* raw = m_scratch + (index + lfn_count) * kDirEntrySize;
    memset(raw, 0, kDirEntrySize);
    memcpy(raw, short_name, 11);
    raw[11] = directory ? kAttrDirectory : kAttrArchive;
    raw[12] = nt_flags;

    const u16 high = static_cast<u16>(first_cluster >> 16);
    const u16 low  = static_cast<u16>(first_cluster & 0xFFFF);
    memcpy(raw + 20, &high, 2);
    memcpy(raw + 26, &low, 2);
    memcpy(raw + 28, &size, 4);

    return write_cluster(cluster, m_scratch);
}

bool Fat32::split_path(const char* path, char* parent, char* name) const
{
    usize length = 0;
    while (path[length] != '\0')
        ++length;

    while (length > 0 && path[length - 1] == '/')
        --length;                                   // ignore a trailing slash
    if (length == 0)
        return false;                               // cannot split the root

    usize slash = length;
    while (slash > 0 && path[slash - 1] != '/')
        --slash;

    const usize name_length = length - slash;
    if (name_length == 0 || name_length >= vfs::kMaxName)
        return false;

    for (usize i = 0; i < name_length; ++i)
        name[i] = path[slash + i];
    name[name_length] = '\0';

    usize parent_length = slash > 0 ? slash - 1 : 0;
    if (parent_length >= vfs::kMaxPath)
        return false;
    for (usize i = 0; i < parent_length; ++i)
        parent[i] = path[i];
    parent[parent_length] = '\0';

    if (parent_length == 0) {
        parent[0] = '/';
        parent[1] = '\0';
    }
    return true;
}

isize Fat32::write(const char* path, u64 offset, const void* buffer, usize bytes)
{
    Located located = resolve(path);
    if (!located.found || located.directory)
        return -1;
    if (bytes == 0)
        return 0;

    u32 first = located.first_cluster;
    if (first == 0) {
        // An empty file owns no cluster at all, so writing to one starts here.
        first = allocate_cluster();
        if (first == 0)
            return -1;
    }

    u32 cluster = first;
    for (u64 skip = offset / m_cluster_bytes; skip > 0; --skip) {
        u32 next = next_cluster(cluster);
        if (next < 2 || next >= kEndOfChain) {
            next = extend_chain(cluster);
            if (next == 0)
                return -1;
        }
        cluster = next;
    }

    const auto* in = static_cast<const u8*>(buffer);
    usize done = 0;
    u32 within = offset % m_cluster_bytes;

    while (done < bytes) {
        usize chunk = m_cluster_bytes - within;
        if (chunk > bytes - done)
            chunk = bytes - done;

        // A partial cluster has to be read first so the bytes we are not
        // touching survive.
        if (chunk != m_cluster_bytes && !read_cluster(cluster, m_scratch))
            return -1;

        memcpy(m_scratch + within, in + done, chunk);
        if (!write_cluster(cluster, m_scratch))
            return -1;

        done += chunk;
        within = 0;

        if (done < bytes) {
            u32 next = next_cluster(cluster);
            if (next < 2 || next >= kEndOfChain) {
                next = extend_chain(cluster);
                if (next == 0)
                    break;
            }
            cluster = next;
        }
    }

    const u64 end = offset + done;
    const u32 size = end > located.size ? static_cast<u32>(end) : located.size;
    if (!update_entry(located, first, size))
        return -1;

    flush_fsinfo();
    return static_cast<isize>(done);
}

bool Fat32::create(const char* path, vfs::Type type)
{
    char parent[vfs::kMaxPath];
    char name[vfs::kMaxName];
    if (!split_path(path, parent, name))
        return false;

    const Located directory = resolve(parent);
    if (!directory.found || !directory.directory)
        return false;
    if (find_in_directory(directory.first_cluster, name).found)
        return false;

    u32 first = 0;
    if (type == vfs::Type::Directory) {
        first = allocate_cluster();
        if (first == 0)
            return false;

        // "." and ".." are real entries that have to be written by hand.
        memset(m_scratch, 0, m_cluster_bytes);

        u8* dot = m_scratch;
        memset(dot, ' ', 11);
        dot[0] = '.';
        dot[11] = kAttrDirectory;
        const u16 self_high = static_cast<u16>(first >> 16);
        const u16 self_low = static_cast<u16>(first & 0xFFFF);
        memcpy(dot + 20, &self_high, 2);
        memcpy(dot + 26, &self_low, 2);

        u8* dotdot = m_scratch + kDirEntrySize;
        memset(dotdot, ' ', 11);
        dotdot[0] = '.';
        dotdot[1] = '.';
        dotdot[11] = kAttrDirectory;
        // A parent that is the root is recorded as cluster 0, not as 2.
        const u32 parent_cluster =
            directory.first_cluster == m_root_cluster ? 0 : directory.first_cluster;
        const u16 up_high = static_cast<u16>(parent_cluster >> 16);
        const u16 up_low = static_cast<u16>(parent_cluster & 0xFFFF);
        memcpy(dotdot + 20, &up_high, 2);
        memcpy(dotdot + 26, &up_low, 2);

        if (!write_cluster(first, m_scratch)) {
            free_chain(first);
            return false;
        }
    }

    if (!add_entry(directory.first_cluster, name,
                   type == vfs::Type::Directory, first, 0)) {
        if (first != 0)
            free_chain(first);
        return false;
    }

    flush_fsinfo();
    return true;
}

bool Fat32::remove(const char* path)
{
    const Located located = resolve(path);
    if (!located.found)
        return false;

    if (located.directory) {
        // Refuse to orphan the contents of a non-empty directory.
        vfs::Entry probe[1];
        usize count = 0;
        if (list(path, probe, 1, count) && count > 0)
            return false;
    }

    if (located.first_cluster >= 2)
        free_chain(located.first_cluster);

    if (!free_entry(located))
        return false;

    flush_fsinfo();
    return true;
}

// Mark a directory entry and its long-filename entries free, without touching
// the cluster chain they point at - so it can be used both to delete a file
// (after freeing the chain) and to move one (leaving the chain in place).
bool Fat32::free_entry(const Located& located)
{
    if (!read_cluster(located.entry_cluster, m_scratch))
        return false;

    // A run that straddles a cluster boundary is not handled; the leftovers
    // would be orphaned LFN entries, which readers ignore.
    for (u32 i = located.first_slot_index; i <= located.entry_index; ++i)
        m_scratch[i * kDirEntrySize] = kEntryFree;

    return write_cluster(located.entry_cluster, m_scratch);
}

bool Fat32::rename(const char* old_path, const char* new_path)
{
    const Located src = resolve(old_path);
    if (!src.found)
        return false;

    char parent[vfs::kMaxPath];
    char name[vfs::kMaxName];
    if (!split_path(new_path, parent, name))
        return false;

    const Located dir = resolve(parent);
    if (!dir.found || !dir.directory)
        return false;
    if (find_in_directory(dir.first_cluster, name).found)
        return false;                       // refuse to clobber an existing name

    // Point a fresh entry in the destination directory at the same cluster
    // chain - no data is copied - then free the old entry.
    if (!add_entry(dir.first_cluster, name, src.directory, src.first_cluster, src.size))
        return false;
    if (!free_entry(src))
        return false;

    // A moved directory's ".." must follow it to its new parent.
    if (src.directory && src.first_cluster >= 2 && read_cluster(src.first_cluster, m_scratch)) {
        u8* dotdot = m_scratch + kDirEntrySize;         // second entry is ".."
        const u32 new_parent = dir.first_cluster == m_root_cluster ? 0 : dir.first_cluster;
        const u16 high = static_cast<u16>(new_parent >> 16);
        const u16 low  = static_cast<u16>(new_parent & 0xFFFF);
        memcpy(dotdot + 20, &high, 2);
        memcpy(dotdot + 26, &low, 2);
        write_cluster(src.first_cluster, m_scratch);
    }

    flush_fsinfo();
    return true;
}

} // namespace fs
