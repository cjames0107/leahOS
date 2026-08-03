/* inflate - the decompressor for RFC 1951 deflate streams.
 *
 * Deflate says a stream is a sequence of blocks, each either literal bytes
 * copied verbatim, or a run of symbols in a Huffman code where a symbol is
 * either a byte or "go back D and copy L bytes". The two interesting jobs are
 * therefore reading canonical Huffman codes, and running the back-references.
 *
 * The codes are canonical, which means the table is entirely determined by how
 * many bits each symbol's code is - the codes themselves are never transmitted
 * and never stored here either. Sorting the symbols by code length puts them in
 * exactly the order the codes count upward in, so decoding walks bit by bit
 * comparing against the count at each length, and the symbol falls out as an
 * index. That is slower than the multi-bit lookup tables a production zlib
 * builds, and it is a few dozen lines instead of several hundred, which is the
 * right trade for reading icons and the occasional photograph.
 *
 * Bit order is the one thing worth stating twice, because deflate uses both.
 * Integers - lengths, distances, the block header - are packed low bit first.
 * Huffman codes are packed high bit of the code first. So `bits()` accumulates
 * upward and `decode()` shifts the code left as it goes, and neither is a typo.
 */

#include <inflate.h>
#include <string.h>

#define MAX_BITS  15        /* longest code deflate permits */
#define MAX_SYMS  288       /* literal/length alphabet, the larger of the two */

struct stream {
    const unsigned char* in;
    size_t               in_len;
    size_t               in_at;
    unsigned long        bitbuf;    /* bits read but not yet consumed */
    unsigned             bitcount;
    unsigned char*       out;
    size_t               out_cap;
    size_t               out_len;
    int                  failed;
};

/* Decoding a bit at a time is correct and slow: an average literal is nine
 * bits, so nine loop iterations and nine buffer checks per byte produced. A
 * photograph is four megabytes of output, and it showed - about thirty seconds
 * to open one.
 *
 * So the common case gets a table. FAST_BITS of the stream index straight into
 * an array holding the symbol and how long its code was; every code that short
 * is found in one lookup. Longer codes are rarer by construction - that is what
 * Huffman coding means - and fall back to the walk, which is still here and
 * still the definition of what the table must agree with.
 *
 * Nine bits is 512 entries, a kilobyte per code. Wider tables win less and cost
 * more to build, and a dynamic block rebuilds this every time.
 */
#define FAST_BITS 9
#define FAST_SIZE (1 << FAST_BITS)

/* A canonical code: how many symbols have each length, the symbols themselves
 * sorted by length and then by value, and the lookup table built from both. */
struct huffman {
    short count[MAX_BITS + 1];
    short symbol[MAX_SYMS];
    /* (length << 12) | symbol, or 0 where no code this short matches. A real
     * entry always has a length of at least 1, so zero is unambiguous. */
    unsigned short fast[FAST_SIZE];
};

/* One code pair per stream, and they are large enough to be worth keeping out
 * of the stack frame. Nothing here recurses or is entered twice at once. */
static struct huffman g_lit, g_dist;

/* --- reading bits --------------------------------------------------------- */

/* Runs off the end of the input by returning zeroes and raising `failed`,
 * rather than by reading past the buffer. A truncated stream is a thing that
 * happens to files, and it should end in an error return, not a fault. */
static unsigned bits(struct stream* s, unsigned need)
{
    while (s->bitcount < need) {
        if (s->in_at >= s->in_len) {
            s->failed = 1;
            return 0;
        }
        s->bitbuf |= (unsigned long)s->in[s->in_at++] << s->bitcount;
        s->bitcount += 8;
    }
    const unsigned value = (unsigned)(s->bitbuf & ((1ul << need) - 1));
    s->bitbuf >>= need;
    s->bitcount -= need;
    return value;
}

/* --- Huffman codes -------------------------------------------------------- */

/* Build a code from its lengths alone. A length of 0 means the symbol is not
 * in the code at all. */
static int build(struct huffman* h, const unsigned char* lengths, int n)
{
    short offset[MAX_BITS + 1];
    int i, len;

    for (len = 0; len <= MAX_BITS; ++len)
        h->count[len] = 0;
    for (i = 0; i < n; ++i)
        h->count[lengths[i]]++;
    if (h->count[0] == n)
        return -1;                      /* no code at all */

    /* Reject a code that claims more symbols at some length than the lengths
     * above it leave room for. An over-subscribed code has no valid reading,
     * and letting it through means decoding garbage confidently. An
     * *under*-subscribed one is allowed through: a single-symbol distance code
     * is incomplete by this measure and appears in real files. */
    int left = 1;
    for (len = 1; len <= MAX_BITS; ++len) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
            return -1;
    }

    offset[1] = 0;
    for (len = 1; len < MAX_BITS; ++len)
        offset[len + 1] = (short)(offset[len] + h->count[len]);
    for (i = 0; i < n; ++i)
        if (lengths[i] != 0)
            h->symbol[offset[lengths[i]]++] = (short)i;

    /* The lookup table. Canonical codes count upward within a length and carry
     * into the next, so walking the sorted symbols in order reproduces exactly
     * the codes decode() would have arrived at bit by bit.
     *
     * The stream delivers a code's high bit first, so the code is reversed to
     * become an index. Every entry whose low `len` bits match then names this
     * symbol, whatever the bits above them turn out to be - which is why the
     * fill strides by 1 << len rather than writing one slot. */
    for (i = 0; i < FAST_SIZE; ++i)
        h->fast[i] = 0;
    unsigned first = 0;
    int index = 0;
    for (len = 1; len <= MAX_BITS; ++len) {
        if (len <= FAST_BITS) {
            for (i = 0; i < h->count[len]; ++i) {
                const unsigned code = first + (unsigned)i;
                unsigned reversed = 0;
                for (int bit = 0; bit < len; ++bit)
                    reversed |= ((code >> bit) & 1u) << (len - 1 - bit);
                const unsigned short entry =
                    (unsigned short)(((unsigned)len << 12) |
                                     (unsigned)h->symbol[index + i]);
                for (unsigned at = reversed; at < FAST_SIZE; at += 1u << len)
                    h->fast[at] = entry;
            }
        }
        index += h->count[len];
        first = (first + (unsigned)h->count[len]) << 1;
    }
    return 0;
}

/* Walk the code one bit at a time. At each length, the codes of that length
 * occupy a contiguous run starting at `first`; if the bits so far land inside
 * that run, the symbol is at the matching offset in the sorted table. */
static int decode(struct stream* s, const struct huffman* h)
{
    int code = 0, first = 0, index = 0, len;

    /* Top up to a full index's worth, then look it up. Running out of input
     * here is not an error yet - it only means fewer bits than the table wants,
     * which the length check below turns into the slow path. */
    while (s->bitcount < FAST_BITS && s->in_at < s->in_len) {
        s->bitbuf |= (unsigned long)s->in[s->in_at++] << s->bitcount;
        s->bitcount += 8;
    }
    const unsigned short entry = h->fast[s->bitbuf & (FAST_SIZE - 1)];
    /* A hit is only usable if its code fits in the bits actually read. When it
     * does, the answer is right even at the end of the stream: the low `len`
     * bits of the index are real bits, and only those decided the entry. */
    if (entry != 0 && (unsigned)(entry >> 12) <= s->bitcount) {
        s->bitbuf >>= entry >> 12;
        s->bitcount -= entry >> 12;
        return entry & 0xFFF;
    }

    for (len = 1; len <= MAX_BITS; ++len) {
        code |= (int)bits(s, 1);
        if (s->failed)
            return -1;
        const int count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;                          /* longer than any code: malformed */
}

/* --- the two fixed tables ------------------------------------------------- */

/* Lengths 257..285 and distances 0..29, with how many extra bits each carries.
 * Length symbol 285 is 258 with no extra bits, which is the one irregularity
 * in the table and is in the standard that way. */
static const unsigned short k_len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
    67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const unsigned char k_len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
    4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const unsigned short k_dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
    769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const unsigned char k_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

/* --- blocks --------------------------------------------------------------- */

/* Literal bytes, byte-aligned, with a length and its complement ahead of them.
 * This is what `img_write_png` produces. */
static int stored_block(struct stream* s)
{
    s->bitbuf = 0;                      /* discard to the byte boundary */
    s->bitcount = 0;
    if (s->in_at + 4 > s->in_len)
        return -1;
    const unsigned len = (unsigned)s->in[s->in_at] |
                         ((unsigned)s->in[s->in_at + 1] << 8);
    const unsigned nlen = (unsigned)s->in[s->in_at + 2] |
                          ((unsigned)s->in[s->in_at + 3] << 8);
    s->in_at += 4;
    if ((len ^ 0xFFFFu) != nlen)
        return -1;
    if (s->in_at + len > s->in_len || s->out_len + len > s->out_cap)
        return -1;
    memcpy(&s->out[s->out_len], &s->in[s->in_at], len);
    s->in_at += len;
    s->out_len += len;
    return 0;
}

/* A block under whichever pair of codes was set up for it. The back-reference
 * copy is a byte at a time on purpose: a run may overlap its own source - "go
 * back 1 and copy 100" is how deflate spells a run of one repeated byte - and
 * memcpy of overlapping regions would produce something else. */
static int coded_block(struct stream* s, const struct huffman* lit,
                       const struct huffman* dist)
{
    for (;;) {
        const int symbol = decode(s, lit);
        if (symbol < 0)
            return -1;

        if (symbol < 256) {
            if (s->out_len >= s->out_cap)
                return -1;
            s->out[s->out_len++] = (unsigned char)symbol;
            continue;
        }
        if (symbol == 256)
            return 0;                   /* end of block */

        const int len_sym = symbol - 257;
        if (len_sym >= 29)
            return -1;
        const unsigned length = k_len_base[len_sym] +
                                bits(s, k_len_extra[len_sym]);

        const int dist_sym = decode(s, dist);
        if (dist_sym < 0 || dist_sym >= 30)
            return -1;
        const unsigned long distance = k_dist_base[dist_sym] +
                                       bits(s, k_dist_extra[dist_sym]);
        if (s->failed)
            return -1;
        if (distance > s->out_len)      /* reaches back before the output */
            return -1;
        if (s->out_len + length > s->out_cap)
            return -1;

        size_t from = s->out_len - distance;
        for (unsigned i = 0; i < length; ++i)
            s->out[s->out_len++] = s->out[from++];
    }
}

/* The code every stream may use without describing it. Built each time rather
 * than kept: fixed blocks are rare enough that the table costs more to hold
 * than to make. */
static int fixed_codes(void)
{
    unsigned char lengths[MAX_SYMS];
    int i;

    for (i = 0; i < 144; ++i)   lengths[i] = 8;
    for (; i < 256; ++i)        lengths[i] = 9;
    for (; i < 280; ++i)        lengths[i] = 7;
    for (; i < 288; ++i)        lengths[i] = 8;
    if (build(&g_lit, lengths, 288) != 0)
        return -1;

    for (i = 0; i < 30; ++i)    lengths[i] = 5;
    return build(&g_dist, lengths, 30);
}

/* The lengths of the code-length code itself arrive in this order, which puts
 * the ones most likely to be used first so the trailing ones can be omitted. */
static const unsigned char k_length_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

/* Read the pair of codes a dynamic block describes, which are themselves
 * encoded - a third Huffman code, over code lengths, with three symbols that
 * mean "repeat". */
static int dynamic_codes(struct stream* s)
{
    unsigned char lengths[MAX_SYMS + 32];
    struct huffman code_length;
    unsigned i;

    const unsigned nlit  = bits(s, 5) + 257;
    const unsigned ndist = bits(s, 5) + 1;
    const unsigned ncode = bits(s, 4) + 4;
    if (s->failed || nlit > 286 || ndist > 30)
        return -1;

    for (i = 0; i < ncode; ++i)
        lengths[k_length_order[i]] = (unsigned char)bits(s, 3);
    for (; i < 19; ++i)
        lengths[k_length_order[i]] = 0;
    if (s->failed || build(&code_length, lengths, 19) != 0)
        return -1;

    i = 0;
    while (i < nlit + ndist) {
        const int symbol = decode(s, &code_length);
        if (symbol < 0)
            return -1;

        if (symbol < 16) {
            lengths[i++] = (unsigned char)symbol;
            continue;
        }

        unsigned repeat;
        unsigned char value = 0;
        if (symbol == 16) {
            if (i == 0)                 /* nothing to repeat */
                return -1;
            value = lengths[i - 1];
            repeat = 3 + bits(s, 2);
        } else if (symbol == 17) {
            repeat = 3 + bits(s, 3);
        } else {
            repeat = 11 + bits(s, 7);
        }
        if (s->failed || i + repeat > nlit + ndist)
            return -1;
        while (repeat-- > 0)
            lengths[i++] = value;
    }

    /* A literal code with no end-of-block symbol would never terminate. */
    if (lengths[256] == 0)
        return -1;
    if (build(&g_lit, lengths, (int)nlit) != 0)
        return -1;
    /* The distance code may legitimately be empty, in a block that is all
     * literals. build() rejects an empty code, so let that stand and rely on
     * the block never asking for a distance. */
    build(&g_dist, &lengths[nlit], (int)ndist);
    return 0;
}

/* --- the stream ----------------------------------------------------------- */

long inflate_raw(const unsigned char* in, size_t in_len,
                 unsigned char* out, size_t out_cap)
{
    struct stream s;
    int final;

    if (in == 0 || out == 0)
        return -1;

    memset(&s, 0, sizeof(s));
    s.in = in;
    s.in_len = in_len;
    s.out = out;
    s.out_cap = out_cap;

    do {
        final = (int)bits(&s, 1);
        const unsigned type = bits(&s, 2);
        if (s.failed)
            return -1;

        int rc;
        if (type == 0) {
            rc = stored_block(&s);
        } else if (type == 1) {
            rc = fixed_codes();
            if (rc == 0)
                rc = coded_block(&s, &g_lit, &g_dist);
        } else if (type == 2) {
            rc = dynamic_codes(&s);
            if (rc == 0)
                rc = coded_block(&s, &g_lit, &g_dist);
        } else {
            rc = -1;                    /* type 3 is reserved */
        }
        if (rc != 0 || s.failed)
            return -1;
    } while (!final);

    return (long)s.out_len;
}

/* Adler-32: two running sums mod 65521, the second over the first. Cheaper
 * than a CRC and enough to catch a stream that decoded to the wrong bytes. */
static unsigned long adler32_of(const unsigned char* data, size_t len)
{
    unsigned long a = 1, b = 0;
    for (size_t i = 0; i < len; ++i) {
        a += data[i];
        if (a >= 65521ul)
            a -= 65521ul;
        b += a;
        if (b >= 65521ul)
            b -= 65521ul;
    }
    return (b << 16) | a;
}

long inflate_zlib(const unsigned char* in, size_t in_len,
                  unsigned char* out, size_t out_cap)
{
    if (in == 0 || in_len < 6)          /* header, trailer, and something */
        return -1;

    const unsigned cmf = in[0], flg = in[1];
    if ((cmf & 0x0F) != 8)              /* compression method: deflate */
        return -1;
    if ((cmf >> 4) > 7)                 /* window larger than 32K */
        return -1;
    if (((cmf << 8) | flg) % 31u != 0)  /* the header's own check bits */
        return -1;
    if (flg & 0x20)                     /* a preset dictionary we do not have */
        return -1;

    const long produced = inflate_raw(in + 2, in_len - 2, out, out_cap);
    if (produced < 0)
        return -1;

    /* The trailer sits at the end of the stream, which is where the input ends
     * - the compressed data does not announce its own length, so this trusts
     * the caller to have passed the stream and not the stream plus trailing
     * bytes. PNG's joined IDAT is exactly that. */
    const unsigned char* trailer = in + in_len - 4;
    const unsigned long want = ((unsigned long)trailer[0] << 24) |
                               ((unsigned long)trailer[1] << 16) |
                               ((unsigned long)trailer[2] << 8) | trailer[3];
    if (adler32_of(out, (size_t)produced) != want)
        return -1;

    return produced;
}
