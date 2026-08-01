#ifndef _NETD_H
#define _NETD_H

/* The network stack, as seen from outside the process that runs it.
 *
 * Everything here is a question with an answer, which is why the port is
 * called with a request and not written to with a stream: resolve this
 * address, ping this host, tell me who I am.
 *
 * Some of those answers take a while to arrive - a ping is not answered until
 * something on the other side of the link answers it. The server takes the
 * request, remembers the handle, and goes back to draining the card; it
 * replies when the frame it was waiting for turns up, or when it gives up.
 * That is why reply is by handle rather than tied to the receive.
 */

#define NET_INFO    1   /* w0 = our IP, w1 = gateway, data = 6 MAC bytes    */
#define NET_RESOLVE 2   /* w0 = IP -> data = 6 MAC bytes, w0 = 0 on success */
#define NET_PING    3   /* w0 = IP, w1 = sequence -> w0 = TTL, -1 on timeout */
#define NET_STATS   4   /* w0 = frames in, w1 = frames out, w2 = arp entries */
#define NET_LOOKUP  5   /* data = a host name -> w0 = its address, -1 if none  */
#define NET_UDP_SEND 6  /* w0 = IP, w1 = port, w2 = source port, data = payload */
#define NET_UDP_RECV 7  /* w0 = source port to listen on -> data = a datagram   */

/* TCP. A connection is named by a number rather than a file descriptor: a
 * descriptor would mean the kernel knowing what a connection is, which is
 * exactly what moving the stack out was for. */
#define NET_TCP_CONNECT 8   /* w0 = IP, w1 = port -> w0 = connection, -1 if not */
#define NET_TCP_SEND    9   /* w0 = connection, data -> w0 = bytes taken        */
#define NET_TCP_RECV   10   /* w0 = connection -> data, w0 = length, 0 at end   */
#define NET_TCP_CLOSE  11   /* w0 = connection                                  */

/* IPv6. The addresses do not fit in a word, so they travel in data - which is
 * the first thing about v6 that is different and not the last. */
#define NET6_INFO   12  /* -> data = 16 bytes link-local + 16 global, w0 = have */
#define NET6_NEIGH  13  /* data = 16-byte address -> data = 6-byte MAC          */
#define NET6_PING   14  /* data = 16-byte address, w1 = seq -> w0 = hop limit   */

#endif
