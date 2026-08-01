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

#endif
