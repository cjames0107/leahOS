/* See <leah/object.hpp>. */

#include <leah/object.hpp>
#include <leah/heap.hpp>
#include <leah/string.hpp>

namespace object {
namespace {

struct Entry {
    Type  type;
    u32   rights;
    void* pointer;
    u32   generation;
};

/* One table per process, found by pid.
 *
 * A flat array rather than something hung off the task, because a task is not
 * a process: threads share a thread group and must share its handles, and the
 * table has to outlive whichever thread happened to open something. Keying on
 * the thread group's pid is what makes "the process holds this" true.
 */
struct Table {
    bool  used;
    u32   pid;
    Entry entries[kMaxHandles];
};

constexpr usize kMaxTables = 96;        // matches the task limit

/* Pointers, and the tables themselves on the heap.
 *
 * Ninety-six tables of a hundred and twenty-eight entries is three hundred
 * kilobytes, and it would sit in the kernel's .bss whether or not a single
 * process ever held a handle - which, for a while yet, most will not. The
 * table is made the first time a process is given something. */
Table* g_tables[kMaxTables];

Table* find(u32 pid)
{
    for (usize i = 0; i < kMaxTables; ++i)
        if (g_tables[i] != nullptr && g_tables[i]->used &&
            g_tables[i]->pid == pid)
            return g_tables[i];
    return nullptr;
}

Table* find_or_make(u32 pid)
{
    Table* table = find(pid);
    if (table != nullptr)
        return table;
    for (usize i = 0; i < kMaxTables; ++i) {
        if (g_tables[i] != nullptr && g_tables[i]->used)
            continue;
        if (g_tables[i] == nullptr) {
            g_tables[i] = static_cast<Table*>(kmalloc(sizeof(Table)));
            if (g_tables[i] == nullptr)
                return nullptr;
        }
        memset(g_tables[i], 0, sizeof(Table));
        g_tables[i]->used = true;
        g_tables[i]->pid = pid;
        return g_tables[i];
    }
    return nullptr;
}

/* Ten bits of slot and the rest generation, which is the same split shm uses
 * and for the same reason. A generation of zero never appears, so a zeroed
 * table can never be mistaken for a live handle. */
constexpr u32 kSlotBits = 10;
constexpr u32 kSlotMask = (1u << kSlotBits) - 1;

static_assert(kMaxHandles <= (1u << kSlotBits),
              "the handle encoding gives the slot ten bits");

Handle encode(usize slot, u32 generation)
{
    return static_cast<Handle>((generation << kSlotBits) |
                               static_cast<u32>(slot));
}

Entry* decode(Table* table, Handle handle)
{
    if (table == nullptr || handle < 0)
        return nullptr;
    const u32 value = static_cast<u32>(handle);
    const usize slot = value & kSlotMask;
    if (slot >= kMaxHandles)
        return nullptr;
    Entry& entry = table->entries[slot];
    if (entry.type == Type::None || entry.generation != (value >> kSlotBits))
        return nullptr;
    return &entry;
}

} // namespace

void init()
{
    for (usize i = 0; i < kMaxTables; ++i)
        g_tables[i] = nullptr;
}

Handle give(u32 pid, Type type, void* pointer, u32 rights)
{
    if (type == Type::None)
        return kNoHandle;
    Table* table = find_or_make(pid);
    if (table == nullptr)
        return kNoHandle;

    for (usize slot = 0; slot < kMaxHandles; ++slot) {
        Entry& entry = table->entries[slot];
        if (entry.type != Type::None)
            continue;
        /* Bumped on reuse, not on first use, so that the number handed out for
         * a slot is never the number that was handed out for it before. */
        ++entry.generation;
        if (entry.generation == 0)
            entry.generation = 1;
        entry.type = type;
        entry.rights = rights;
        entry.pointer = pointer;
        return encode(slot, entry.generation);
    }
    return kNoHandle;
}

void* look(u32 pid, Handle handle, Type type, u32 needed)
{
    const Entry* entry = decode(find(pid), handle);
    if (entry == nullptr || entry->type != type)
        return nullptr;
    /* Every right, not any: a caller asking for read and write on a handle
     * that carries only read is asking for something it was not given. */
    if ((entry->rights & needed) != needed)
        return nullptr;
    return entry->pointer;
}

u32 rights_of(u32 pid, Handle handle)
{
    const Entry* entry = decode(find(pid), handle);
    return entry != nullptr ? entry->rights : 0;
}

Type type_of(u32 pid, Handle handle)
{
    const Entry* entry = decode(find(pid), handle);
    return entry != nullptr ? entry->type : Type::None;
}

Handle duplicate(u32 pid, Handle handle, u32 mask)
{
    Table* table = find(pid);
    const Entry* entry = decode(table, handle);
    if (entry == nullptr || (entry->rights & kDuplicate) == 0)
        return kNoHandle;
    /* Narrowed, never widened. A mask asking for a right the original does not
     * hold simply does not get it - which is what makes handing a handle to
     * something less trusted a safe thing to do. */
    return give(pid, entry->type, entry->pointer, entry->rights & mask);
}

bool close(u32 pid, Handle handle)
{
    Entry* entry = decode(find(pid), handle);
    if (entry == nullptr)
        return false;
    entry->type = Type::None;
    entry->rights = 0;
    entry->pointer = nullptr;
    return true;
}

void close_all(u32 pid)
{
    Table* table = find(pid);
    if (table == nullptr)
        return;
    table->used = false;
}

void inherit(u32 from_pid, u32 to_pid)
{
    const Table* from = find(from_pid);
    if (from == nullptr)
        return;
    Table* to = find_or_make(to_pid);
    if (to == nullptr)
        return;
    for (usize slot = 0; slot < kMaxHandles; ++slot)
        to->entries[slot] = from->entries[slot];
    to->pid = to_pid;
    to->used = true;
}

} // namespace object
