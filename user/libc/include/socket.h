#ifndef _SOCKET_H
#define _SOCKET_H

#include <stdint.h>

/* Sockets: the half of the network this system did not have.
 *
 * The stack could open a connection, carry it and close it, and could not be
 * the end that was connected *to*. There was no listen and no accept, so
 * nothing here could be a server - no sshd, no httpd, no inetd, and no way to
 * write one.
 *
 * A socket is a file descriptor, and that is the part that matters more than
 * the names. accept() hands back something that read(), write(), close(),
 * dup2() and a redirect all work on, which is what lets a server hand a
 * connection to an ordinary program as its standard input and output. A
 * connection that could only be reached through calls of its own would need
 * every program that spoke over one to be written for this system.
 *
 * The addresses are a host-order address and a port rather than a sockaddr.
 * There is no sockaddr anywhere else here, and inventing one so that these
 * five calls could take a pointer to it would be carrying a shape from another
 * system for the sake of the shape. If porting software ever needs one it is a
 * wrapper over these, not a change to them.
 *
 * What is deliberately missing: poll on a socket. The stack answers a question
 * at a time over IPC, so a read either has an answer or waits for one, and
 * there is nothing between the two for poll to report. A server built on
 * blocking accept and a process per connection works; one built on a poll loop
 * would spin. Saying so is better than a poll that always claims readiness.
 */

#define AF_INET      2
#define SOCK_STREAM  1

/* A socket that is not connected to anything yet. Only AF_INET and
 * SOCK_STREAM; anything else fails rather than pretending. */
int socket(int domain, int type, int protocol);

/* The port this socket answers on. An address of 0 means every address this
 * machine has, which is the only thing a single-homed machine can mean. */
int bind(int fd, uint32_t address, uint16_t port);

/* Start answering. `backlog` is how many connections may wait to be accepted;
 * it is a hint and small numbers are the honest ones here. */
int listen(int fd, int backlog);

/* The next connection, as a new descriptor. Waits when there is none, which is
 * what makes it the call a server sits in. `peer` and `peer_port` may be 0
 * when the caller does not care who it is. */
int accept(int fd, uint32_t* peer, uint16_t* peer_port);

/* The other direction, for completeness and because a client written against
 * these should not have to use a different call. */
int connect(int fd, uint32_t address, uint16_t port);

/* read() and write() do the same thing and are what most callers should use.
 * These exist because a caller that says send() means it. `flags` is accepted
 * and ignored: none of the ones POSIX defines apply to this stack. */
long send(int fd, const void* buffer, unsigned long bytes, int flags);
long recv(int fd, void* buffer, unsigned long bytes, int flags);

#endif /* _SOCKET_H */
