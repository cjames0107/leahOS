#ifndef _CLI_H
#define _CLI_H

#include <stddef.h>

/* The things a command-line program here writes every time.
 *
 * Counted rather than guessed at, before any of it was moved here:
 * thirty-three programs printed their own usage text, eighteen opened a file
 * and read it whole by hand, nine walked a directory tree, and most of them
 * formatted an error as "name: something went wrong" with the name typed out
 * again as a literal. None of that was what any of those programs was about.
 *
 * The pattern this replaced, from head.c, is representative:
 *
 *     if (i < argc && argv[i][0] == '-' && argv[i][1] == 'n') {
 *         if (argv[i][2] != '\0') { want = atoi_simple(&argv[i][2]); ++i; }
 *         else if (i + 1 < argc)  { want = atoi_simple(argv[i + 1]); i += 2; }
 *     }
 *
 * - which is correct, and was written again in the next program, and is where
 * the difference between "-n10" and "-n 10" was decided one program at a
 * time. Every command-line program under user/ now begins with cli_begin, so
 * that decision is made once, here.
 *
 * A few of them pass 0 for the options and keep their own argument handling:
 * sh, env, echo, printf, kill and find, whose arguments are not letters after
 * a dash - a signal, a format, a command to run with its own options, a test
 * spelled as a word. They still take their name and their usage line from
 * here, which is the part that was being written out by hand.
 */

/* Start up: remember the program's name for messages, the usage line for when
 * it is asked for or needed, and which options this program understands.
 *
 * The name comes from argv[0] rather than a literal, so a program that is
 * renamed says its new name.
 *
 * `options` is the letters, with a colon after any that take a value: "n:v"
 * means -n takes one and -v does not. Two things follow from saying so:
 *
 *  - the value is not mistaken for a filename. Without it "head -n 10 x"
 *    leaves "10" among the positional arguments, and head opens a file called
 *    10. The separated form cannot be told from a flag followed by a filename
 *    by looking at it, so the program has to say.
 *
 *  - an option the program does not understand is refused, once, here, rather
 *    than being silently treated as a filename by some programs and reported
 *    by others in their own words.
 *
 * Declaring options also answers --help with the usage line, so every program
 * that has options has that without writing it.
 *
 * Pass 0 when the arguments are not option-shaped at all - `echo -n`, `kill
 * -9`, `printf '%s'` - and nothing is parsed, refused, or answered, because
 * those programs must pass `--help` through as the word it is. `usage` may
 * be 0. */
void cli_begin(int argc, char** argv, const char* usage, const char* options);

/* "name: ..." on stderr. cli_fail returns -1 so it can be the whole of an
 * error path; cli_die does not return. */
int  cli_fail(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void cli_die(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

/* Print the usage line and exit 1. */
void cli_usage(void);

/* --- options ---------------------------------------------------------------
 *
 * Options are read out of the argument list and removed from it, so what is
 * left is the positional arguments in order. That is the part every program
 * got slightly differently: some stopped at the first non-option, some did
 * not, and a few silently treated a stray "-x" as a filename.
 */

/* Was this flag given? "-a", and also inside a run like "-la". */
int cli_flag(const char* name);

/* An option with a value: "-n 10" and "-n10" both. Returns `fallback` when it
 * was not given. */
const char* cli_value(const char* name, const char* fallback);
long        cli_number(const char* name, long fallback);

/* A count that may also be written as the bare -NUMBER head and tail have
 * always taken: `head -1` and `head -n 1` are the same request. Declare it by
 * putting a '#' in the options string, which is what makes -5 a known option
 * rather than an unknown one. */
long        cli_count(const char* name, long fallback);

/* What is left after the options: the files, the paths, the words. `index` is
 * from 0. cli_arg returns 0 past the end. */
int         cli_argc(void);
const char* cli_arg(int index);

/* --- files -----------------------------------------------------------------
 *
 * Reading a file whole is eighteen copies of open/read-until-zero/close, each
 * with its own buffer size and its own idea of what to do when it does not
 * fit.
 */

/* The whole file, into a buffer the caller owns. Returns the length, or -1;
 * `out` is always terminated so it can be treated as a string. The file is
 * read in full or not at all - a truncated half of a document is worse than a
 * failure, because nothing downstream can tell. */
long cli_read_file(const char* path, char* out, unsigned long max);

/* Write a whole file, creating or replacing it. Returns 0, or -1. */
int cli_write_file(const char* path, const void* data, unsigned long bytes);

/* --- walking a tree --------------------------------------------------------
 *
 * Nine programs walked directories. The tricky parts are the same every time
 * and are decided here: how deep to go, that "." and ".." are not entries to
 * recurse into, and that a name beginning with a dot is an ordinary file that
 * is walked like any other.
 *
 * The callback gets the full path and the entry's type - one of the S_IF*
 * values, so a caller can tell a regular file from a device or a fifo and not
 * only from a directory - and returns 0 to carry on or non-zero to stop the
 * whole walk, which is what makes "find the first one" as cheap as it should
 * be.
 *
 * The root itself is not passed to the callback; what is walked is what is
 * inside it. A caller that has something to say about the root says it before
 * calling, where it has the root's own name to hand.
 */
typedef int (*cli_walk_fn)(const char* path, unsigned type, void* user);

int cli_walk(const char* root, int max_depth, cli_walk_fn fn, void* user);

/* --- saying sizes ---------------------------------------------------------- */

/* "4.0 KiB", "12 MiB". Bytes below a kilobyte are printed as bytes, because
 * rounding 900 to "0.9 KiB" tells you less than the number did. */
void cli_human(unsigned long long bytes, char* out, unsigned long max);

#endif
