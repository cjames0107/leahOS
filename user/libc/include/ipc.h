#ifndef _IPC_H
#define _IPC_H

#include <stdint.h>

/* Talking to a server that is another process.
 *
 * The kernel copies the message from one address space into the other; neither
 * side can see the other's memory, and neither has to know where the other is.
 * Bulk data does not travel here - put it in a shared segment and send the key.
 */

#define IPC_INLINE 256

struct ipc_message {
    uint32_t tag;               /* what this is; the server defines the set   */
    uint32_t bytes;             /* how much of data means anything            */
    int64_t  word[4];           /* small arguments, and small answers         */
    int32_t  shm_key;           /* bulk payload, or 0                         */
    uint32_t shm_bytes;
    char     data[IPC_INLINE];
};

/* Well-known ports, mirrored from <leah/ipc.hpp>. */
#define IPC_PORT_NET   1
#define IPC_PORT_VFS   2
#define IPC_PORT_BLOCK 3
#define IPC_PORT_NIC   4
#define IPC_PORT_AUDIO 5

/* Server side. */
int port_create(unsigned name);
int port_destroy(int port);
/* Blocks until a request arrives. Returns a handle to answer with, or -1. */
int ipc_recv(int port, struct ipc_message* out, unsigned* caller_pid);
/* The same without blocking: -1 when nothing is waiting. A server that also
 * owns hardware has a card to drain as well as a port to answer. */
int ipc_try_recv(int port, struct ipc_message* out, unsigned* caller_pid);
int ipc_reply(int handle, const struct ipc_message* msg);

/* Client side. port_open returns -1 when nobody has claimed the name yet. */
int port_open(unsigned name);
/* Sends and blocks until the answer arrives. -1 if the server died first. */
int ipc_call(int port, const struct ipc_message* request,
             struct ipc_message* reply);

#endif
