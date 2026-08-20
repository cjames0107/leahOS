#ifndef _RTF_H
#define _RTF_H

/* Rich text: a document with styled characters, read from and written to RTF.
 *
 * The model is deliberately flat. A document is a string and a style byte per
 * character, rather than a tree of runs with a tree of paragraphs over it. A
 * tree is the right shape for a word processor that has to merge, split and
 * balance runs on every keystroke; for an editor of this size it is a great
 * deal of machinery whose only purpose is to avoid storing one byte per
 * character, and one byte per character is nothing.
 *
 * It is also exactly what RTF's own model is. RTF has no runs: it has a
 * current state and control words that change it, and a run is whatever
 * happened between two of them. Reading is therefore "walk the file, keep the
 * state, append characters", and writing is "walk the characters, emit a
 * control word whenever the state differs from the last one" - both of which
 * are a page of code against this representation and considerably more against
 * any other.
 *
 * What is deliberately not here: tables, images, colours, embedded objects,
 * and every other destination RTF can carry. Unknown destinations are skipped
 * whole, which is what the specification asks a reader to do with them, so a
 * file from elsewhere loses what this cannot show rather than being refused.
 */

/* The style of one character. Flags in the low bits, the size as an index into
 * rtf_size_points in the high ones - so a style is one byte and comparing two
 * of them is comparing two bytes. */
#define RTF_BOLD       0x01u
#define RTF_ITALIC     0x02u
#define RTF_UNDERLINE  0x04u
#define RTF_FLAGS      0x07u
#define RTF_SIZE_SHIFT 3
#define RTF_SIZE_MASK  0x38u        /* three bits: eight sizes */

#define RTF_SIZES 8
extern const int rtf_size_points[RTF_SIZES];

/* The size index a document starts at, and the point size it means. */
#define RTF_SIZE_DEFAULT 2          /* rtf_size_points[2] is 12 point */

static inline unsigned rtf_style_size(unsigned char style)
{
    return ((unsigned)style & RTF_SIZE_MASK) >> RTF_SIZE_SHIFT;
}
static inline unsigned char rtf_style_with_size(unsigned char style, unsigned i)
{
    return (unsigned char)((style & ~RTF_SIZE_MASK) |
                           ((i << RTF_SIZE_SHIFT) & RTF_SIZE_MASK));
}

/* Paragraph alignment. One per document rather than one per paragraph: this
 * editor has no paragraph model, and pretending otherwise would mean a second
 * flat array whose only entries are at the newlines. */
#define RTF_LEFT   0
#define RTF_CENTRE 1
#define RTF_RIGHT  2

struct rtf_doc {
    char*          text;    /* the characters, with '\n' between paragraphs */
    unsigned char* style;   /* one per character, in step with text         */
    long           len;
    long           cap;
    int            align;
};

/* An empty document, or 0 if there is no room for one. */
struct rtf_doc* rtf_new(void);
void            rtf_free(struct rtf_doc* d);

/* Read a file. Returns 0 and leaves errno if it cannot be read; a file that is
 * not RTF at all is taken as plain text, because the alternative is refusing
 * to open something the person plainly meant to open. */
struct rtf_doc* rtf_read(const char* path);

/* Write one. 0 on success, -1 with errno set. */
int rtf_write(const char* path, const struct rtf_doc* d);

/* Editing. `at` is a character index; inserting at len appends. Both return 0
 * on success and -1 if there was no room. */
int rtf_insert(struct rtf_doc* d, long at, const char* s, long n,
               unsigned char style);
int rtf_delete(struct rtf_doc* d, long at, long n);

/* Turn `flags` on or off across a range, leaving everything else alone. */
void rtf_restyle(struct rtf_doc* d, long from, long to, unsigned flags, int on);

/* Set the size index across a range. */
void rtf_resize(struct rtf_doc* d, long from, long to, unsigned index);

/* What the whole of a range has in common, for showing the toolbar's state:
 * a flag is reported set only when every character in the range has it. An
 * empty range reports the style at `from`, which is what a caret sitting
 * between two characters should inherit. */
unsigned char rtf_style_at(const struct rtf_doc* d, long from, long to);

#endif /* _RTF_H */
