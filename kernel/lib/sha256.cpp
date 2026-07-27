#include <leah/sha256.hpp>
#include <leah/string.hpp>

namespace crypto {
namespace {

constexpr u32 kRoundConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

u32 rotate_right(u32 value, u32 bits) { return value >> bits | value << (32 - bits); }

void compress(u32 state[8], const u8 block[64])
{
    u32 w[64];
    for (u32 i = 0; i < 16; ++i) {
        w[i] = static_cast<u32>(block[i * 4]) << 24 |
               static_cast<u32>(block[i * 4 + 1]) << 16 |
               static_cast<u32>(block[i * 4 + 2]) << 8 |
               static_cast<u32>(block[i * 4 + 3]);
    }
    for (u32 i = 16; i < 64; ++i) {
        const u32 s0 = rotate_right(w[i - 15], 7) ^ rotate_right(w[i - 15], 18) ^
                       (w[i - 15] >> 3);
        const u32 s1 = rotate_right(w[i - 2], 17) ^ rotate_right(w[i - 2], 19) ^
                       (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    u32 a = state[0], b = state[1], c = state[2], d = state[3];
    u32 e = state[4], f = state[5], g = state[6], h = state[7];

    for (u32 i = 0; i < 64; ++i) {
        const u32 s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        const u32 choose = (e & f) ^ (~e & g);
        const u32 t1 = h + s1 + choose + kRoundConstants[i] + w[i];
        const u32 s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        const u32 majority = (a & b) ^ (a & c) ^ (b & c);
        const u32 t2 = s0 + majority;

        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

} // namespace

void sha256(const void* data, usize length, u8 out[kSha256Digest])
{
    u32 state[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };

    const auto* bytes = static_cast<const u8*>(data);
    usize remaining = length;
    while (remaining >= 64) {
        compress(state, bytes);
        bytes += 64;
        remaining -= 64;
    }

    // The tail: the message, a single 1 bit, zero padding, then the original
    // length in bits as a big-endian 64-bit count. If the length will not fit
    // in this block, it takes a second one.
    u8 tail[128] = {};
    memcpy(tail, bytes, remaining);
    tail[remaining] = 0x80;
    const usize blocks = remaining + 9 > 64 ? 2u : 1u;
    const u64 bits = static_cast<u64>(length) * 8;
    for (u32 i = 0; i < 8; ++i)
        tail[blocks * 64 - 1 - i] = static_cast<u8>(bits >> (i * 8));

    for (usize i = 0; i < blocks; ++i)
        compress(state, tail + i * 64);

    for (u32 i = 0; i < 8; ++i) {
        out[i * 4]     = static_cast<u8>(state[i] >> 24);
        out[i * 4 + 1] = static_cast<u8>(state[i] >> 16);
        out[i * 4 + 2] = static_cast<u8>(state[i] >> 8);
        out[i * 4 + 3] = static_cast<u8>(state[i]);
    }
}

void password_hash(const char* salt, const char* password, u32 iterations,
                   u8 out[kSha256Digest])
{
    u8 buffer[256] = {};
    usize n = 0;
    for (usize i = 0; salt[i] != '\0' && n < sizeof(buffer); ++i)
        buffer[n++] = static_cast<u8>(salt[i]);
    for (usize i = 0; password[i] != '\0' && n < sizeof(buffer); ++i)
        buffer[n++] = static_cast<u8>(password[i]);

    sha256(buffer, n, out);
    for (u32 i = 1; i < iterations; ++i)
        sha256(out, kSha256Digest, out);
}

bool digest_equal(const u8* a, const u8* b)
{
    u8 difference = 0;
    for (usize i = 0; i < kSha256Digest; ++i)
        difference |= static_cast<u8>(a[i] ^ b[i]);
    return difference == 0;
}

} // namespace crypto
