#ifndef _AUTH_H
#define _AUTH_H

/* What the authentication server answers.
 *
 * This used to be four system calls, and the reason given for putting them in
 * the kernel was a real one: there is no setuid-on-exec here, so an ordinary
 * `su` cannot read /etc/shadow, and making the shadow file world readable to
 * let it would give away the only thing in there worth protecting. The kernel
 * had the privilege already, so authentication went in the kernel.
 *
 * A privileged server is the other answer to that problem, and it is the one
 * that fits a system built out of servers. authd runs as root, owns both
 * account files, and hands out answers rather than hashes - so the hash still
 * never reaches the process that asked, which was the whole point. What the
 * kernel keeps is the part only the kernel can do: changing a task's identity.
 */

#define AUTH_LOGIN   1  /* data: user\0password\0 -> word: uid, gid; data: home */
#define AUTH_USERADD 2  /* data: user\0password\0home\0, word: uid, gid */
#define AUTH_PASSWD  3  /* data: user\0old\0new\0 */
#define AUTH_UIDNAME 4  /* word[0]: uid -> data: name */
#define AUTH_EXISTS  5  /* data: user -> word[0]: 1 or 0 */

#endif
