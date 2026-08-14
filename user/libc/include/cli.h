#ifndef _CLI_H
#define _CLI_H

#include <stddef.h>

/* The things a command-line program here writes every time.
 *
 * Counted rather than guessed at: thirty-three programs print their own usage
 * text, eighteen open a file and read it whole by hand, nine walk a directory
 * tree, and most of them format an error as "name: something went wrong" with
 * the name typed out again as a literal. None of that is what any of those
 * programs is about.
 *
 * The pattern this replaces, from head.c, is representative:
 *
 *     if (i < argc && argv[i][0] == '-' && argv[i][1] == 'n') {
 *         if (argv[i][2] != '\0') { want = atoi_simple(&argv[i][2]); ++i; }
 *         else if (i + 1 < argc)  { want = atoi_simple(argv[i + 1]); i += 2; }
 *     }
 *
 * - which is correct, and is written again in the next program, and is where
 * the difference between "-n10" and "-n 10" is decided one program at a time.
 */

/* Start up: remember the program's name for messages, and the usage line for
 * when it is asked for or needed. `usage` may be 0.
 *
 * The name comes from argv[0] rather than a literal, so a program that is
 * renamed says its new name. */
void cli_begin(int argc, char** argv, const char* usage);

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
 * Nine programs walk directories. The tricky parts are the same every time and
 * are decided here: how deep to go, that "." and ".." are not entries to
 * recurse into, and that a bundle is a document rather than a folder to
 * descend into.
 *
 * The callback gets the full path and whether it is a directory, and returns 0
 * to carry on or non-zero to stop the whole walk - which is what makes "find
 * the first one" as cheap as it should be.
 */
typedef int (*cli_walk_fn)(const char* path, int is_dir, void* user);

int cli_walk(const char* root, int max_depth, cli_walk_fn fn, void* user);

/* --- saying sizes ---------------------------------------------------------- */

/* "4.0 KiB", "12 MiB". Bytes below a kilobyte are printed as bytes, because
 * rounding 900 to "0.9 KiB" tells you less than the number did. */
void cli_human(unsigned long long bytes, char* out, unsigned long max);

#endif
