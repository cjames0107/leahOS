#ifndef _UNISTD_H
#define _UNISTD_H

#include <stddef.h>

typedef long ssize_t;
typedef int  pid_t;

ssize_t write(int fd, const void* buffer, size_t count);
ssize_t read(int fd, void* buffer, size_t count);
pid_t   getpid(void);

/* Internal: fs.c hands the descriptor table to the next image, and gets its
 * own in order before a fork. Both have to happen on this side: the saved
 * table is keyed by the pid that saved it, and a child asks with the wrong
 * one. */
void    __fd_save_for_exec(void);
void    __fd_before_fork(void);
void    __fd_resolve(const char* path, char* out);

pid_t fork(void);
int   execve(const char* path, char* const argv[], char* const envp[]);

/* Reap any child. waitpid(-1, status, 0), and always was - see <sys/wait.h>
 * for what the status word means, and for waiting on one job in particular. */
pid_t wait(int* status);

/* --- process groups and sessions -------------------------------------------
 *
 * A process group is a job. Everything in `a | b | c` is put in one group, so
 * one Ctrl-C reaches all three and one wait covers all three - which is the
 * whole reason the concept exists, and why a shell is the main thing that ever
 * calls these. A session is a login: a set of groups sharing one terminal.
 *
 * Both are inherited across fork and kept across execve, so a shell can put a
 * child where it belongs before the child has run any of its own code.
 */

/* Move a process into a group. pid 0 means the caller; pgid 0 means a group of
 * its own, named after it. A shell calls this from both sides of a fork -
 * whichever runs first wins, and the child has to be in the group before
 * anything is sent to it. It is idempotent for exactly that reason. */
int   setpgid(pid_t pid, pid_t pgid);
pid_t getpgid(pid_t pid);       /* pid 0 means the caller */
pid_t getpgrp(void);            /* getpgid(0) */

/* Start a new session, in a new group, with no controlling terminal. Fails if
 * the caller already leads a process group, because the new session would have
 * to take that group with it. */
pid_t setsid(void);
pid_t getsid(pid_t pid);

/* Which process group the terminal on `fd` is currently listening to. Only the
 * foreground group may be sent what the keyboard generates; everything else is
 * a background job.
 *
 * There is no terminal driver to hold this - a terminal here is a program at
 * the far end of a pipe - so it lives in a small piece of shared memory that
 * the terminal creates and everything it starts inherits, the same way the
 * controlling terminal's descriptor does. */
pid_t tcgetpgrp(int fd);
int   tcsetpgrp(int fd, pid_t pgid);
void  yield(void);

/* Block for at least `ms` milliseconds. Prefer this to spinning on yield() in a
 * polling loop: a task that only yields stays runnable and keeps taking the
 * kernel lock, which on a multiprocessor can starve another CPU out of it. */
void  msleep(unsigned long ms);

int    close(int fd);
long   lseek(int fd, long offset, int whence);

/* The lowest free descriptor onto the same thing. */
int    dup(int oldfd);

/* The controlling terminal: which descriptor is the terminal this process is
 * attached to, or -1. Not the same as standard input, which redirection moves
 * - `something | less` is the case where the difference matters, and it is
 * what makes /dev/tty work. Set by whoever creates a terminal; inherited from
 * there through fork and execve. */
int    tty_fd(void);
void   tty_set(int fd);

/* Whether that descriptor is the terminal. The question a program should ask
 * before deciding to page, colour or prompt: `man ls` and `man ls | head` are
 * both run by a process with a terminal, and only the first should page. */
int    isatty(int fd);

/* --- pseudo-terminals -------------------------------------------------------
 *
 * A pty is a pair: a master, which a terminal program holds, and a slave,
 * which is an ordinary terminal to whatever is run on it. What is typed into
 * the master arrives at the slave through the line discipline - lines
 * assembled, keys echoed back, the interrupt keys turned into signals to the
 * foreground process group - and what the program prints comes back the other
 * way untouched.
 *
 * This is what lets a program be run under a terminal without knowing it is:
 * isatty says yes because the descriptor is one, not because anybody said so.
 */
int  pty_open(int* index);          /* the master, and which pair it is */
int  pty_slave(int index);          /* the other end of that pair */
void ptsname(int index, char* out, int max);

/* How big the terminal says it is. Asking the descriptor is the answer that
 * stays true: COLUMNS and LINES in the environment are a copy taken when the
 * program started and are wrong the moment a window is resized. */
int  tty_size(int fd, unsigned* rows, unsigned* columns);
int  tty_set_size(int fd, unsigned rows, unsigned columns);

/* The terminal's control block - where the foreground process group lives, for
 * want of a tty driver to keep it in. A terminal calls tty_control_create
 * before starting its shell; everything below inherits the key. */
unsigned tty_control_key(void);
void     tty_set_control(unsigned key);
unsigned tty_control_create(void);
int    chdir(const char* path);
int    getcwd(char* buffer, size_t size);
int    unlink(const char* path);
int    pipe(int fds[2]);
int    dup2(int oldfd, int newfd);
int    rename(const char* oldpath, const char* newpath);
void*  sbrk(long increment);

/* Credentials. uid 0 is root; only root may change them. */
unsigned getuid(void);
unsigned getgid(void);
int      setuid(unsigned uid);
int      setgid(unsigned gid);

/* Authenticate as `user` and, on success, become them. The password is checked
 * inside the kernel against a shadow file no user process can read, so this
 * needs no setuid bit. Pass a null password as root, which is not asked for one.
 * `home` receives the account's home directory. Returns 0, or -1. */
int login(const char* user, const char* password, char* home);

/* Console echo, off while a password is being typed. */
void setecho(int on);

/* The account name for a uid. Returns 0, or -1 when there is no such account. */
int username(unsigned uid, char* name_out);

/* Create an account. Root only. The kernel hashes the password and creates the
 * home directory; nothing here ever sees a digest. Returns 0, or -1. */
int useradd(const char* name, const char* password, unsigned uid, unsigned gid,
            const char* home);

/* Change a password. Root may change anyone's and may pass a null `old`;
 * anyone else may change only their own and must prove it. Returns 0, or -1. */
int passwd(const char* name, const char* old_password, const char* new_password);

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2


/* Write out everything the filesystem is holding back.
 *
 * Metadata changes are batched into a journal transaction and committed a
 * little later, so a call that returned successfully is in the tree but is
 * not necessarily on the disk yet. This is what makes it so. */
void sync(void);

#endif /* _UNISTD_H */
