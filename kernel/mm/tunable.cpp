/* See <leah/tunable.hpp>. */

#include <leah/tunable.hpp>

namespace tunable {
namespace {

struct Entry {
    i64 value;
    i64 lowest;
    i64 highest;
};

/* Bounds are part of the definition, not a check bolted on. A tunable without
 * them is a way to wedge the machine from a shell. */
Entry g_entries[static_cast<usize>(Key::Count)] = {
    { 0, 0, 0 },            // None
    { 1,   1,  64 },        // MapAhead: one page is pure demand paging
    { 32,  4,  64 },        // ImageLimit
    { 1,   0,   1 },        // PoisonFrames
    { 1,   0,   1 },        // MapFileEager
};

Entry* of(Key key)
{
    const auto i = static_cast<usize>(key);
    if (key == Key::None || i >= static_cast<usize>(Key::Count))
        return nullptr;
    return &g_entries[i];
}

} // namespace

void init() { }

i64 get(Key key)
{
    const Entry* e = of(key);
    return e != nullptr ? e->value : -1;
}

bool set(Key key, i64 value)
{
    Entry* e = of(key);
    if (e == nullptr || value < e->lowest || value > e->highest)
        return false;
    /* A single aligned word, written whole. Anything reading it concurrently
     * sees the old value or the new one and never half of each, which is all
     * a tunable needs - none of them is read as part of a decision that has to
     * be consistent with another. */
    __atomic_store_n(&e->value, value, __ATOMIC_RELAXED);
    return true;
}

u32 map_ahead()
{
    return static_cast<u32>(__atomic_load_n(
        &g_entries[static_cast<usize>(Key::MapAhead)].value, __ATOMIC_RELAXED));
}

bool map_file_eager()
{
    return __atomic_load_n(
               &g_entries[static_cast<usize>(Key::MapFileEager)].value,
               __ATOMIC_RELAXED) != 0;
}

bool poison_frames()
{
    return __atomic_load_n(
               &g_entries[static_cast<usize>(Key::PoisonFrames)].value,
               __ATOMIC_RELAXED) != 0;
}

} // namespace tunable
