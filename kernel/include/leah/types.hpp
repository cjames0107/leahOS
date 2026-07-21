#pragma once

// Fixed-width integer types. Freestanding <cstdint> exists, but spelling these
// out keeps the kernel's vocabulary short and independent of the host headers.

using u8  = unsigned char;
using u16 = unsigned short;
using u32 = unsigned int;
using u64 = unsigned long long;

using i8  = signed char;
using i16 = short;
using i32 = int;
using i64 = long long;

using usize = u64;
using isize = i64;

using paddr_t = u64;    // physical address
using vaddr_t = u64;    // virtual address

static_assert(sizeof(u8) == 1);
static_assert(sizeof(u16) == 2);
static_assert(sizeof(u32) == 4);
static_assert(sizeof(u64) == 8);
static_assert(sizeof(void*) == 8, "leahOS targets x86-64 only");
