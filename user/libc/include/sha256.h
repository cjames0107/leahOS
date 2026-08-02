#ifndef _SHA256_H
#define _SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST 32

void sha256(const void* data, size_t length, uint8_t out[SHA256_DIGEST]);

/* Salted and stretched: hash the salt and password together, then re-hash the
 * digest `iterations` times. The repetition is what makes a guess expensive - a
 * single hash is far too fast to check a password with.
 *
 * Not memory-hard, so it is weaker than bcrypt or argon2 against an attacker
 * with hardware; it is the honest limit of what fits here. */
void password_hash(const char* salt, const char* password, uint32_t iterations,
                   uint8_t out[SHA256_DIGEST]);

/* Compare without leaking where they first differ. A plain memcmp returns as
 * soon as it finds a difference, and the timing of that says how much of a
 * guess was right. */
int digest_equal(const uint8_t* a, const uint8_t* b);

#endif
