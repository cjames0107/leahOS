#pragma once

#include <leah/types.hpp>

// SHA-256, written out because there is no library to borrow one from.
//
// Used for password hashing, which is the one thing here that must not be a
// toy: storing what the user typed, or a hash without a salt, would make the
// password file worth stealing.

namespace crypto {

constexpr usize kSha256Digest = 32;

void sha256(const void* data, usize length, u8 out[kSha256Digest]);

// Salted and stretched: hash the salt and password together, then re-hash the
// digest `iterations` times. The repetition is what makes a guess expensive -
// a single hash is far too fast to check a password with.
//
// Not memory-hard, so it is weaker than bcrypt or argon2 against an attacker
// with hardware; it is the honest limit of what fits here without a much larger
// implementation.
void password_hash(const char* salt, const char* password, u32 iterations,
                   u8 out[kSha256Digest]);

// Compare two digests without leaking where they first differ. A plain memcmp
// returns as soon as it finds a difference, and the timing of that says how
// much of a guess was right.
bool digest_equal(const u8* a, const u8* b);

} // namespace crypto
