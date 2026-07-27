#include <leah/accounts.hpp>
#include <leah/heap.hpp>
#include <leah/hpet.hpp>
#include <leah/timer.hpp>
#include <leah/scheduler.hpp>
#include <leah/sha256.hpp>
#include <leah/string.hpp>
#include <leah/vfs.hpp>

namespace accounts {
namespace {

// name:uid:gid:home:salt:iterations:hex-digest
constexpr const char* kShadowPath = "/etc/shadow";
constexpr const char* kPasswdPath = "/etc/passwd";
constexpr u32 kIterations = 4096;

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

// Append text to a file the kernel owns. Read-modify-write of the whole thing,
// which is fine for a file of a few lines and avoids needing append-mode
// plumbing in the VFS.
bool append_line(const char* path, const char* line)
{
    u64 size = 0;
    char* existing = vfs::read_entire_file(path, &size);

    usize line_length = 0;
    while (line[line_length] != '\0')
        ++line_length;

    const usize total = static_cast<usize>(size) + line_length;
    char* combined = static_cast<char*>(kmalloc(total + 1));
    if (combined == nullptr) {
        kfree(existing);
        return false;
    }
    if (existing != nullptr)
        memcpy(combined, existing, static_cast<usize>(size));
    memcpy(combined + size, line, line_length);

    const bool ok = vfs::write_entire_file(path, combined, total);
    kfree(combined);
    kfree(existing);
    return ok;
}

// Write a decimal number into `out`, returning how many characters it took.
usize put_u32(char* out, u32 value)
{
    char digits[12];
    usize n = 0;
    do {
        digits[n++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    for (usize i = 0; i < n; ++i)
        out[i] = digits[n - 1 - i];
    return n;
}

usize put_text(char* out, const char* text)
{
    usize n = 0;
    while (text[n] != '\0') {
        out[n] = text[n];
        ++n;
    }
    return n;
}

// A salt only has to be unique, not secret - its job is to stop two users with
// the same password sharing a digest. The clock plus a counter gives that
// without needing an entropy source the kernel does not have.
void make_salt(char* out, usize length)
{
    static u32 sequence = 0;
    static const char kAlphabet[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    u64 mix = hpet::available() ? hpet::counter() : timer::ticks();
    mix ^= static_cast<u64>(++sequence) * 0x9E3779B97F4A7C15ull;

    for (usize i = 0; i + 1 < length; ++i) {
        mix = mix * 6364136223846793005ull + 1442695040888963407ull;
        out[i] = kAlphabet[(mix >> 33) % (sizeof(kAlphabet) - 1)];
    }
    out[length - 1] = '\0';
}

} // namespace

bool login(const char* user, const char* password, char* home_out,
           usize home_size)
{
    Entry entry{};
    if (!find(user, 0, true, entry))
        return false;

    // Everything goes through root. An ordinary user may only climb to root;
    // reaching another ordinary user means going up and then back down, which
    // costs two passwords rather than one.
    if (scheduler::current_uid() != 0 && entry.uid != 0)
        return false;

    // The target's password is required at every step, root included: being
    // root is authority over the machine, not knowledge of everyone's password.
    if (password == nullptr)
        return false;
    u8 digest[crypto::kSha256Digest];
    crypto::password_hash(entry.salt, password, entry.iterations, digest);
    if (!crypto::digest_equal(digest, entry.digest))
        return false;

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

bool exists(const char* user)
{
    Entry entry{};
    return find(user, 0, true, entry);
}

// The lowest unused ordinary uid. Handing out one that is already taken would
// not fail loudly - it would silently make two accounts the same person, since
// every permission check works on the number rather than the name.
u32 next_free_uid()
{
    u64 size = 0;
    char* text = vfs::read_entire_file(kShadowPath, &size);
    if (text == nullptr)
        return 1000;

    u32 highest = 999;
    usize offset = 0;
    while (offset < size) {
        usize end = offset;
        while (end < size && text[end] != '\n')
            ++end;

        char field[64];
        usize p = take_field(text, offset, end, field, sizeof(field));   // name
        take_field(text, p, end, field, sizeof(field));                  // uid
        const u32 uid = parse_u32(field);
        if (uid > highest && uid < 60000)
            highest = uid;
        offset = end + 1;
    }
    kfree(text);
    return highest + 1;
}

bool uid_taken(u32 uid)
{
    char name[kMaxName];
    return lookup_uid(uid, name, sizeof(name));
}

bool create(const char* user, const char* password, u32 uid, u32 gid,
            const char* home)
{
    if (scheduler::current_uid() != 0)
        return false;                       // only root makes accounts
    if (user == nullptr || user[0] == '\0' || password == nullptr)
        return false;
    if (exists(user))
        return false;

    // Zero means "pick one", since a second root is never what was wanted.
    if (uid == 0) {
        uid = next_free_uid();
        gid = uid;
    } else if (uid_taken(uid)) {
        return false;                       // two accounts, one identity
    }

    // A name with a colon in it would split into extra fields and corrupt the
    // file; refuse rather than silently mangle it.
    for (usize i = 0; user[i] != '\0'; ++i) {
        if (user[i] == ':' || user[i] == '\n')
            return false;
    }

    char salt[9];
    make_salt(salt, sizeof(salt));

    u8 digest[crypto::kSha256Digest];
    crypto::password_hash(salt, password, kIterations, digest);

    // name:uid:gid:home:salt:iterations:digest
    char line[512];
    usize n = 0;
    n += put_text(line + n, user);      line[n++] = ':';
    n += put_u32(line + n, uid);        line[n++] = ':';
    n += put_u32(line + n, gid);        line[n++] = ':';
    n += put_text(line + n, home);      line[n++] = ':';
    n += put_text(line + n, salt);      line[n++] = ':';
    n += put_u32(line + n, kIterations); line[n++] = ':';
    for (usize i = 0; i < crypto::kSha256Digest; ++i) {
        static const char kHex[] = "0123456789abcdef";
        line[n++] = kHex[digest[i] >> 4];
        line[n++] = kHex[digest[i] & 0x0F];
    }
    line[n++] = '\n';
    line[n] = '\0';
    if (!append_line(kShadowPath, line))
        return false;

    // The public half, which carries no secret and stays world readable.
    n = 0;
    n += put_text(line + n, user);      line[n++] = ':';
    line[n++] = 'x';                    line[n++] = ':';
    n += put_u32(line + n, uid);        line[n++] = ':';
    n += put_u32(line + n, gid);        line[n++] = ':';
    n += put_text(line + n, home);      line[n++] = ':';
    n += put_text(line + n, "/bin/sh.elf");
    line[n++] = '\n';
    line[n] = '\0';
    append_line(kPasswdPath, line);

    // The home directory, owned by its user. Created here rather than by the
    // caller so an account is never made without somewhere to live.
    if (home != nullptr && home[0] != '\0') {
        vfs::Stat existing{};
        if (!vfs::stat(home, existing)) {
            vfs::create(home, vfs::Type::Directory);
            vfs::chown(home, uid, gid);
            vfs::chmod(home, 0700);
        }
    }
    return true;
}

bool set_password(const char* user, const char* old_password,
                  const char* new_password)
{
    Entry entry{};
    if (!find(user, 0, true, entry) || new_password == nullptr)
        return false;

    const u32 caller = scheduler::current_uid();
    if (caller != 0) {
        // Changing someone else's password is root's business.
        if (caller != entry.uid)
            return false;
        // And changing your own means proving you know it.
        if (old_password == nullptr)
            return false;
        u8 digest[crypto::kSha256Digest];
        crypto::password_hash(entry.salt, old_password, entry.iterations, digest);
        if (!crypto::digest_equal(digest, entry.digest))
            return false;
    }

    // Rewrite the shadow file with this account's line replaced. A fresh salt
    // each time, so the same password twice does not produce the same digest.
    u64 size = 0;
    char* text = vfs::read_entire_file(kShadowPath, &size);
    if (text == nullptr)
        return false;

    char salt[9];
    make_salt(salt, sizeof(salt));
    u8 digest[crypto::kSha256Digest];
    crypto::password_hash(salt, new_password, kIterations, digest);

    char* out = static_cast<char*>(kmalloc(static_cast<usize>(size) + 512));
    if (out == nullptr) {
        kfree(text);
        return false;
    }

    usize written = 0;
    usize offset = 0;
    while (offset < size) {
        usize end = offset;
        while (end < size && text[end] != '\n')
            ++end;

        char name[kMaxName];
        take_field(text, offset, end, name, sizeof(name));

        if (strcmp(name, user) == 0) {
            usize n = written;
            n += put_text(out + n, user);       out[n++] = ':';
            n += put_u32(out + n, entry.uid);   out[n++] = ':';
            n += put_u32(out + n, entry.gid);   out[n++] = ':';
            n += put_text(out + n, entry.home); out[n++] = ':';
            n += put_text(out + n, salt);       out[n++] = ':';
            n += put_u32(out + n, kIterations); out[n++] = ':';
            for (usize i = 0; i < crypto::kSha256Digest; ++i) {
                static const char kHex[] = "0123456789abcdef";
                out[n++] = kHex[digest[i] >> 4];
                out[n++] = kHex[digest[i] & 0x0F];
            }
            written = n;
        } else {
            for (usize i = offset; i < end; ++i)
                out[written++] = text[i];
        }
        out[written++] = '\n';
        offset = end + 1;
    }

    const bool ok = vfs::write_entire_file(kShadowPath, out, written);
    kfree(out);
    kfree(text);
    return ok;
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
