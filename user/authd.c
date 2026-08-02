/* authd - the account database, and the only thing that reads the hashes.
 *
 * This was four system calls and four hundred lines inside the kernel, and the
 * argument for putting it there was sound as far as it went: without
 * setuid-on-exec, an ordinary `su` cannot open /etc/shadow, and opening the
 * shadow file to everyone to work around that would hand out the one thing in
 * it worth guarding. The kernel could already read it, so the check went in the
 * kernel.
 *
 * But none of that argues for ring 0 specifically. It argues for *something*
 * privileged doing the reading and answering yes or no. A server does that just
 * as well: authd runs as root, both files are its own, and what crosses the
 * port is a verdict rather than a digest - so the hash still never reaches the
 * process that asked, which was the property being defended.
 *
 * What stayed behind is the part that genuinely needs the kernel: changing a
 * running process's identity. authd decides, and asks; the kernel does it.
 */

#include <auth.h>
#include <fcntl.h>
#include <ipc.h>
#include <sha256.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

/* name:uid:gid:home:salt:iterations:hex-digest */
#define SHADOW_PATH "/etc/shadow"
#define PASSWD_PATH "/etc/passwd"
#define ITERATIONS  4096
#define MAX_NAME    32
#define MAX_HOME    128

struct entry {
    char     name[MAX_NAME];
    unsigned uid;
    unsigned gid;
    char     home[MAX_HOME];
    char     salt[32];
    unsigned iterations;
    uint8_t  digest[SHA256_DIGEST];
};

/* ---- the file, read whole and written whole -------------------------------
 *
 * A few lines is not worth an incremental update, and rewriting the whole thing
 * means a half-finished edit cannot leave an account no one can log in to. */

static char* slurp(const char* path, long* size_out)
{
    const int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    struct stat st;
    if (stat(path, &st) != 0) { close(fd); return 0; }

    char* text = malloc((size_t)st.st_size + 1);
    if (text == 0) { close(fd); return 0; }
    const long got = read(fd, text, (unsigned long)st.st_size);
    close(fd);
    if (got < 0) { free(text); return 0; }
    text[got] = '\0';
    if (size_out != 0)
        *size_out = got;
    return text;
}

static int spill(const char* path, const char* data, long length)
{
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return -1;
    const long put = write(fd, data, (unsigned long)length);
    close(fd);
    return put == length ? 0 : -1;
}

/* ---- parsing --------------------------------------------------------------- */

static long take_field(const char* line, long offset, long length, char* out,
                       size_t out_size)
{
    size_t n = 0;
    while (offset < length && line[offset] != ':' && line[offset] != '\n') {
        if (n + 1 < out_size)
            out[n++] = line[offset];
        ++offset;
    }
    out[n] = '\0';
    return offset < length && line[offset] == ':' ? offset + 1 : offset;
}

static unsigned parse_u32(const char* text)
{
    unsigned value = 0;
    size_t i;
    for (i = 0; text[i] >= '0' && text[i] <= '9'; ++i)
        value = value * 10 + (unsigned)(text[i] - '0');
    return value;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_digest(const char* hex, uint8_t* out)
{
    size_t i;
    for (i = 0; i < SHA256_DIGEST; ++i) {
        const int high = hex_value(hex[i * 2]);
        const int low  = hex_value(hex[i * 2 + 1]);
        if (high < 0 || low < 0)
            return 0;
        out[i] = (uint8_t)(high << 4 | low);
    }
    return 1;
}

/* Walk the shadow file for a line matching by name or by uid. */
static int find(const char* name, unsigned uid, int by_name, struct entry* out)
{
    long size = 0;
    char* text = slurp(SHADOW_PATH, &size);
    if (text == 0)
        return 0;

    int found = 0;
    long offset = 0;
    while (offset < size && !found) {
        long end = offset;
        while (end < size && text[end] != '\n')
            ++end;

        struct entry e;
        char field[64];
        char hex[80];
        long p = offset;
        memset(&e, 0, sizeof(e));
        p = take_field(text, p, end, e.name, sizeof(e.name));
        p = take_field(text, p, end, field, sizeof(field));
        e.uid = parse_u32(field);
        p = take_field(text, p, end, field, sizeof(field));
        e.gid = parse_u32(field);
        p = take_field(text, p, end, e.home, sizeof(e.home));
        p = take_field(text, p, end, e.salt, sizeof(e.salt));
        p = take_field(text, p, end, field, sizeof(field));
        e.iterations = parse_u32(field);
        take_field(text, p, end, hex, sizeof(hex));

        const int matches = by_name ? strcmp(e.name, name) == 0 : e.uid == uid;
        if (e.name[0] != '\0' && matches) {
            /* A malformed digest is no account, rather than an account nobody
             * can log in to by accident. */
            if (parse_digest(hex, e.digest)) {
                *out = e;
                found = 1;
            }
        }
        offset = end + 1;
    }
    free(text);
    return found;
}

static int exists(const char* user)
{
    struct entry e;
    return find(user, 0, 1, &e);
}

static int lookup_uid(unsigned uid, char* name_out, size_t name_size)
{
    struct entry e;
    size_t n = 0;
    if (!find(0, uid, 0, &e))
        return 0;
    while (e.name[n] != '\0' && n + 1 < name_size) {
        name_out[n] = e.name[n];
        ++n;
    }
    name_out[n] = '\0';
    return 1;
}

/* ---- writing --------------------------------------------------------------- */

static int append_line(const char* path, const char* line)
{
    long size = 0;
    char* existing = slurp(path, &size);
    const size_t line_length = strlen(line);
    char* combined = malloc((size_t)size + line_length + 1);
    int ok;

    if (combined == 0) { free(existing); return 0; }
    if (existing != 0)
        memcpy(combined, existing, (size_t)size);
    memcpy(combined + size, line, line_length);

    ok = spill(path, combined, size + (long)line_length) == 0;
    free(combined);
    free(existing);
    return ok;
}

static size_t put_u32(char* out, unsigned value)
{
    char digits[12];
    size_t n = 0, i;
    do {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);
    for (i = 0; i < n; ++i)
        out[i] = digits[n - 1 - i];
    return n;
}

static size_t put_text(char* out, const char* text)
{
    size_t n = 0;
    while (text[n] != '\0') { out[n] = text[n]; ++n; }
    return n;
}

/* A salt only has to be unique, not secret - its job is to stop two users with
 * the same password sharing a digest. The cycle counter plus a sequence number
 * gives that, and rdtsc is readable from ring 3, so moving out here cost
 * nothing that mattered. */
static void make_salt(char* out, size_t length)
{
    static const char alphabet[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    static unsigned sequence = 0;
    unsigned low, high;
    unsigned long mix;
    size_t i;

    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    mix = ((unsigned long)high << 32 | low);
    mix ^= (unsigned long)(++sequence) * 0x9E3779B97F4A7C15ul;

    for (i = 0; i + 1 < length; ++i) {
        mix = mix * 6364136223846793005ul + 1442695040888963407ul;
        out[i] = alphabet[(mix >> 33) % (sizeof(alphabet) - 1)];
    }
    out[length - 1] = '\0';
}

static size_t put_digest(char* out, const uint8_t* digest)
{
    static const char hex[] = "0123456789abcdef";
    size_t n = 0, i;
    for (i = 0; i < SHA256_DIGEST; ++i) {
        out[n++] = hex[digest[i] >> 4];
        out[n++] = hex[digest[i] & 0x0F];
    }
    return n;
}

/* The lowest unused ordinary uid. Handing out one already taken would not fail
 * loudly - it would silently make two accounts the same person, because every
 * permission check works on the number and not the name. */
static unsigned next_free_uid(void)
{
    long size = 0;
    char* text = slurp(SHADOW_PATH, &size);
    unsigned highest = 999;
    long offset = 0;

    if (text == 0)
        return 1000;
    while (offset < size) {
        char field[64];
        long end = offset, p;
        unsigned uid;
        while (end < size && text[end] != '\n')
            ++end;
        p = take_field(text, offset, end, field, sizeof(field));   /* name */
        take_field(text, p, end, field, sizeof(field));            /* uid */
        uid = parse_u32(field);
        if (uid > highest && uid < 60000)
            highest = uid;
        offset = end + 1;
    }
    free(text);
    return highest + 1;
}

/* ---- the three operations that need a password ----------------------------- */

static int do_login(unsigned caller_uid, const char* user, const char* password,
                    struct entry* out)
{
    if (!find(user, 0, 1, out))
        return 0;

    /* Everything goes through root. An ordinary user may only climb to root;
     * reaching another ordinary user means going up and coming back down, which
     * costs two passwords rather than one. */
    if (caller_uid != 0 && out->uid != 0)
        return 0;

    /* The target's password every time, root included: being root is authority
     * over the machine, not knowledge of everyone's password. */
    {
        uint8_t digest[SHA256_DIGEST];
        password_hash(out->salt, password == 0 ? "" : password,
                      out->iterations, digest);
        return digest_equal(digest, out->digest);
    }
}

static int do_useradd(unsigned caller_uid, const char* user,
                      const char* password, unsigned uid, unsigned gid,
                      const char* home)
{
    char salt[9];
    uint8_t digest[SHA256_DIGEST];
    char line[512];
    size_t n = 0, i;

    if (caller_uid != 0)                    /* only root makes accounts */
        return 0;
    if (user == 0 || user[0] == '\0' || password == 0)
        return 0;
    if (exists(user))
        return 0;

    /* Zero means "pick one", since a second root is never what was wanted. */
    if (uid == 0) {
        uid = next_free_uid();
        gid = uid;
    } else {
        char taken[MAX_NAME];
        if (lookup_uid(uid, taken, sizeof(taken)))
            return 0;                       /* two accounts, one identity */
    }

    /* A colon would split into extra fields and corrupt the file. Refuse rather
     * than silently mangle it. */
    for (i = 0; user[i] != '\0'; ++i)
        if (user[i] == ':' || user[i] == '\n')
            return 0;

    make_salt(salt, sizeof(salt));
    password_hash(salt, password, ITERATIONS, digest);

    n += put_text(line + n, user);        line[n++] = ':';
    n += put_u32(line + n, uid);          line[n++] = ':';
    n += put_u32(line + n, gid);          line[n++] = ':';
    n += put_text(line + n, home);        line[n++] = ':';
    n += put_text(line + n, salt);        line[n++] = ':';
    n += put_u32(line + n, ITERATIONS);   line[n++] = ':';
    n += put_digest(line + n, digest);
    line[n++] = '\n';
    line[n] = '\0';
    if (!append_line(SHADOW_PATH, line))
        return 0;

    /* The public half, which carries no secret and stays world readable. */
    n = 0;
    n += put_text(line + n, user);        line[n++] = ':';
    line[n++] = 'x';                      line[n++] = ':';
    n += put_u32(line + n, uid);          line[n++] = ':';
    n += put_u32(line + n, gid);          line[n++] = ':';
    n += put_text(line + n, home);        line[n++] = ':';
    n += put_text(line + n, "/bin/sh.elf");
    line[n++] = '\n';
    line[n] = '\0';
    append_line(PASSWD_PATH, line);

    /* Somewhere to live, made here so an account is never created without it. */
    if (home != 0 && home[0] != '\0') {
        struct stat st;
        if (stat(home, &st) != 0) {
            mkdir(home);
            chown(home, uid, gid);
            chmod(home, 0700);
        }
    }
    return 1;
}

static int do_passwd(unsigned caller_uid, const char* user, const char* old_pw,
                     const char* new_pw)
{
    struct entry e;
    long size = 0;
    char* text;
    char* out;
    char salt[9];
    uint8_t digest[SHA256_DIGEST];
    long offset = 0, written = 0;
    int ok;

    if (!find(user, 0, 1, &e) || new_pw == 0)
        return 0;

    if (caller_uid != 0) {
        /* Someone else's password is root's business, and your own means
         * proving you know it. */
        if (caller_uid != e.uid || old_pw == 0)
            return 0;
        password_hash(e.salt, old_pw, e.iterations, digest);
        if (!digest_equal(digest, e.digest))
            return 0;
    }

    text = slurp(SHADOW_PATH, &size);
    if (text == 0)
        return 0;

    /* A fresh salt each time, so the same password twice does not produce the
     * same digest. */
    make_salt(salt, sizeof(salt));
    password_hash(salt, new_pw, ITERATIONS, digest);

    out = malloc((size_t)size + 512);
    if (out == 0) { free(text); return 0; }

    while (offset < size) {
        char name[MAX_NAME];
        long end = offset;
        while (end < size && text[end] != '\n')
            ++end;
        take_field(text, offset, end, name, sizeof(name));

        if (strcmp(name, user) == 0) {
            long n = written;
            n += (long)put_text(out + n, user);     out[n++] = ':';
            n += (long)put_u32(out + n, e.uid);     out[n++] = ':';
            n += (long)put_u32(out + n, e.gid);     out[n++] = ':';
            n += (long)put_text(out + n, e.home);   out[n++] = ':';
            n += (long)put_text(out + n, salt);     out[n++] = ':';
            n += (long)put_u32(out + n, ITERATIONS); out[n++] = ':';
            n += (long)put_digest(out + n, digest);
            written = n;
        } else {
            long i;
            for (i = offset; i < end; ++i)
                out[written++] = text[i];
        }
        out[written++] = '\n';
        offset = end + 1;
    }

    ok = spill(SHADOW_PATH, out, written) == 0;
    free(out);
    free(text);
    return ok;
}

/* ---- the port -------------------------------------------------------------- */

/* Arguments arrive packed as consecutive C strings, because a message carries
 * one buffer and this way none of them needs a length. */
static const char* nth_string(const struct ipc_message* m, unsigned index)
{
    unsigned at = 0;
    unsigned i;
    for (i = 0; i < index; ++i) {
        while (at < sizeof(m->data) && m->data[at] != '\0')
            ++at;
        if (at >= sizeof(m->data))
            return "";
        ++at;
    }
    return at < sizeof(m->data) ? &m->data[at] : "";
}

int main(void)
{
    const int port = port_create(IPC_PORT_AUTH);
    if (port < 0) {
        printf("authd: something already answers for the accounts\n");
        return 1;
    }
    printf("authd[%d]: /etc/shadow is mine, in ring 3\n", getpid());

    for (;;) {
        struct ipc_message m, r;
        unsigned from = 0;
        const int handle = ipc_recv(port, &m, &from);
        if (handle < 0)
            continue;

        /* Who is asking decides what they may do, and the kernel is the only
         * honest source for that - a uid in the message would be a uid the
         * caller chose. */
        const unsigned caller_uid = (unsigned)__syscall(SYS_uidof, from, 0, 0, 0, 0);

        memset(&r, 0, sizeof(r));
        r.tag = m.tag;
        r.word[0] = -1;

        if (m.tag == AUTH_LOGIN) {
            struct entry e;
            memset(&e, 0, sizeof(e));
            if (do_login(caller_uid, nth_string(&m, 0), nth_string(&m, 1), &e)) {
                /* Verified. The kernel makes it true of the caller; nothing
                 * about the account leaves here except where to go home. */
                if (__syscall(SYS_setcreds, from, e.uid, e.gid, 0, 0) == 0) {
                    r.word[0] = 0;
                    r.word[1] = e.uid;
                    r.word[2] = e.gid;
                    r.bytes = (unsigned)put_text(r.data, e.home) + 1;
                }
            }
            /* The hash and the salt go out of scope here and never went
             * anywhere else. */
            memset(&e, 0, sizeof(e));
        } else if (m.tag == AUTH_USERADD) {
            r.word[0] = do_useradd(caller_uid, nth_string(&m, 0),
                                   nth_string(&m, 1), (unsigned)m.word[0],
                                   (unsigned)m.word[1], nth_string(&m, 2))
                            ? 0 : -1;
        } else if (m.tag == AUTH_PASSWD) {
            const char* old_pw = nth_string(&m, 1);
            r.word[0] = do_passwd(caller_uid, nth_string(&m, 0),
                                  old_pw[0] == '\0' ? 0 : old_pw,
                                  nth_string(&m, 2)) ? 0 : -1;
        } else if (m.tag == AUTH_UIDNAME) {
            char name[MAX_NAME];
            if (lookup_uid((unsigned)m.word[0], name, sizeof(name))) {
                r.bytes = (unsigned)put_text(r.data, name) + 1;
                r.word[0] = 0;
            }
        } else if (m.tag == AUTH_EXISTS) {
            r.word[0] = exists(nth_string(&m, 0)) ? 1 : 0;
        }

        /* The password the caller sent is in this process's memory until it is
         * overwritten. It goes no further than the reply. */
        memset(m.data, 0, sizeof(m.data));
        ipc_reply(handle, &r);
    }
}
