#include <leah/interrupts.hpp>
#include <leah/io.hpp>
#include <leah/pic.hpp>

namespace pic {
namespace {

constexpr u16 kMasterCommand = 0x20;
constexpr u16 kMasterData    = 0x21;
constexpr u16 kSlaveCommand  = 0xA0;
constexpr u16 kSlaveData     = 0xA1;

constexpr u8 kEoi = 0x20;

constexpr u8 kIcw1Init      = 0x10;
constexpr u8 kIcw1Icw4      = 0x01;
constexpr u8 kIcw4Mode8086  = 0x01;

constexpr u8 kReadIsr = 0x0B;    // OCW3: read the In-Service Register

u8 read_isr(u16 command_port)
{
    io::out8(command_port, kReadIsr);
    return io::in8(command_port);
}

} // namespace

void init()
{
    const u8 master_mask = io::in8(kMasterData);
    const u8 slave_mask  = io::in8(kSlaveData);

    // ICW1: begin initialisation, ICW4 will follow.
    io::out8(kMasterCommand, kIcw1Init | kIcw1Icw4);
    io::wait();
    io::out8(kSlaveCommand, kIcw1Init | kIcw1Icw4);
    io::wait();

    // ICW2: vector offset for each controller.
    io::out8(kMasterData, interrupts::kIrqBase);
    io::wait();
    io::out8(kSlaveData, interrupts::kIrqBase + 8);
    io::wait();

    // ICW3: how the two are wired to each other. The slave hangs off the
    // master's IRQ 2 line, which is why IRQ 2 is never a real device.
    io::out8(kMasterData, 1 << 2);
    io::wait();
    io::out8(kSlaveData, 2);
    io::wait();

    // ICW4: 8086 mode rather than the 8080 mode the chip still defaults to.
    io::out8(kMasterData, kIcw4Mode8086);
    io::wait();
    io::out8(kSlaveData, kIcw4Mode8086);
    io::wait();

    io::out8(kMasterData, master_mask);
    io::out8(kSlaveData, slave_mask);
}

void mask(u8 irq)
{
    const u16 port = irq < 8 ? kMasterData : kSlaveData;
    const u8 bit = static_cast<u8>(1 << (irq < 8 ? irq : irq - 8));
    io::out8(port, static_cast<u8>(io::in8(port) | bit));
}

void unmask(u8 irq)
{
    const u16 port = irq < 8 ? kMasterData : kSlaveData;
    const u8 bit = static_cast<u8>(1 << (irq < 8 ? irq : irq - 8));
    io::out8(port, static_cast<u8>(io::in8(port) & ~bit));

    // Anything on the slave is invisible to the CPU unless the cascade line
    // itself is open.
    if (irq >= 8)
        unmask(2);
}

void mask_all()
{
    io::out8(kMasterData, 0xFF);
    io::out8(kSlaveData, 0xFF);
}

void end_of_interrupt(u8 irq)
{
    // The slave cannot reach the CPU directly, so anything it raised has to be
    // acknowledged at both chips.
    if (irq >= 8)
        io::out8(kSlaveCommand, kEoi);
    io::out8(kMasterCommand, kEoi);
}

bool is_spurious(u8 irq)
{
    if (irq != 7 && irq != 15)
        return false;

    // A genuine interrupt sets its bit in the In-Service Register. A spurious
    // one does not, which is the only way to tell them apart. Both candidates
    // are line 7 of their own chip, hence the same bit either way.
    const u16 command = irq == 7 ? kMasterCommand : kSlaveCommand;
    constexpr u8 kLine7 = 1 << 7;
    return (read_isr(command) & kLine7) == 0;
}

void handle_spurious(u8 irq)
{
    // A spurious slave IRQ still reached the CPU through the master's cascade,
    // so the master - and only the master - is owed an acknowledgement.
    if (irq == 15)
        io::out8(kMasterCommand, kEoi);
}

} // namespace pic
