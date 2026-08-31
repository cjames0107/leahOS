/* See <object.h>. Thin wrappers; the table lives in the kernel, which is the
 * entire point - a table this side of the boundary would be one the process
 * could write. */

#include <object.h>
#include <sys/syscall.h>

int obj_close(int handle)
{
    return (int)__syscall(SYS_handleclose, handle, 0, 0, 0, 0);
}

int obj_duplicate(int handle, unsigned mask)
{
    return (int)__syscall(SYS_handledup, handle, (long)mask, 0, 0, 0);
}

unsigned obj_rights(int handle)
{
    const long info = __syscall(SYS_handleinfo, handle, 0, 0, 0, 0);
    return info < 0 ? 0u : (unsigned)((unsigned long)info >> 32);
}
