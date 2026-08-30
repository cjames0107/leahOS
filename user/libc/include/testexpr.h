#ifndef _TESTEXPR_H
#define _TESTEXPR_H

/* The conditional expression that `test` evaluates.
 *
 * `test -f /etc/passwd`, `[ "$x" = yes ]`, `[ $n -gt 3 ]` - the thing every
 * shell script asks questions with, and the reason `if` is worth having at
 * all.
 *
 * Here rather than in the shell because there are two callers and one set of
 * rules: the shell runs it as a builtin, because a fork and an exec per
 * comparison inside a loop is most of what the loop costs, and /bin/test is
 * the same expression for anything that is not the shell. Two copies of
 * "what does -nt mean" would drift, and the one that drifted would be the one
 * nobody was looking at.
 *
 * `argv` is the arguments *after* the command name - so for `test -f x` it is
 * {"-f", "x"} and argc is 2. A trailing "]" is the caller's to strip.
 *
 * Returns 0 when the expression is true and 1 when it is false, which is the
 * shell's convention and the opposite of C's. 2 means the expression itself
 * was malformed, which is not the same as false: `[ -f ]` is a broken test,
 * not a file that is missing.
 */
int test_expr(int argc, char* const argv[]);

#endif /* _TESTEXPR_H */
