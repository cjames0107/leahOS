#pragma once

#include <leah/types.hpp>

// Kernel heap. A first-fit free list over a virtual region grown on demand from
// the frame allocator.

namespace heap {

void init();

void* allocate(usize bytes);
void* allocate_aligned(usize bytes, usize alignment);
void  release(void* pointer);

usize heap_size();
usize used_bytes();

} // namespace heap

void* kmalloc(usize bytes);
void  kfree(void* pointer);
