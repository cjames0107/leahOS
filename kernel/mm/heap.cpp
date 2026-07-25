#include <leah/heap.hpp>
#include <leah/memory.hpp>
#include <leah/panic.hpp>
#include <leah/pmm.hpp>
#include <leah/string.hpp>
#include <leah/vmm.hpp>

namespace heap {
namespace {

// Well clear of both the identity map and anything the firmware described, so
// a stray heap pointer lands somewhere obviously wrong rather than quietly on
// top of physical memory.
constexpr vaddr_t kHeapBase = memory::kHeapBase;       // high half, its own slot
constexpr usize kGrowthPages = 16;                     // 64 KiB at a time
constexpr u32 kMagic = 0x1EA4B10C;

// 32 bytes exactly, so a 16-byte aligned block yields a 16-byte aligned
// payload without any further padding.
struct alignas(16) Block {
    usize  size;        // payload bytes
    Block* prev;        // address order, not free-list order
    Block* next;
    u32    free;
    u32    magic;
};

static_assert(sizeof(Block) == 32);

Block* g_first = nullptr;
Block* g_last  = nullptr;

vaddr_t g_break = kHeapBase;    // first unmapped byte
usize g_size = 0;
usize g_used = 0;

constexpr usize align_up(usize value, usize alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

Block* payload_to_block(void* pointer)
{
    return reinterpret_cast<Block*>(static_cast<u8*>(pointer) - sizeof(Block));
}

void* block_to_payload(Block* block)
{
    return reinterpret_cast<u8*>(block) + sizeof(Block);
}

// Pull more frames from the PMM and map them at the end of the heap. Frames
// need not be physically contiguous - that is the entire point of having a VMM.
bool grow(usize bytes)
{
    const usize pages = align_up(bytes + sizeof(Block), pmm::kPageSize) / pmm::kPageSize;
    const usize count = pages < kGrowthPages ? kGrowthPages : pages;

    const vaddr_t start = g_break;
    for (usize i = 0; i < count; ++i) {
        const paddr_t frame = pmm::alloc();
        if (frame == 0)
            return false;
        if (!vmm::map(g_break, frame, vmm::Write | vmm::NoExecute)) {
            pmm::free(frame);
            return false;
        }
        g_break += pmm::kPageSize;
    }

    const usize added = count * pmm::kPageSize;
    g_size += added;

    auto* block = reinterpret_cast<Block*>(start);
    block->size  = added - sizeof(Block);
    block->prev  = g_last;
    block->next  = nullptr;
    block->free  = 1;
    block->magic = kMagic;

    if (g_last != nullptr)
        g_last->next = block;
    else
        g_first = block;
    g_last = block;

    // The new region is adjacent to the old tail, so if that was free the two
    // are really one block - merging keeps large allocations satisfiable.
    if (block->prev != nullptr && block->prev->free != 0) {
        Block* prev = block->prev;
        prev->size += sizeof(Block) + block->size;
        prev->next = nullptr;
        g_last = prev;
    }
    return true;
}

void split(Block* block, usize wanted)
{
    // Only worth splitting if the tail can hold a header plus something useful.
    if (block->size < wanted + sizeof(Block) + 16)
        return;

    auto* rest = reinterpret_cast<Block*>(
        reinterpret_cast<u8*>(block_to_payload(block)) + wanted);

    rest->size  = block->size - wanted - sizeof(Block);
    rest->prev  = block;
    rest->next  = block->next;
    rest->free  = 1;
    rest->magic = kMagic;

    if (block->next != nullptr)
        block->next->prev = rest;
    else
        g_last = rest;

    block->next = rest;
    block->size = wanted;
}

void coalesce(Block* block)
{
    Block* next = block->next;
    if (next != nullptr && next->free != 0) {
        block->size += sizeof(Block) + next->size;
        block->next = next->next;
        if (next->next != nullptr)
            next->next->prev = block;
        else
            g_last = block;
    }

    Block* prev = block->prev;
    if (prev != nullptr && prev->free != 0) {
        prev->size += sizeof(Block) + block->size;
        prev->next = block->next;
        if (block->next != nullptr)
            block->next->prev = prev;
        else
            g_last = prev;
    }
}

} // namespace

void init()
{
    if (!grow(kGrowthPages * pmm::kPageSize))
        panic("heap: cannot map the initial heap region");
}

void* allocate(usize bytes)
{
    if (bytes == 0)
        return nullptr;

    const usize wanted = align_up(bytes, 16);

    for (int attempt = 0; attempt < 2; ++attempt) {
        for (Block* block = g_first; block != nullptr; block = block->next) {
            if (block->free == 0 || block->size < wanted)
                continue;
            split(block, wanted);
            block->free = 0;
            g_used += block->size;
            return block_to_payload(block);
        }
        if (attempt == 0 && !grow(wanted))
            return nullptr;
    }
    return nullptr;
}

void* allocate_aligned(usize bytes, usize alignment)
{
    if (alignment <= 16)
        return allocate(bytes);

    // Over-allocate and hand back an aligned interior pointer. The slack byte
    // count is stored just below it so release() can find the real block.
    void* raw = allocate(bytes + alignment + sizeof(u64));
    if (raw == nullptr)
        return nullptr;

    const auto base = reinterpret_cast<u64>(raw);
    const u64 aligned = align_up(base + sizeof(u64), alignment);
    reinterpret_cast<u64*>(aligned)[-1] = aligned - base;
    return reinterpret_cast<void*>(aligned);
}

void release(void* pointer)
{
    if (pointer == nullptr)
        return;

    Block* block = payload_to_block(pointer);
    if (block->magic != kMagic) {
        // Probably an aligned allocation; step back to the real header.
        const u64 slack = reinterpret_cast<u64*>(pointer)[-1];
        block = payload_to_block(static_cast<u8*>(pointer) - slack);
        if (block->magic != kMagic)
            panic("heap: free of an invalid or corrupted pointer");
    }
    if (block->free != 0)
        panic("heap: double free");

    g_used -= block->size;
    block->free = 1;
    coalesce(block);
}

usize heap_size()  { return g_size; }
usize used_bytes() { return g_used; }

} // namespace heap

void* kmalloc(usize bytes) { return heap::allocate(bytes); }
void  kfree(void* pointer) { heap::release(pointer); }

// Freestanding C++ still expects these to exist the moment anything uses new.
//
// These must be declared with the ABI's own size_t - `unsigned long` here -
// rather than our u64 `unsigned long long`. The two are both 64 bits but are
// distinct types, and the compiler will not accept a near-miss signature.
using size_t = __SIZE_TYPE__;

void* operator new(size_t bytes)                  { return heap::allocate(bytes); }
void* operator new[](size_t bytes)                { return heap::allocate(bytes); }
void  operator delete(void* p)           noexcept { heap::release(p); }
void  operator delete[](void* p)         noexcept { heap::release(p); }
void  operator delete(void* p, size_t)   noexcept { heap::release(p); }
void  operator delete[](void* p, size_t) noexcept { heap::release(p); }
