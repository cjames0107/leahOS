#ifndef _TERMIOS_H
#define _TERMIOS_H

/* The terminal's line settings.
 *
 * What a program changes when it wants keys as they are pressed rather than a
 * line at a time - an editor, a pager, anything that responds to j and k
 * without waiting for return.
 *
 * There is no terminal driver here to hold these. A terminal in this system is
 * an ordinary program at the far end of a pipe, and it is the one doing the
 * line discipline: assembling lines, echoing keys, applying backspace, turning
 * Ctrl-C into a signal. So the settings live where it can see them - the same
 * page of shared memory that carries the foreground process group - and it
 * consults them on every key.
 *
 * The flags are the POSIX ones by name and meaning. The ones that describe
 * baud rates, parity and flow control are not here: there is no serial line
 * under this, and a field nothing reads is a promise nothing keeps.
 */

/* c_lflag - the local modes, which are the ones that matter here. */
#define ISIG   0x0001   /* Ctrl-C and friends become signals */
#define ICANON 0x0002   /* assemble lines; a read returns at the newline */
#define ECHO   0x0008   /* show what was typed */

/* c_cc indices. */
#define VINTR   0       /* what sends SIGINT   - Ctrl-C */
#define VQUIT   1       /* what sends SIGQUIT  - Ctrl-\ */
#define VERASE  2       /* backspace */
#define VSUSP   3       /* what sends SIGTSTP  - Ctrl-Z */
#define VMIN    4       /* fewest bytes a raw read may return */
#define VTIME   5       /* tenths of a second a raw read may wait */
#define NCCS    8

struct termios {
    unsigned      c_iflag;      /* accepted and unused; see above */
    unsigned      c_oflag;
    unsigned      c_cflag;
    unsigned      c_lflag;
    unsigned char c_cc[NCCS];
};

/* tcsetattr's `when`. All three are accepted; there is nothing buffered on
 * this side to drain or flush, so they do the same thing. */
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

int tcgetattr(int fd, struct termios* out);
int tcsetattr(int fd, int when, const struct termios* in);

/* Turn a settings block into "give me every key as it arrives, echo nothing,
 * and do not turn any of them into signals". The one combination almost every
 * caller of tcsetattr actually wants, spelled once. */
void cfmakeraw(struct termios* t);

#endif /* _TERMIOS_H */
