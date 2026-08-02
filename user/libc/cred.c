/* Credentials. The four calls that touched the account database are now a port
 * away instead of a ring away, and the signatures are unchanged - login, su,
 * passwd, adduser and the settings panel were never told where the check
 * happened, so none of them had to be edited when it moved. */

#include <auth.h>
#include <ipc.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static int g_auth = -2;             /* -2 not tried, -1 no server */

static int auth_port(void)
{
    if (g_auth == -2)
        g_auth = port_open(IPC_PORT_AUTH);
    return g_auth;
}

/* The arguments go in as consecutive C strings: a message carries one buffer,
 * and packed this way none of them needs a length beside it. */
static unsigned pack(struct ipc_message* m, unsigned at, const char* text)
{
    if (text != 0) {
        while (*text != '\0' && at + 1 < sizeof(m->data))
            m->data[at++] = *text++;
    }
    m->data[at++] = '\0';
    return at;
}

static int ask(struct ipc_message* q, struct ipc_message* a)
{
    const int port = auth_port();
    if (port < 0)
        return -1;
    memset(a, 0, sizeof(*a));
    if (ipc_call(port, q, a) != 0)
        return -1;
    /* Wipe our copy: a password should not outlive the question. */
    memset(q->data, 0, sizeof(q->data));
    return 0;
}

unsigned getuid(void) { return (unsigned)__syscall(SYS_getuid, 0, 0, 0, 0, 0); }
unsigned getgid(void) { return (unsigned)__syscall(SYS_getgid, 0, 0, 0, 0, 0); }

int setuid(unsigned uid)
{
    return (int)__syscall(SYS_setuid, (long)uid, 0, 0, 0, 0);
}

int setgid(unsigned gid)
{
    return (int)__syscall(SYS_setgid, (long)gid, 0, 0, 0, 0);
}

int login(const char* user, const char* password, char* home)
{
    struct ipc_message q, a;
    unsigned at = 0;
    memset(&q, 0, sizeof(q));
    q.tag = AUTH_LOGIN;
    at = pack(&q, at, user);
    at = pack(&q, at, password);
    q.bytes = at;
    if (ask(&q, &a) != 0 || a.word[0] != 0)
        return -1;
    /* The server has already asked the kernel to make this true of us; what
     * comes back is only where the account lives. */
    if (home != 0) {
        unsigned n = 0;
        while (n + 1 < 128 && a.data[n] != '\0') { home[n] = a.data[n]; ++n; }
        home[n] = '\0';
    }
    return 0;
}

void setecho(int on)
{
    __syscall(SYS_setecho, on, 0, 0, 0, 0);
}

int username(unsigned uid, char* name_out)
{
    struct ipc_message q, a;
    unsigned n = 0;
    memset(&q, 0, sizeof(q));
    q.tag = AUTH_UIDNAME;
    q.word[0] = (long)uid;
    if (ask(&q, &a) != 0 || a.word[0] != 0)
        return -1;
    while (n + 1 < 32 && a.data[n] != '\0') { name_out[n] = a.data[n]; ++n; }
    name_out[n] = '\0';
    return 0;
}

int useradd(const char* name, const char* password, unsigned uid, unsigned gid,
            const char* home)
{
    struct ipc_message q, a;
    unsigned at = 0;
    memset(&q, 0, sizeof(q));
    q.tag = AUTH_USERADD;
    q.word[0] = (long)uid;
    q.word[1] = (long)gid;
    at = pack(&q, at, name);
    at = pack(&q, at, password);
    at = pack(&q, at, home);
    q.bytes = at;
    return ask(&q, &a) == 0 && a.word[0] == 0 ? 0 : -1;
}

int passwd(const char* name, const char* old_password, const char* new_password)
{
    struct ipc_message q, a;
    unsigned at = 0;
    memset(&q, 0, sizeof(q));
    q.tag = AUTH_PASSWD;
    at = pack(&q, at, name);
    at = pack(&q, at, old_password);
    at = pack(&q, at, new_password);
    q.bytes = at;
    return ask(&q, &a) == 0 && a.word[0] == 0 ? 0 : -1;
}

int chmod(const char* path, unsigned mode)
{
    return (int)__syscall(SYS_chmod, (long)path, (long)mode, 0, 0, 0);
}

int chown(const char* path, unsigned uid, unsigned gid)
{
    return (int)__syscall(SYS_chown, (long)path, (long)uid, (long)gid, 0, 0);
}
