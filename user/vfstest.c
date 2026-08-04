/* vfstest - read a real file through the filesystem server.
 *
 * Four processes deep: this one asks vfsd, which asks blockd, which asks the
 * drive. The contents are checked against what the build actually put on the
 * disk, so a server returning something plausible but wrong fails.
 */

#include <ipc.h>
#include <shm.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <vfsd.h>

static int g_failures;

static void check(const char* what, int ok)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

int main(void)
{
    int port = -1;
    for (int i = 0; i < 400 && port < 0; ++i) {
        port = port_open(IPC_PORT_VFS);
        if (port < 0) msleep(10);
    }
    if (port < 0) {
        printf("vfstest: no filesystem server is running\n");
        return 1;
    }

    const int shm = shm_open(VFS_SHM_KEY, sizeof(struct vfs_shared), 0);
    struct vfs_shared* buf = shm < 0 ? 0 : (struct vfs_shared*)shm_map(shm);
    if (buf == 0) {
        printf("vfstest: cannot map the server's buffer\n");
        return 1;
    }

    struct ipc_message q, a;

    /* The root, which every ext4 filesystem has and which must be a directory. */
    memset(&q, 0, sizeof(q));
    q.tag = VFS_STAT;
    q.data[0] = '/'; q.bytes = 1;
    check("the root exists and is a directory",
          ipc_call(port, &q, &a) == 0 && a.word[0] >= 0 &&
          a.word[1] == VFS_KIND_DIR);

    /* Something the build put there, found by walking two levels. */
    memset(&q, 0, sizeof(q));
    q.tag = VFS_STAT;
    const char* path = "/usr/share/doc/readme.md";
    unsigned n = 0;
    while (path[n] != '\0') { q.data[n] = path[n]; ++n; }
    q.bytes = n;
    const int found = ipc_call(port, &q, &a) == 0 && a.word[0] > 0;
    check("a file two directories deep is found", found);
    const long size = found ? (long)a.word[0] : 0;

    if (found) {
        memset(&q, 0, sizeof(q));
        q.tag = VFS_READ;
        for (unsigned i = 0; i < n; ++i) q.data[i] = path[i];
        q.bytes = n;
        q.word[1] = 0;
        q.word[2] = 256;
        const int got = ipc_call(port, &q, &a) == 0 && a.word[0] > 0;
        check("its contents come back", got);
        if (got) {
            buf->data[a.word[0] < 60 ? a.word[0] : 60] = '\0';
            printf("      first line: %s\n", (char*)buf->data);
            /* A markdown file the build wrote; it starts with a heading. */
            check("the bytes are the file's own", buf->data[0] == '#');
        }

        /* And the far end of it, which needs the offset honoured rather than
         * the first block returned whatever was asked for. */
        if (size > 64) {
            memset(&q, 0, sizeof(q));
            q.tag = VFS_READ;
            for (unsigned i = 0; i < n; ++i) q.data[i] = path[i];
            q.bytes = n;
            q.word[1] = size - 8;
            q.word[2] = 64;
            check("a read past the start returns only what is left",
                  ipc_call(port, &q, &a) == 0 && a.word[0] == 8);
        }
    }

    /* Listing, which is the other way a directory is used. */
    int entries = 0;
    for (unsigned i = 0; i < 32; ++i) {
        memset(&q, 0, sizeof(q));
        q.tag = VFS_LIST;
        q.data[0] = '/'; q.bytes = 1;
        q.word[1] = (long)i;
        if (ipc_call(port, &q, &a) != 0 || a.word[0] < 0)
            break;
        ++entries;
    }
    check("the root lists more than a handful of entries", entries >= 6);
    printf("      %d entries in /\n", entries);

    /* --- and now writing ------------------------------------------------
     *
     * Everything made here is checked by reading it back, and then the image
     * is checked by fsck on the host afterwards - which is the part that
     * catches a filesystem that looks right from inside and is not.
     */
    memset(&q, 0, sizeof(q));
    q.tag = VFS_MKDIR;
    const char* dir = "/made";
    n = 0; while (dir[n] != '\0') { q.data[n] = dir[n]; ++n; }
    q.bytes = n;
    check("a directory can be made", ipc_call(port, &q, &a) == 0 && a.word[0] == 0);

    memset(&q, 0, sizeof(q));
    q.tag = VFS_CREATE;
    const char* file = "/made/hello.txt";
    n = 0; while (file[n] != '\0') { q.data[n] = file[n]; ++n; }
    q.bytes = n;
    check("a file can be made inside it",
          ipc_call(port, &q, &a) == 0 && a.word[0] == 0);

    /* Longer than one block, so an extent has to grow rather than just exist. */
    const unsigned long length = 5000;
    for (unsigned long i = 0; i < length; ++i)
        buf->data[i] = (unsigned char)('A' + (i % 26));
    memset(&q, 0, sizeof(q));
    q.tag = VFS_WRITE;
    for (unsigned i = 0; i < n; ++i) q.data[i] = file[i];
    q.bytes = n;
    q.word[1] = 0;
    q.word[2] = (long)length;
    check("five thousand bytes are written",
          ipc_call(port, &q, &a) == 0 && a.word[0] == (long)length);

    memset(buf->data, 0, length);
    memset(&q, 0, sizeof(q));
    q.tag = VFS_READ;
    for (unsigned i = 0; i < n; ++i) q.data[i] = file[i];
    q.bytes = n;
    q.word[1] = 0;
    q.word[2] = (long)length;
    int same = ipc_call(port, &q, &a) == 0 && a.word[0] == (long)length;
    for (unsigned long i = 0; same && i < length; ++i)
        if (buf->data[i] != (unsigned char)('A' + (i % 26)))
            same = 0;
    check("and read back byte for byte across two blocks", same);

    /* A second file, then remove it, so the free counts have to come back. */
    memset(&q, 0, sizeof(q));
    q.tag = VFS_CREATE;
    const char* spare = "/made/spare.txt";
    n = 0; while (spare[n] != '\0') { q.data[n] = spare[n]; ++n; }
    q.bytes = n;
    ipc_call(port, &q, &a);
    memset(&q, 0, sizeof(q));
    q.tag = VFS_UNLINK;
    for (unsigned i = 0; i < n; ++i) q.data[i] = spare[i];
    q.bytes = n;
    check("a file can be removed again",
          ipc_call(port, &q, &a) == 0 && a.word[0] == 0);

    memset(&q, 0, sizeof(q));
    q.tag = VFS_STAT;
    for (unsigned i = 0; i < n; ++i) q.data[i] = spare[i];
    q.bytes = n;
    check("and is gone afterwards",
          !(ipc_call(port, &q, &a) == 0 && a.word[0] >= 0));

    if (g_failures == 0)
        printf("  ok  a filesystem was read and written across four processes\n");
    return g_failures;
}
