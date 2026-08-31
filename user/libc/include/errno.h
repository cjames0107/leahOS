#ifndef _ERRNO_H
#define _ERRNO_H

/* Why the last call failed.
 *
 * Until this existed everything returned -1 and nothing said why. "cannot
 * open" covered no such file, permission refused, it is a directory, and out
 * of descriptors, and several messages in this system had to be written to
 * cover all four at once because the information genuinely was not there.
 *
 * The numbers are the ones Linux uses. Nothing here has to interoperate with
 * anything, so the choice is free - and picking the set everybody already
 * knows means a number seen in a debugger is a number somebody can look up.
 *
 * errno is per thread. It has to be: two threads failing at the same time on
 * one global would each read the other's answer, and this system has threads.
 * There is no thread-local storage, so it is a small table keyed by tid - see
 * __errno_location.
 */

#define EPERM            1  /* not permitted - the operation, not the file */
#define ENOENT           2  /* no such file or directory */
#define ESRCH            3  /* no such process */
#define EINTR            4  /* interrupted by a signal */
#define EIO              5  /* the device said no */
#define ENXIO            6  /* no such device or address */
#define E2BIG            7  /* argument list too long */
#define ENOEXEC          8  /* not in a format that can be run */
#define EBADF            9  /* not an open descriptor */
#define ECHILD          10  /* no children to wait for */
#define EAGAIN          11  /* try again - nothing ready, and not blocking */
#define ENOMEM          12  /* out of memory */
#define EACCES          13  /* permission denied - this file, this way */
#define EFAULT          14  /* a pointer that is not the caller's to give */
#define EBUSY           16  /* in use */
#define EEXIST          17  /* it is already there */
#define EXDEV           18  /* across two filesystems, which a link cannot be */
#define ENODEV          19  /* no such device */
#define ENOTDIR         20  /* a component of the path is not a directory */
#define EISDIR          21  /* it is a directory, and this is not for those */
#define EINVAL          22  /* the arguments do not make sense */
#define ENFILE          23  /* the system's descriptor table is full */
#define EMFILE          24  /* this process's is */
#define ENOTTY          25  /* not a terminal */
#define EFBIG           27  /* file too large */
#define ENOSPC          28  /* no room left on the device */
#define ESPIPE          29  /* a pipe cannot be seeked */
#define EROFS           30  /* read-only filesystem */
#define EMLINK          31  /* too many links */
#define EPIPE           32  /* writing to a pipe nobody is reading */
#define EDOM            33  /* outside a function's domain - sqrt of -1 */
#define ERANGE          34  /* outside what the result can hold */
#define ENAMETOOLONG    36  /* the path is longer than anything here allows */
#define ENOSYS          38  /* not implemented */
#define ENOTEMPTY       39  /* a directory with things still in it */
#define ELOOP           40  /* too many symbolic links */
#define ETIMEDOUT      110  /* it took too long */
#define EADDRINUSE      98  /* that port is already claimed */
#define ECONNREFUSED   111  /* nothing listening there */
#define EHOSTUNREACH   113  /* no route */

/* The largest number a syscall may return as a negated error. A return more
 * negative than this is a real value - a pointer near the top of the address
 * space, say - and not a failure. Same convention as Linux, for the same
 * reason: one register has to carry both. */
#define ERRNO_MAX      256

/* The calling thread's errno. A function rather than a variable, because
 * which one it is depends on who is asking. */
int* __errno_location(void);

#define errno (*__errno_location())

/* A sentence for a number. Never null, and never a buffer the caller has to
 * free: unknown codes come back as "unknown error", which is more use than a
 * crash. */
const char* strerror(int code);

/* "prefix: reason" on standard error, which is what every UNIX tool prints and
 * why they all look the same. A null or empty prefix writes just the reason. */
void perror(const char* prefix);

#endif /* _ERRNO_H */
