#include <leah/acpi.hpp>
#include <leah/memory.hpp>
#include <leah/string.hpp>

namespace acpi {
namespace {

// Every ACPI table starts with this header; `length` covers the header and the
// body, and the bytes of the whole thing must sum to zero mod 256.
struct [[gnu::packed]] TableHeader {
    char signature[4];
    u32  length;
    u8   revision;
    u8   checksum;
    char oem_id[6];
    char oem_table_id[8];
    u32  oem_revision;
    u32  creator_id;
    u32  creator_revision;
};

struct [[gnu::packed]] Rsdp {
    char signature[8];      // "RSD PTR "
    u8   checksum;          // over the first 20 bytes
    char oem_id[6];
    u8   revision;          // 0 = ACPI 1.0 (RSDT only), 2+ = XSDT present
    u32  rsdt_address;
    // ACPI 2.0+ continues:
    u32  length;
    u64  xsdt_address;
    u8   extended_checksum; // over the whole `length` bytes
    u8   reserved[3];
};

// MADT entry types we care about.
constexpr u8 kMadtLocalApic         = 0;
constexpr u8 kMadtIoApic            = 1;
constexpr u8 kMadtSourceOverride    = 2;
constexpr u8 kMadtLocalApicOverride = 5;

bool g_available = false;
u64  g_lapic_address = 0;
u64  g_hpet_address = 0;

IoApic g_io_apics[kMaxIoApics];
usize  g_io_apic_count = 0;

SourceOverride g_overrides[kMaxOverrides];
usize          g_override_count = 0;

u8    g_cpu_apic_ids[kMaxCpus];
usize g_cpu_count = 0;

usize g_table_count = 0;

// The tables live in low physical memory that the direct map already covers, so
// reaching them is an offset rather than a mapping.
template <typename T>
const T* at_phys(u64 phys)
{
    return reinterpret_cast<const T*>(memory::phys_to_direct(phys));
}

bool checksum_ok(const void* base, usize length)
{
    const auto* bytes = static_cast<const u8*>(base);
    u8 sum = 0;
    for (usize i = 0; i < length; ++i)
        sum = static_cast<u8>(sum + bytes[i]);
    return sum == 0;
}

// Scan a physical range on 16-byte boundaries for the RSDP's signature. The
// pointer is only ever aligned like that, which is what makes the search cheap.
const Rsdp* scan_for_rsdp(u64 start, u64 end)
{
    for (u64 phys = start; phys + sizeof(Rsdp) <= end; phys += 16) {
        const auto* candidate = at_phys<Rsdp>(phys);
        if (memcmp(candidate->signature, "RSD PTR ", 8) != 0)
            continue;
        if (!checksum_ok(candidate, 20))
            continue;                       // signature matched by accident
        if (candidate->revision >= 2 && !checksum_ok(candidate, candidate->length))
            continue;
        return candidate;
    }
    return nullptr;
}

const Rsdp* find_rsdp()
{
    // The BIOS records the Extended BIOS Data Area's segment at 0x40E. It is
    // the first place to look; the ROM area is the fallback.
    const u64 ebda = static_cast<u64>(*at_phys<u16>(0x40E)) << 4;
    if (ebda >= 0x400 && ebda < 0xA0000) {
        if (const Rsdp* found = scan_for_rsdp(ebda, ebda + 1024))
            return found;
    }
    return scan_for_rsdp(0xE0000, 0x100000);
}

void parse_madt(const TableHeader* table)
{
    const auto* bytes = reinterpret_cast<const u8*>(table);
    g_lapic_address = *reinterpret_cast<const u32*>(bytes + 36);

    // Entries run from offset 44 to the end of the table, each self-describing
    // its own length - so an unknown type is skipped rather than fatal.
    usize offset = 44;
    while (offset + 2 <= table->length) {
        const u8 type   = bytes[offset];
        const u8 length = bytes[offset + 1];
        if (length < 2)
            break;                          // malformed; stop rather than spin

        switch (type) {
        case kMadtLocalApic: {
            const u32 flags = *reinterpret_cast<const u32*>(bytes + offset + 4);
            if ((flags & 1) != 0 && g_cpu_count < kMaxCpus)   // enabled only
                g_cpu_apic_ids[g_cpu_count++] = bytes[offset + 3];
            break;
        }
        case kMadtIoApic:
            if (g_io_apic_count < kMaxIoApics) {
                IoApic& io = g_io_apics[g_io_apic_count++];
                io.id       = bytes[offset + 2];
                io.address  = *reinterpret_cast<const u32*>(bytes + offset + 4);
                io.gsi_base = *reinterpret_cast<const u32*>(bytes + offset + 8);
            }
            break;
        case kMadtSourceOverride:
            if (g_override_count < kMaxOverrides) {
                SourceOverride& ov = g_overrides[g_override_count++];
                ov.source = bytes[offset + 3];
                ov.gsi    = *reinterpret_cast<const u32*>(bytes + offset + 4);
                ov.flags  = *reinterpret_cast<const u16*>(bytes + offset + 8);
            }
            break;
        case kMadtLocalApicOverride:
            // A 64-bit address that supersedes the 32-bit one in the header.
            g_lapic_address = *reinterpret_cast<const u64*>(bytes + offset + 4);
            break;
        default:
            break;
        }
        offset += length;
    }
}

void parse_hpet(const TableHeader* table)
{
    // Layout: the 36-byte header, a 32-bit event-timer block id, then a
    // Generic Address Structure at offset 40 whose own 64-bit address field
    // begins 4 bytes in, at offset 44.
    const auto* bytes = reinterpret_cast<const u8*>(table);
    if (table->length >= 52)
        g_hpet_address = *reinterpret_cast<const u64*>(bytes + 44);
}

void parse_table(u64 phys)
{
    const auto* header = at_phys<TableHeader>(phys);
    if (header->length < sizeof(TableHeader) || !checksum_ok(header, header->length))
        return;
    ++g_table_count;

    if (memcmp(header->signature, "APIC", 4) == 0)
        parse_madt(header);
    else if (memcmp(header->signature, "HPET", 4) == 0)
        parse_hpet(header);
}

} // namespace

bool init()
{
    const Rsdp* rsdp = find_rsdp();
    if (rsdp == nullptr)
        return false;

    // ACPI 2.0's XSDT holds 64-bit pointers and supersedes the RSDT. Prefer it
    // when the revision says it exists and it actually validates.
    if (rsdp->revision >= 2 && rsdp->xsdt_address != 0) {
        const auto* xsdt = at_phys<TableHeader>(rsdp->xsdt_address);
        if (checksum_ok(xsdt, xsdt->length)) {
            const usize entries = (xsdt->length - sizeof(TableHeader)) / 8;
            const auto* pointers = reinterpret_cast<const u64*>(
                reinterpret_cast<const u8*>(xsdt) + sizeof(TableHeader));
            for (usize i = 0; i < entries; ++i)
                parse_table(pointers[i]);
            g_available = true;
            return true;
        }
    }

    if (rsdp->rsdt_address == 0)
        return false;
    const auto* rsdt = at_phys<TableHeader>(rsdp->rsdt_address);
    if (!checksum_ok(rsdt, rsdt->length))
        return false;

    const usize entries = (rsdt->length - sizeof(TableHeader)) / 4;
    const auto* pointers = reinterpret_cast<const u32*>(
        reinterpret_cast<const u8*>(rsdt) + sizeof(TableHeader));
    for (usize i = 0; i < entries; ++i)
        parse_table(pointers[i]);

    g_available = true;
    return true;
}

bool available() { return g_available; }

u64 local_apic_address() { return g_lapic_address; }

usize io_apic_count() { return g_io_apic_count; }
const IoApic& io_apic_at(usize index) { return g_io_apics[index]; }

usize override_count() { return g_override_count; }
const SourceOverride& override_at(usize index) { return g_overrides[index]; }

usize cpu_count() { return g_cpu_count; }
u8 cpu_apic_id(usize index) { return g_cpu_apic_ids[index]; }

u64 hpet_address() { return g_hpet_address; }

usize table_count() { return g_table_count; }

} // namespace acpi
