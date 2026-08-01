#include <leah/elf.hpp>
#include <leah/heap.hpp>
#include <leah/pmm.hpp>
#include <leah/string.hpp>
#include <leah/vfs.hpp>
#include <leah/vmm.hpp>

namespace elf {
namespace {

struct [[gnu::packed]] Header {
    u8  ident[16];
    u16 type;
    u16 machine;
    u32 version;
    u64 entry;
    u64 program_header_offset;
    u64 section_header_offset;
    u32 flags;
    u16 header_size;
    u16 program_header_size;
    u16 program_header_count;
    u16 section_header_size;
    u16 section_header_count;
    u16 section_name_index;
};

struct [[gnu::packed]] ProgramHeader {
    u32 type;
    u32 flags;
    u64 offset;
    u64 virtual_address;
    u64 physical_address;
    u64 file_size;
    u64 memory_size;
    u64 alignment;
};

static_assert(sizeof(Header) == 64);
static_assert(sizeof(ProgramHeader) == 56);

constexpr u8 kMagic[4] = { 0x7F, 'E', 'L', 'F' };

constexpr u8 kClass64      = 2;
constexpr u8 kLittleEndian = 1;

constexpr u16 kTypeExecutable = 2;
constexpr u16 kMachineX86_64  = 0x3E;

constexpr u32 kSegmentLoad = 1;

constexpr u32 kFlagExecute = 1;
constexpr u32 kFlagWrite   = 2;

} // namespace

const char* error_name(Error error)
{
    switch (error) {
    case Error::None:             return "ok";
    case Error::NotFound:         return "file not found";
    case Error::TooSmall:         return "file is smaller than an ELF header";
    case Error::NotElf:           return "missing ELF magic";
    case Error::WrongClass:       return "not a little-endian 64-bit object";
    case Error::WrongMachine:     return "not an x86-64 object";
    case Error::WrongType:        return "not an executable";
    case Error::BadProgramHeader: return "malformed program header";
    case Error::OutOfMemory:      return "out of memory";
    case Error::ReadFailed:       return "read failed";
    }
    return "unknown";
}

namespace {

// Where the bytes come from. A path goes through the filesystem; an image is
// already in memory. Everything after the fetch is identical, which is why the
// loader takes one of these rather than being written twice.
struct Source {
    const char* path;       // null when the image is in memory
    const u8*   memory;
    u64         size;
};

isize fetch(const Source& src, u64 offset, void* out, usize length)
{
    if (src.memory != nullptr) {
        if (offset >= src.size)
            return 0;
        u64 n = length;
        if (n > src.size - offset)
            n = src.size - offset;
        memcpy(out, src.memory + offset, static_cast<usize>(n));
        return static_cast<isize>(n);
    }
    return vfs::read(src.path, offset, out, length);
}

Error load_from(const Source& src, Image& out)
{
    out = Image{};

    const u64 total = src.size;
    if (total < sizeof(Header))
        return Error::TooSmall;

    Header header{};
    if (fetch(src, 0, &header, sizeof(header)) != static_cast<isize>(sizeof(header)))
        return Error::ReadFailed;

    if (memcmp(header.ident, kMagic, sizeof(kMagic)) != 0)
        return Error::NotElf;
    if (header.ident[4] != kClass64 || header.ident[5] != kLittleEndian)
        return Error::WrongClass;
    if (header.machine != kMachineX86_64)
        return Error::WrongMachine;
    if (header.type != kTypeExecutable)
        return Error::WrongType;
    if (header.program_header_size != sizeof(ProgramHeader) ||
        header.program_header_count == 0)
        return Error::BadProgramHeader;

    out.entry = header.entry;
    out.lowest = ~0ull;

    for (u16 i = 0; i < header.program_header_count; ++i) {
        const u64 offset = header.program_header_offset +
                           static_cast<u64>(i) * header.program_header_size;
        if (offset + sizeof(ProgramHeader) > total)
            return Error::BadProgramHeader;

        ProgramHeader segment{};
        if (fetch(src, offset, &segment, sizeof(segment)) !=
            static_cast<isize>(sizeof(segment)))
            return Error::ReadFailed;

        if (segment.type != kSegmentLoad)
            continue;
        if (segment.file_size > segment.memory_size)
            return Error::BadProgramHeader;
        if (segment.offset + segment.file_size > total)
            return Error::BadProgramHeader;
        if (segment.memory_size == 0)
            continue;

        // A segment rarely starts on a page boundary, and the bytes in front of
        // it inside the first page belong to it too - so round outward.
        const vaddr_t start = segment.virtual_address & ~(vmm::kPageSize - 1);
        const vaddr_t end   = (segment.virtual_address + segment.memory_size
                               + vmm::kPageSize - 1) & ~(vmm::kPageSize - 1);

        // Everything is mapped writable during load because the loader has to
        // write into it. A read-only segment gets its permissions tightened
        // once its contents are in place. User so ring 3 can reach it - the
        // whole point of loading it as a process rather than a kernel blob.
        u64 flags = vmm::Write | vmm::User;
        if ((segment.flags & kFlagExecute) == 0)
            flags |= vmm::NoExecute;

        for (vaddr_t page = start; page < end; page += vmm::kPageSize) {
            const paddr_t frame = pmm::alloc();
            if (frame == 0)
                return Error::OutOfMemory;
            if (!vmm::map(page, frame, flags)) {
                pmm::free(frame);
                return Error::OutOfMemory;
            }
            memset(reinterpret_cast<void*>(page), 0, vmm::kPageSize);
        }

        if (segment.file_size > 0) {
            const isize got = fetch(src, segment.offset,
                                    reinterpret_cast<void*>(segment.virtual_address),
                                    segment.file_size);
            if (got < 0 || static_cast<u64>(got) != segment.file_size)
                return Error::ReadFailed;
        }

        // Anything past file_size is .bss and must read as zero. The pages were
        // already cleared above, so there is nothing left to do here - but the
        // distinction is why memory_size and file_size are separate fields.

        if ((segment.flags & kFlagWrite) == 0) {
            u64 readonly = vmm::User;
            if ((segment.flags & kFlagExecute) == 0)
                readonly |= vmm::NoExecute;
            for (vaddr_t page = start; page < end; page += vmm::kPageSize)
                vmm::map(page, vmm::translate(page), readonly);
        }

        if (start < out.lowest)
            out.lowest = start;
        if (end > out.highest)
            out.highest = end;
        ++out.segments;
    }

    if (out.segments == 0)
        return Error::BadProgramHeader;

    return Error::None;
}

} // namespace

Error load(const char* path, Image& out)
{
    vfs::Stat info{};
    if (!vfs::stat(path, info) || info.type != vfs::Type::File)
        return Error::NotFound;
    const Source src{ path, nullptr, info.size };
    return load_from(src, out);
}

Error load_memory(const u8* image, usize size, Image& out)
{
    const Source src{ nullptr, image, size };
    return load_from(src, out);
}

} // namespace elf
