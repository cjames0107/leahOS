#include <leah/console.hpp>
#include <leah/elf.hpp>
#include <leah/pmm.hpp>
#include <leah/process.hpp>
#include <leah/syscall.hpp>
#include <leah/vmm.hpp>

namespace process {
namespace {

bool map_user_stack()
{
    for (usize i = 0; i < kUserStackPages; ++i) {
        const vaddr_t page = kUserStackTop - (i + 1) * vmm::kPageSize;
        const paddr_t frame = pmm::alloc();
        if (frame == 0)
            return false;
        if (!vmm::map(page, frame, vmm::Write | vmm::User | vmm::NoExecute))
            return false;
    }
    return true;
}

} // namespace

Result run(const char* path)
{
    const vmm::AddressSpace kernel = vmm::kernel_space();

    // Give the program its own page tables, sharing the kernel's mappings so a
    // syscall or interrupt taken while it runs still finds the kernel. Switch to
    // it before loading, so the ELF's pages and stack land in this space rather
    // than the kernel's.
    const vmm::AddressSpace space = vmm::create_address_space();
    if (space == 0)
        return { false, 0 };
    vmm::switch_address_space(space);

    elf::Image image{};
    const elf::Error error = elf::load(path, image);
    if (error != elf::Error::None || !map_user_stack()) {
        vmm::switch_address_space(kernel);
        vmm::destroy_address_space(space);
        if (error != elf::Error::None)
            console::printf("  process: %s: %s\n", path, elf::error_name(error));
        return { false, 0 };
    }

    const u64 code = syscall::run(image.entry, kUserStackTop);

    // Back to the kernel's own space, then reclaim everything the program used.
    // destroy frees only the slots this space added, so the shared kernel
    // mappings survive for the next process.
    vmm::switch_address_space(kernel);
    vmm::destroy_address_space(space);

    return { true, code };
}

} // namespace process
