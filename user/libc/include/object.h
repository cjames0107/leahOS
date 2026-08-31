#ifndef _OBJECT_H
#define _OBJECT_H

/* Capabilities, from the userland side.
 *
 * A handle is a small integer that names a kernel object, and it means nothing
 * outside the process holding it - the kernel resolves one in the sender's
 * table and installs what it names in the receiver's, so passing a handle over
 * IPC passes the authority and not the number. Nobody can pass a right they do
 * not hold, which is what makes it safe to hand one to something less trusted.
 *
 * These values mirror object::Rights in <leah/object.hpp> and must agree with
 * it.
 */

#define OBJ_READ      (1u << 0)
#define OBJ_WRITE     (1u << 1)
#define OBJ_EXECUTE   (1u << 2)   /* may be the program of an exec */
#define OBJ_MAP       (1u << 3)
#define OBJ_SIGNAL    (1u << 4)
#define OBJ_WAIT      (1u << 5)
#define OBJ_DUPLICATE (1u << 6)   /* may be copied at all */
#define OBJ_TRANSFER  (1u << 7)   /* may be sent to another process */
#define OBJ_DESTROY   (1u << 8)

/* Give one up. */
int obj_close(int handle);

/* A second handle on the same object, with rights narrowed by `mask`. Never
 * widened: a mask asking for a right the original does not carry simply does
 * not get it. Returns -1 if the original may not be duplicated. */
int obj_duplicate(int handle, unsigned mask);

/* What this handle permits, or 0 if it names nothing. */
unsigned obj_rights(int handle);

#endif /* _OBJECT_H */
