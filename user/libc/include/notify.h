#ifndef _NOTIFY_H
#define _NOTIFY_H

/* Saying something without opening a window.
 *
 * "The download finished", "the disk is nearly full", "that command has
 * ended" - things worth telling somebody about that are not worth interrupting
 * them for, and that a program with no window of its own has no other way to
 * say at all.
 *
 * Posting never blocks and never fails in a way a caller has to handle: the
 * messages go into a ring in the window server's control block, and if nothing
 * is showing them they are simply overwritten in turn. A program should be
 * able to mention something without first checking whether there is anybody to
 * mention it to.
 */

/* `from` is the application's own name, shown above the message - "From
 * Terminal". Both are copied; neither has to outlive the call. */
void notify(const char* from, const char* text);

#endif /* _NOTIFY_H */
