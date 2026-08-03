#ifndef _INFLATE_H
#define _INFLATE_H

#include <stddef.h>

/* Deflate decompression - RFC 1951, and the zlib wrapper of RFC 1950.
 *
 * This is the whole format: stored blocks, fixed Huffman codes, and dynamic
 * ones with the code-length alphabet that describes them. It is what PNG's
 * IDAT is written in, what gzip carries, and what zip members use, so it is
 * worth having once in libc rather than a stunted copy in each reader.
 *
 * Decompression only. Writing a compressor is a separate job with separate
 * decisions - `img_write_png` still emits stored blocks, which is valid
 * deflate and always will be.
 *
 * The output buffer is supplied by the caller at its final size, and both
 * functions refuse to exceed it. That is not a shortcut: a back-reference
 * copies from what has already been produced, so a whole-output buffer *is*
 * the sliding window, and one that never wraps is simpler than one that does.
 * Callers of this generally know their output size already - PNG does, from
 * its own header - and one that does not should grow a buffer and retry.
 */

/* Both return the number of bytes produced, or -1 if the stream is malformed,
 * truncated, or larger than `out_cap`. */

long inflate_raw(const unsigned char* in, size_t in_len,
                 unsigned char* out, size_t out_cap);

/* The same, behind the two-byte zlib header and four-byte Adler-32 trailer.
 * The checksum is verified: a stream that decompresses to the wrong bytes is
 * a failure, not a picture with something wrong in it. */
long inflate_zlib(const unsigned char* in, size_t in_len,
                  unsigned char* out, size_t out_cap);

#endif /* _INFLATE_H */
