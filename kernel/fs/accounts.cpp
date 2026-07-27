#include <leah/accounts.hpp>
#include <leah/heap.hpp>
#include <leah/scheduler.hpp>
#include <leah/sha256.hpp>
#include <leah/string.hpp>
#include <leah/vfs.hpp>

namespace accounts {
namespace {

// name:uid:gid:home:salt:iterations:hex-digest
constexpr const char* kShadowPath = "/etc/shadow";

struct Entry {
    char name[kMaxName];
    u32  uid;
    u32  gid;
    char home[kMaxHome];
    char salt[32];
    u32  iterations;
    u8   digest[crypto::kSha256Digest];
};

// Copy up to the next colon (or end of line) into `out`, and report where the
// field ended.
usize take_field(const char* line, usize offset, usize length, char* out,
                 usize out_size)
{
    usize n = 0;
    while (offset < length && line[offset] != ':' && line[offset] != '\n') {
        if (n + 1 < out_size)
            out[n++] = line[offset];
        ++offset;
    }
    out[n] = '\0';
    return offset < length && line[offset] == ':' ? offset + 1 : offset;
}

u32 parse_u32(const char* text)
{
    u32 value = 0;
    for (usize i = 0; text[i] >= '0' && text[i] <= '9'; ++i)
        value = value * 10 + static_cast<u32>(text[i] - '0');
    return value;
}

int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool parse_digest(const char* hex, u8* out)
{
    for (usize i = 0; i < crypto::kSha256Digest; ++i) {
        const int high = hex_value(hex[i * 2]);
        const int low  = hex_value(hex[i * 2 + 1]);
        if (high < 0 || low < 0)
            return false;
        out[i] = static_cast<u8>(high << 4 | low);
    }
    return true;
}

// Walk the shadow file looking for a line whose first field matches. Returns
// false when there is no such account, or no readable file.
bool find(const char* name, u32 uid, bool by_name, Entry& out)
{
    u64 size = 0;
    char* text = vfs::read_entire_file(kShadowPath, &size);
    if (text == nullptr)
        return false;

    bool found = false;
    usize offset = 0;
    while (offset < size && !found) {
        // Isolate one line.
        usize end = offset;
        while (end < size && text[end] != '\n')
            ++end;

        Entry entry{};
        char field[64];
        usize p = offset;
        p = take_field(text, p, end, entry.name, sizeof(entry.name));
        p = take_field(text, p, end, field, sizeof(field));
        entry.uid = parse_u32(field);
        p = take_field(text, p, end, field, sizeof(field));
        entry.gid = parse_u32(field);
        p = take_field(text, p, end, entry.home, sizeof(entry.home));
        p = take_field(text, p, end, entry.salt, sizeof(entry.salt));
        p = take_field(text, p, end, field, sizeof(field));
        entry.iterations = parse_u32(field);

        char hex[80];
        take_field(text, p, end, hex, sizeof(hex));

        const bool matches = by_name ? strcmp(entry.name, name) == 0
                                     : entry.uid == uid;
        if (entry.name[0] != '\0' && matches) {
            // A malformed digest is treated as no account rather than as an
            // account nobody can log in to by accident.
            if (parse_digest(hex, entry.digest)) {
                out = entry;
                found = true;
            }
        }
        offset = end + 1;
    }

    kfree(text);
    return found;
}

} // namespace

bool login(const char* user, const char* password, char* home_out,
           usize home_size)
{
    Entry entry{};
    if (!find(user, 0, true, entry))
        return false;

    // Root already has every privilege this would grant, so asking it for a
    // password protects nothing.
    if (scheduler::current_uid() != 0) {
        if (password == nullptr)
            return false;
        u8 digest[crypto::kSha256Digest];
        crypto::password_hash(entry.salt, password, entry.iterations, digest);
        if (!crypto::digest_equal(digest, entry.digest))
            return false;
    }

    // Set both at once and unconditionally. The ordinary setuid rule - only
    // root may change credentials - is exactly wrong here: the whole point is
    // that a correct password authorises the change, and going through that
    // rule would refuse every successful login by a non-root user.
    scheduler::set_credentials(entry.uid, entry.gid);

    if (home_out != nullptr && home_size > 0) {
        usize n = 0;
        while (entry.home[n] != '\0' && n + 1 < home_size) {
            home_out[n] = entry.home[n];
            ++n;
        }
        home_out[n] = '\0';
    }
    return true;
}

bool lookup_uid(u32 uid, char* name_out, usize name_size)
{
    Entry entry{};
    if (!find(nullptr, uid, false, entry))
        return false;
    usize n = 0;
    while (entry.name[n] != '\0' && n + 1 < name_size) {
        name_out[n] = entry.name[n];
        ++n;
    }
    name_out[n] = '\0';
    return true;
}

} // namespace accounts
