#ifndef _POLL_H
#define _POLL_H

/* Waiting on several descriptors at once.
 *
 * The thing a program needs when it has more than one thing to listen to and
 * no way to know which will speak first. Without it the only options are a
 * thread per descriptor - which is what the terminal does, and why it has a
 * reader thread - or a spin with a sleep in it, which is either slow to
 * respond or busy.
 *
 * Only pipes and the keyboard can actually make a caller wait. A file on a
 * disk is always ready, and libc answers for those itself rather than asking
 * the kernel about something it would only say yes to.
 */

struct pollfd {
    int   fd;
    short events;       /* what to watch for */
    short revents;      /* what happened; written by poll */
};

#define POLLIN   0x001  /* a read would not block */
#define POLLOUT  0x004  /* a write would not block */
#define POLLERR  0x008
#define POLLHUP  0x010  /* the other end has gone */
#define POLLNVAL 0x020  /* not an open descriptor */

/* Returns how many entries have a non-zero revents, 0 if the time ran out, or
 * -1. A negative timeout waits indefinitely; zero returns at once, which is
 * how to ask "is anything ready" without waiting at all.
 *
 * POLLERR, POLLHUP and POLLNVAL come back whether or not they were asked for.
 * A caller waiting to read a pipe whose writer has gone needs to hear about
 * it, and would not have thought to ask.
 */
int poll(struct pollfd* fds, unsigned long count, int timeout_ms);

#endif /* _POLL_H */
