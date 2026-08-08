#ifndef _REGEX_H
#define _REGEX_H

/* Regular expressions.
 *
 * grep searched for a literal substring, and said so in its manual, because a
 * half-built regex engine that silently mishandles a pattern is worse than a
 * substring search that is honest. This is the whole thing instead.
 *
 * Parsed to a tree and matched by backtracking, rather than compiled to an
 * automaton. A backtracker is what makes `(a|b)*c` easy to write and
 * `(a*)*b` slow to fail, and the second is a real weakness - the guard against
 * it is a step limit rather than a cleverer engine, because a cleverer engine
 * is a much larger program and nothing here searches adversarial input.
 *
 * What is understood:
 *
 *   .           any one character
 *   [abc] [^a]  a set, with a-z ranges and a leading ^ to invert
 *   * + ?       none or more, one or more, none or one
 *   {n} {n,} {n,m}   exactly, at least, between
 *   |           either side
 *   ( )         grouping
 *   ^ $         the start and the end of the line
 *   \           the next character, literally
 *   \d \w \s    a digit, a word character, whitespace - and \D \W \S for not
 *
 * What is not: backreferences, which need the engine to remember what a group
 * matched and are the feature that makes a regex language stop being regular.
 */

struct regex;

/* Compile. Returns null and fills `error` (when not null) with something a
 * person can read - "unmatched (" rather than a number. */
struct regex* regex_compile(const char* pattern, int ignore_case,
                            const char** error);

void regex_free(struct regex* re);

/* Whether the text contains a match anywhere. `start` and `end`, when not
 * null, receive the offsets of the leftmost one - which is what a highlighter
 * or a substitution needs and a plain yes/no does not. */
int regex_search(struct regex* re, const char* text,
                 int* start, int* end);

#endif /* _REGEX_H */
