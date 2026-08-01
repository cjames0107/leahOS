#pragma once

#include <leah/types.hpp>

namespace interrupts { struct Frame; }

// The system call interface, and the machinery to run a program in ring 3.
//
// Calls come in through the SYSCALL instruction: number in RAX, arguments in
// RDI, RSI, RDX, R10, R8, R9 - the SysV order with R10 standing in for RCX,
// which SYSCALL destroys. The return value comes back in RAX.

namespace syscall {

// leahOS's own numbers, not Linux's; there is no compatibility to preserve yet.
enum Number : u64 {
    Exit    = 0,
    Write   = 1,
    Read    = 2,
    GetPid  = 3,
    Fork    = 4,
    Execve  = 5,
    Wait    = 6,
    Yield   = 7,
    Open    = 8,
    Close   = 9,
    Lseek   = 10,
    Stat    = 11,
    Getdents = 12,
    Chdir   = 13,
    Getcwd  = 14,
    Mkdir   = 15,
    Unlink  = 16,
    Pipe    = 17,
    Dup2    = 18,
    Sbrk    = 19,
    Rename  = 20,
    // 21-24 and 39-41 were the network: netinfo, ping, arp, resolve, connect,
    // send and receive. The stack is a process now and is reached by message
    // rather than by trap. The numbers are left as a gap rather than reused -
    // a stale binary calling a recycled number is a bug that looks like
    // anything except that.
    Mmap    = 25,
    Munmap  = 26,
    Clone   = 27,
    Gettid  = 28,
    Kill      = 29,
    Signal    = 30,
    Sigreturn = 31,
    Getuid    = 32,
    Setuid    = 33,
    Getgid    = 34,
    Setgid    = 35,
    Chmod     = 36,
    Chown     = 37,
    Futex     = 38,
    Login     = 42,   // authenticate and switch credentials
    SetEcho   = 43,   // console echo, off while a password is typed
    UserName  = 44,
    UserAdd   = 45,
    SetPasswd = 46,
    // Shared memory: the first thing two processes can both write to.
    ShmOpen    = 47,   // open or create a segment by key
    ShmMap     = 48,   // map a segment into this process
    ShmSize    = 49,

    // What a window server needs in order to live outside the kernel: the
    // screen, and input before the console has cooked it. Root only - these
    // hand over the display and the keyboard wholesale.
    FbInfo     = 50,   // geometry and pitch
    FbMap      = 51,   // map the linear framebuffer
    InputPoll  = 52,   // mouse position and buttons, and one key at a time
    FbFont     = 53,   // the 8x16 BIOS font, so a server can draw its own text
    Sleep      = 54,   // block for a number of milliseconds
    ShmDestroy = 55,
    // End the calling thread only. Exit ends the whole process, which is the
    // distinction POSIX draws between exit and exit_group - and without it a
    // process cannot end while one of its threads is blocked in a syscall.
    ThreadExit = 56,
    ProcList   = 57,   // snapshot of the task table
    MemInfo    = 58,   // physical memory: used, free, usable
    CpuInfo    = 59,   // per-processor slice counts

    // Audio. Queueing samples and setting the volume are separate calls
    // because they are asked at completely different rates: one every few
    // milliseconds while something is playing, one when a person moves a
    // slider.
    AudioPlay   = 60,  // queue interleaved 16-bit stereo samples
    AudioSpace  = 61,  // how many samples would be taken right now
    AudioVolume = 62,  // rdi < 0 reads, otherwise sets 0..100
    AudioStop   = 63,  // drop what is queued and silence the output
    AudioFlush  = 64,  // hand over a part-filled buffer
    AudioInfo   = 65,  // what the output device is, for an about page

    // Message passing between address spaces. The primitive the drivers, the
    // filesystem and the network stack are being moved out onto: a server owns
    // a port, a client calls it, and neither can see the other's memory.
    PortCreate  = 66,
    PortOpen    = 67,
    PortDestroy = 68,
    IpcCall     = 69,  // send and block for the answer
    IpcRecv     = 70,  // block for a request; returns a handle to answer with
    IpcReply    = 71,

    // What a driver in ring 3 needs and an ordinary program must not have.
    // Deliberately four separate grants rather than one "make me a driver":
    // a sound driver that asks for a mixer's ports has no business mapping a
    // disk controller's registers, and the narrower the grant the more true
    // that stays.
    IoPermit    = 72,  // let this task use a range of I/O ports
    MapPhysical = 73,  // map a device's registers into this address space
    DmaAlloc    = 74,  // physically contiguous memory, and its physical address
    IrqListen   = 75,  // claim an interrupt line
    IrqWait     = 76,
    IpcTryRecv  = 77,  // recv without blocking  // block until it fires
};

// mmap protection and flags, mirrored in user/libc/include/sys/mman.h.
constexpr u64 kProtRead  = 1;
constexpr u64 kProtWrite = 2;
constexpr u64 kProtExec  = 4;
constexpr u64 kMapPrivate   = 0x02;
constexpr u64 kMapAnonymous = 0x20;
constexpr u64 kMapFixed     = 0x10;

// futex operations, mirrored in user/libc/include/thread.h.
constexpr u64 kFutexWait = 0;
constexpr u64 kFutexWake = 1;

// The register state a syscall handler sees. Field order is a contract with
// syscall_entry in syscall_entry.asm - do not reorder without editing both.
struct [[gnu::packed]] Frame {
    u64 r15, r14, r13, r12, rbp, rbx;    // callee-saved
    u64 r11, r10, r9, r8;
    u64 rdx, rsi, rdi;
    u64 rax;                             // number on entry, return value on exit
    u64 user_rip;                        // RCX at entry: where SYSCALL came from
    u64 user_flags;                      // R11 at entry
    u64 user_rsp;
};

// Selectors a user process runs with, RPL 3. Used to build the register frame
// a new or forked process resumes on.
constexpr u64 kUserCode = 0x20 | 3;
constexpr u64 kUserData = 0x18 | 3;
constexpr u64 kUserFlags = 0x202;       // IF set

void init();

// The per-processor half of init(): the SYSCALL MSRs, which every CPU that
// runs user code needs for its own.
void init_this_cpu();


// Deliver a pending signal to a task interrupted in ring 3 by a hardware IRQ,
// rewriting the frame the ISR is about to IRETQ through. Called from the
// interrupt dispatcher once the IRQ has been acknowledged.
void deliver_on_interrupt(interrupts::Frame& frame);

} // namespace syscall
