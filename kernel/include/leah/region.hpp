#pragma once

#include <leah/types.hpp>
#include <leah/vmm.hpp>

// What backs a range of addresses.
//
// The kernel has never had this. Every mapping decision was made once, at the
// moment a page table entry was written, and after that a page was whatever
// the entry said - there was nowhere to record *why* it said that. Which is
// why a file mapping had to be built in full up front: the fault handler could
// not have found out what belonged at an address it had not been told about.
//
// A region is that record. It says a range of one address space is backed by a
// held image from a given offset, so a fault inside it can be answered by
// mapping the one page that faulted rather than by having mapped all of them
// in advance. Other systems call this a VMA and hang far more off it - named
// mappings, protection changes, merging and splitting. This is the smallest
// version that makes demand paging possible, and it will grow when something
// needs it to.
//
// Regions belong to an address space, not to a task: threads share both, and a
// forked child gets a copy so that its private mapping stays private.

namespace region {

// How many mappings the machine may have at once, across every process. A
// mapped file is still an unusual thing here; a desktop's worth of programs
// uses a handful.
constexpr usize kMaxRegions = 128;

void init();

// Record that [base, base + bytes) of `space` is `image` from `offset`.
// `flags` are the vmm page flags the pages should be given when they fault in.
bool add(vmm::AddressSpace space, u64 base, u64 bytes, void* image, u64 offset,
         u64 flags);

// What backs `address` in `space`, if anything. Fills in the image, the offset
// of the page containing the address, and the flags it should be mapped with.
bool find(vmm::AddressSpace space, u64 address, void** out_image,
          u64* out_offset, u64* out_flags);

// The child of a fork sees what its parent saw.
bool inherit(vmm::AddressSpace from, vmm::AddressSpace to);

// Everything an address space had, dropped. Called when it is destroyed.
void forget(vmm::AddressSpace space);

} // namespace region
