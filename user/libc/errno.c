/* errno, and the words for it.
 *
 * Per thread, because two threads failing at once on one global would each
 * read the other's answer. There is no thread-local storage here - no %fs base
 * per thread, no __thread - so it is a small table keyed by thread id.
 *
 * Linear search over sixteen entries sounds slow and is not: it runs on the
 * failure path, once, and sixteen comparisons is nothing against the syscall
 * that just failed. A process with more than sixteen live threads shares the
 * last slot, which is wrong in the same way one global would be but only for
 * the seventeenth thread onwards.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <thread.h>
#include <unistd.h>

#define SLOTS 16

static struct {
    int tid;
    int value;
} g_slots[SLOTS];

static int g_used;
static int g_overflow;          /* the shared slot, for thread seventeen on */

int* __errno_location(void)
{
    const int self = gettid();
    for (int i = 0; i < g_used; ++i)
        if (g_slots[i].tid == self)
            return &g_slots[i].value;
    if (g_used < SLOTS) {
        g_slots[g_used].tid = self;
        g_slots[g_used].value = 0;
        return &g_slots[g_used++].value;
    }
    return &g_overflow;
}

/* Short, lower case, no full stop - so that "open: %s" reads as a sentence and
 * two of them can be joined without looking odd. */
static const struct { int code; const char* text; } kTexts[] = {
    { 0,              "no error" },
    { EPERM,          "operation not permitted" },
    { ENOENT,         "no such file or directory" },
    { ESRCH,          "no such process" },
    { EINTR,          "interrupted" },
    { EIO,            "input/output error" },
    { ENXIO,          "no such device or address" },
    { E2BIG,          "argument list too long" },
    { ENOEXEC,        "not an executable" },
    { EBADF,          "bad file descriptor" },
    { ECHILD,         "no child processes" },
    { EAGAIN,         "nothing ready, try again" },
    { ENOMEM,         "out of memory" },
    { EACCES,         "permission denied" },
    { EFAULT,         "bad address" },
    { EBUSY,          "device or resource busy" },
    { EEXIST,         "file exists" },
    { EXDEV,          "cross-device link" },
    { ENODEV,         "no such device" },
    { ENOTDIR,        "not a directory" },
    { EISDIR,         "is a directory" },
    { EINVAL,         "invalid argument" },
    { ENFILE,         "too many open files in the system" },
    { EMFILE,         "too many open files" },
    { ENOTTY,         "not a terminal" },
    { EFBIG,          "file too large" },
    { ENOSPC,         "no space left on device" },
    { ESPIPE,         "illegal seek" },
    { EROFS,          "read-only filesystem" },
    { EMLINK,         "too many links" },
    { EPIPE,          "broken pipe" },
    { EDOM,           "argument out of domain" },
    { ERANGE,         "result out of range" },
    { ENAMETOOLONG,   "file name too long" },
    { ENOSYS,         "not implemented" },
    { ENOTEMPTY,      "directory not empty" },
    { ELOOP,          "too many levels of symbolic links" },
    { ETIMEDOUT,      "timed out" },
    { ECONNREFUSED,   "connection refused" },
    { EHOSTUNREACH,   "no route to host" },
};

const char* strerror(int code)
{
    for (unsigned i = 0; i < sizeof(kTexts) / sizeof(kTexts[0]); ++i)
        if (kTexts[i].code == code)
            return kTexts[i].text;
    return "unknown error";
}

void perror(const char* prefix)
{
    const char* reason = strerror(errno);
    if (prefix != 0 && prefix[0] != '\0')
        fprintf(stderr, "%s: %s\n", prefix, reason);
    else
        fprintf(stderr, "%s\n", reason);
}
