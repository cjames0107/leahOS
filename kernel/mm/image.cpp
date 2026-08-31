/* See <leah/image.hpp>. */

#include <leah/console.hpp>
#include <leah/heap.hpp>
#include <leah/image.hpp>
#include <leah/lock.hpp>
#include <leah/memory.hpp>
#include <leah/pmm.hpp>
#include <leah/scheduler.hpp>
#include <leah/string.hpp>
#include <leah/vmm.hpp>

namespace image {
namespace {

struct Image {
    bool     used;
    char     name[kNameMax];
    u64      version;
    u64      bytes;
    usize    pages;
    paddr_t* frames;        // one per page, in order
    u64      used_at;       // for eviction: the tick it was last asked for
};

Image g_images[kMaxImages];
u64   g_clock;

/* Taken below the handle table and above the frame allocator, which is the
 * order things actually happen in: a process presents a handle, the handle
 * names an image, and mapping the image allocates frames. */
sync::RankedLock g_lock(sync::Rank::Image, "image");

/* An image's frames are private to the image until somebody maps them, so the
 * pages beyond the last byte are zero - which is what a program with a
 * segment whose file part ends mid-page needs to read there. */
bool fill(Image& image, const u8* bytes, u64 size)
{
    for (usize i = 0; i < image.pages; ++i) {
        const paddr_t frame = pmm::alloc();
        if (frame == 0) {
            for (usize j = 0; j < i; ++j)
                pmm::free(image.frames[j]);
            return false;
        }
        auto* page = reinterpret_cast<u8*>(memory::phys_to_direct(frame));
        const u64 at = static_cast<u64>(i) * vmm::kPageSize;
        const u64 take = size - at < vmm::kPageSize ? size - at : vmm::kPageSize;
        memcpy(page, bytes + at, take);
        if (take < vmm::kPageSize)
            memset(page + take, 0, vmm::kPageSize - take);
        image.frames[i] = frame;
    }
    return true;
}

void discard(Image& image)
{
    if (image.frames != nullptr) {
        /* Release, not free. Something may still be running against this
         * image; dropping the image's own reference is all that is being said
         * here, and the frames go when the last mapping of them does. */
        for (usize i = 0; i < image.pages; ++i)
            pmm::release(image.frames[i]);
        kfree(image.frames);
    }
    image.frames = nullptr;
    image.used = false;
    image.pages = 0;
    image.bytes = 0;
    image.name[0] = '\0';
}

/* A free slot, or the least recently used one emptied out. Evicting is always
 * safe and never wrong - the worst it costs is one read of a file the next
 * time it is run. */
Image* take_slot()
{
    for (usize i = 0; i < kMaxImages; ++i)
        if (!g_images[i].used)
            return &g_images[i];

    Image* oldest = &g_images[0];
    for (usize i = 1; i < kMaxImages; ++i)
        if (g_images[i].used_at < oldest->used_at)
            oldest = &g_images[i];
    discard(*oldest);
    return oldest;
}

bool same_name(const Image& image, const char* name)
{
    for (usize i = 0; i < kNameMax; ++i) {
        if (image.name[i] != name[i])
            return false;
        if (name[i] == '\0')
            return true;
    }
    return false;                       // longer than a name can be
}

/* A pointer handed back to us. Checked against the array rather than trusted,
 * because the only thing standing between a stale one and somebody else's
 * pages is this function. */
Image* of(void* pointer)
{
    auto* image = static_cast<Image*>(pointer);
    if (image < &g_images[0] || image >= &g_images[kMaxImages])
        return nullptr;
    if (((reinterpret_cast<u8*>(image) - reinterpret_cast<u8*>(&g_images[0])) %
         sizeof(Image)) != 0)
        return nullptr;
    return image->used ? image : nullptr;
}

} // namespace

void init()
{
    for (usize i = 0; i < kMaxImages; ++i)
        g_images[i].used = false;
}

void* find(const char* name, u64 version)
{
    sync::Guard guard(g_lock);
    return find_locked(name, version);
}

void* find_locked(const char* name, u64 version)
{
    for (usize i = 0; i < kMaxImages; ++i) {
        Image& image = g_images[i];
        if (!image.used || image.version != version || !same_name(image, name))
            continue;
        image.used_at = ++g_clock;
        return &image;
    }
    return nullptr;
}

void* create(const char* name, u64 version, const u8* bytes, u64 size)
{
    if (size == 0 || size > kMaxBytes)
        return nullptr;
    sync::Guard guard(g_lock);

    /* Already here at this version: the caller raced another exec of the same
     * program, which is the ordinary case when a shell starts two at once. */
    void* existing = find_locked(name, version);
    if (existing != nullptr)
        return existing;

    Image* image = take_slot();
    const usize pages =
        static_cast<usize>((size + vmm::kPageSize - 1) / vmm::kPageSize);
    auto* frames = static_cast<paddr_t*>(kmalloc(pages * sizeof(paddr_t)));
    if (frames == nullptr)
        return nullptr;

    image->frames = frames;
    image->pages = pages;
    image->bytes = size;
    if (!fill(*image, bytes, size)) {
        kfree(frames);
        image->frames = nullptr;
        image->pages = 0;
        return nullptr;
    }

    usize n = 0;
    while (n + 1 < kNameMax && name[n] != '\0') {
        image->name[n] = name[n];
        ++n;
    }
    image->name[n] = '\0';
    image->version = version;
    image->used = true;
    image->used_at = ++g_clock;
    return image;
}

bool valid(void* image) { return of(image) != nullptr; }

u64 size_of(void* pointer)
{
    sync::Guard guard(g_lock);
    const Image* image = of(pointer);
    return image != nullptr ? image->bytes : 0;
}

paddr_t frame_at(void* pointer, u64 offset)
{
    sync::Guard guard(g_lock);
    return frame_at_locked(pointer, offset);
}

paddr_t frame_at_locked(void* pointer, u64 offset)
{
    const Image* image = of(pointer);
    if (image == nullptr || (offset & (vmm::kPageSize - 1)) != 0)
        return 0;
    const usize page = static_cast<usize>(offset / vmm::kPageSize);
    return page < image->pages ? image->frames[page] : 0;
}

bool share_frame(void* pointer, u64 offset)
{
    sync::Guard guard(g_lock);
    const paddr_t frame = frame_at_locked(pointer, offset);
    return frame != 0 && pmm::share(frame);
}

bool read(void* pointer, u64 offset, void* into, u64 bytes)
{
    sync::Guard guard(g_lock);
    const Image* image = of(pointer);
    if (image == nullptr || offset > image->bytes ||
        bytes > image->bytes - offset)
        return false;

    auto* out = static_cast<u8*>(into);
    u64 done = 0;
    while (done < bytes) {
        const u64 at = offset + done;
        const usize page = static_cast<usize>(at / vmm::kPageSize);
        const u64 within = at % vmm::kPageSize;
        u64 take = vmm::kPageSize - within;
        if (take > bytes - done)
            take = bytes - done;
        const auto* from =
            reinterpret_cast<const u8*>(memory::phys_to_direct(image->frames[page]));
        memcpy(out + done, from + within, static_cast<usize>(take));
        done += take;
    }
    return true;
}

} // namespace image

