/* usbd - the xHCI host controller and the keyboards on it, in ring 3.
 *
 * The last real device driver in the kernel, and the one with the least excuse:
 * xHCI is rings in memory and a doorbell register, which is exactly the shape
 * the driver ABI was built for. Mapping the registers, allocating memory the
 * controller can reach, and knowing its physical address are all things a ring
 * 3 driver can ask for, so none of this needed ring 0 for anything except the
 * last step - putting a decoded keystroke where a blocked reader will find it.
 * That step stayed behind, as one system call, because the queue is what a
 * sleeping task is waiting on and waking it is the scheduler's business.
 *
 * The controller was polled from the timer tick before, which is why it had to
 * be in the kernel at all: nothing else ran often enough. Here it is a process
 * with a loop, which is the same thing said honestly.
 */

#include <driver.h>
#include <ipc.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

/* --- capability registers (at the start of the register window) ------------ */
#define CAP_LENGTH   0x00   /* u8; where the operational registers begin */
#define HCS_PARAMS1  0x04   /* slots 7:0, interrupters 18:8, ports 31:24 */
#define HCC_PARAMS1  0x10   /* AC64 bit 0, context size bit 2 */
#define DB_OFF       0x14
#define RTS_OFF      0x18

/* --- operational registers, relative to CAPLENGTH -------------------------- */
#define USB_CMD   0x00
#define USB_STS   0x04
#define CRCR      0x18      /* command ring control, 64-bit */
#define DCBAAP    0x30      /* device context base address array, 64-bit */
#define CONFIG    0x38
#define PORT_BASE 0x400     /* port n (1-based) at PORT_BASE + (n-1)*0x10 */

#define CMD_RUN   (1u << 0)
#define CMD_RESET (1u << 1)

#define STS_HALTED    (1u << 0)
#define STS_NOT_READY (1u << 11)

/* --- port status and control ----------------------------------------------- */
#define PORT_CONNECTED (1u << 0)
#define PORT_ENABLED   (1u << 1)
#define PORT_RESET     (1u << 4)
#define PORT_POWER     (1u << 9)
/* Change bits are write-1-to-clear and sit alongside bits that are not, so
 * every write has to mask them off or it clears things by accident. */
#define PORT_CHANGE_MASK ((1u<<17)|(1u<<18)|(1u<<20)|(1u<<21)|(1u<<22))
#define PORT_WRITE_MASK  (~(PORT_CHANGE_MASK | PORT_ENABLED))

/* --- TRB types -------------------------------------------------------------- */
#define TRB_NORMAL         1
#define TRB_SETUP_STAGE    2
#define TRB_DATA_STAGE     3
#define TRB_STATUS_STAGE   4
#define TRB_LINK           6
#define TRB_ENABLE_SLOT    9
#define TRB_ADDRESS_DEVICE 11
#define TRB_CONFIGURE_EP   12
#define TRB_TRANSFER_EVENT 32
#define TRB_COMMAND_DONE   33

#define TRB_CYCLE (1u << 0)
#define TRB_IOC   (1u << 5)   /* interrupt on completion */
#define TRB_IDT   (1u << 6)   /* immediate data (the setup packet) */

#define COMPLETION_SUCCESS 1

#define MAX_PORTS   32
#define MAX_DEVICES 8
#define RING_SIZE   64        /* TRBs per ring, including the link */

#define PCI_ADDRESS 0xCF8
#define PCI_DATA    0xCFC

struct trb {
    unsigned long parameter;
    unsigned status;
    unsigned control;
} __attribute__((packed));

struct ring {
    struct trb*   trbs;
    unsigned long phys;
    unsigned      enqueue;
    unsigned      cycle;
};

/* Speeds as the port status register reports them. */
#define SPEED_FULL  1
#define SPEED_LOW   2
#define SPEED_HIGH  3
#define SPEED_SUPER 4

struct device {
    int      present;
    unsigned char slot, port, speed;
    unsigned char device_class, device_subclass, device_protocol;
    unsigned short vendor, product;
    unsigned char interrupt_in;
    unsigned short interrupt_packet;
};

static volatile unsigned char* g_regs;
static volatile unsigned char* g_op;
static volatile unsigned char* g_run;
static volatile unsigned*      g_doorbells;

static unsigned g_max_ports;
static unsigned g_max_slots;
static unsigned g_context_size = 32;   /* 64 when HCCPARAMS1 bit 2 is set */

static unsigned long* g_dcbaa;
static unsigned long  g_dcbaa_phys;

static struct ring g_command;
static struct ring g_event;
static unsigned long g_erst_phys;
static unsigned g_event_dequeue;
static unsigned g_event_cycle = 1;

/* Per-slot state: its contexts, and the transfer rings for endpoints in use. */
struct slot {
    unsigned char* context;         /* the device context (output) */
    unsigned long  context_phys;
    unsigned char* input;           /* the input context, reused per command */
    unsigned long  input_phys;
    struct ring    rings[32];       /* indexed by endpoint context id (dci) */
    int            ring_ready[32];
};

static struct slot   g_slots[MAX_DEVICES + 1];
static struct device g_devices[MAX_DEVICES];
static unsigned      g_device_count;

/* One bounce buffer, reused. The kernel version allocated a fresh one for every
 * control transfer and never gave any of them back - harmless there because it
 * only ever ran during enumeration, but a process that keeps running cannot
 * leak physical memory per transfer. Control transfers are serialised through
 * one waiter anyway, so one buffer is all there is ever use for. */
static unsigned char* g_bounce;
static unsigned long  g_bounce_phys;

static unsigned read32(volatile unsigned char* base, unsigned off)
{
    return *(volatile unsigned*)(base + off);
}
static void write32(volatile unsigned char* base, unsigned off, unsigned v)
{
    *(volatile unsigned*)(base + off) = v;
}
static void write64(volatile unsigned char* base, unsigned off, unsigned long v)
{
    *(volatile unsigned long*)(base + off) = v;
}

static void* alloc_dma(unsigned long bytes, unsigned long* phys_out)
{
    void* virt = dma_alloc(bytes, phys_out);
    if (virt != 0)
        memset(virt, 0, bytes);
    return virt;
}

/* A ring ends with a Link TRB pointing back at its own start, which is what
 * makes it a ring rather than a list: the controller follows it round instead
 * of running off the end. */
static int init_ring(struct ring* r)
{
    struct trb* link;
    r->trbs = alloc_dma(RING_SIZE * sizeof(struct trb), &r->phys);
    if (r->trbs == 0)
        return 0;
    r->enqueue = 0;
    r->cycle = 1;

    link = &r->trbs[RING_SIZE - 1];
    link->parameter = r->phys;
    link->status = 0;
    /* Toggle Cycle: crossing the link flips the producer cycle bit, which is
     * how the controller tells a fresh TRB from a stale one on the next lap. */
    link->control = (TRB_LINK << 10) | (1u << 1);
    return 1;
}

static void ring_push(struct ring* r, unsigned long parameter, unsigned status,
                      unsigned control)
{
    struct trb* t = &r->trbs[r->enqueue];
    t->parameter = parameter;
    t->status = status;
    t->control = control | (r->cycle ? TRB_CYCLE : 0);

    ++r->enqueue;
    if (r->enqueue == RING_SIZE - 1) {
        struct trb* link = &r->trbs[RING_SIZE - 1];
        link->control = (link->control & ~TRB_CYCLE) | (r->cycle ? TRB_CYCLE : 0);
        r->enqueue = 0;
        r->cycle ^= 1;
    }
}

/* The device context index an endpoint address maps to. Endpoint 0 is 1; after
 * that each endpoint number has two slots, OUT then IN. Getting this wrong
 * points the controller at the wrong context and nothing transfers. */
static unsigned endpoint_dci(unsigned char address)
{
    const unsigned number = address & 0x0F;
    if (number == 0)
        return 1;
    return number * 2 + ((address & 0x80) != 0 ? 1 : 0);
}

static void ring_doorbell(unsigned char slot, unsigned target)
{
    g_doorbells[slot] = target;
}

/* An interrupt transfer that has been posted and not yet completed.
 *
 * A control transfer can be waited on, because the device answers immediately.
 * An interrupt endpoint cannot: a keyboard completes its transfer only when a
 * key changes, so waiting would block until someone typed. These are posted and
 * left, and the completion is picked up whenever the event ring is drained. */
struct pending {
    int            active, complete;
    unsigned char  endpoint;
    unsigned char* bounce;
    unsigned long  bounce_phys;
    unsigned       length, transferred;
};

static struct pending g_pending[MAX_DEVICES + 1];

/* Record a transfer event against a posted interrupt transfer. True when it
 * belonged to one, so a waiting caller knows to keep looking. */
static int claim_interrupt_event(unsigned char slot, unsigned endpoint_id,
                                 unsigned residual)
{
    struct pending* p;
    if (slot == 0 || slot > MAX_DEVICES)
        return 0;
    p = &g_pending[slot];
    if (!p->active || endpoint_dci(p->endpoint) != endpoint_id)
        return 0;
    p->transferred = p->length > residual ? p->length - residual : p->length;
    p->complete = 1;
    return 1;
}

static void advance_event_ring(void)
{
    ++g_event_dequeue;
    if (g_event_dequeue == RING_SIZE) {
        g_event_dequeue = 0;
        g_event_cycle ^= 1;
    }
    /* Tell the controller how far we have consumed, with the busy bit set. */
    write64(g_run, 0x38,
            (g_event.phys + g_event_dequeue * sizeof(struct trb)) | (1ul << 3));
}

/* Wait for the next event, returning its completion code. Polled rather than
 * interrupt-driven, like the NIC. */
static int wait_for_event(unsigned expected_type, unsigned* completion,
                          unsigned char* slot_out, unsigned* transferred)
{
    int spin;
    for (spin = 0; spin < 2000000; ++spin) {
        struct trb* t = &g_event.trbs[g_event_dequeue];
        unsigned type, endpoint_id;
        int claimed;

        if ((t->control & TRB_CYCLE) != g_event_cycle) {
            __asm__ volatile("pause");
            continue;                   /* not yet written by the controller */
        }

        type = (t->control >> 10) & 0x3F;
        endpoint_id = (t->control >> 16) & 0x1F;
        *completion = (t->status >> 24) & 0xFF;
        *slot_out = (unsigned char)((t->control >> 24) & 0xFF);
        *transferred = t->status & 0x1FFFFF;

        /* A completion belonging to a posted interrupt transfer is filed away
         * rather than handed to whoever happens to be waiting - the event ring
         * is shared, so without this a control transfer would consume the
         * keyboard's completion and call it its own. */
        claimed = type == TRB_TRANSFER_EVENT &&
                  claim_interrupt_event(*slot_out, endpoint_id, *transferred);

        advance_event_ring();

        if (type == expected_type && !claimed)
            return 1;
        /* Port status changes arrive unbidden, and claimed completions belong
         * to someone else; skip both and keep looking. */
    }
    return 0;
}

/* Drain whatever the controller has posted without waiting for anything in
 * particular, filing interrupt completions as they go past. */
static void drain_events(void)
{
    int i;
    for (i = 0; i < 64; ++i) {
        struct trb* t = &g_event.trbs[g_event_dequeue];
        unsigned type, endpoint_id, residual;
        unsigned char slot;

        if ((t->control & TRB_CYCLE) != g_event_cycle)
            return;

        type = (t->control >> 10) & 0x3F;
        endpoint_id = (t->control >> 16) & 0x1F;
        slot = (unsigned char)((t->control >> 24) & 0xFF);
        residual = t->status & 0x1FFFFF;
        if (type == TRB_TRANSFER_EVENT)
            claim_interrupt_event(slot, endpoint_id, residual);

        advance_event_ring();
    }
}

/* Issue a command on the command ring and wait for its completion event. */
static int run_command(unsigned long parameter, unsigned control,
                       unsigned char* slot_out)
{
    unsigned completion = 0, transferred = 0;
    ring_push(&g_command, parameter, 0, control | TRB_IOC);
    ring_doorbell(0, 0);
    if (!wait_for_event(TRB_COMMAND_DONE, &completion, slot_out, &transferred))
        return 0;
    return completion == COMPLETION_SUCCESS;
}

static volatile unsigned char* port_regs(unsigned port)   /* 1-based */
{
    return g_op + PORT_BASE + (port - 1) * 0x10;
}

static unsigned* context_at(unsigned char* base, unsigned index)
{
    return (unsigned*)(base + index * g_context_size);
}

static unsigned short default_packet_size(unsigned speed)
{
    if (speed == SPEED_LOW)   return 8;
    if (speed == SPEED_SUPER) return 512;
    return 64;                              /* full and high speed */
}

/* Reset a port and wait for the controller to enable it. USB 3 ports train
 * themselves on connect, but a USB 2 port stays disabled until asked. */
static int reset_port(unsigned port)
{
    volatile unsigned char* regs = port_regs(port);
    unsigned status = read32(regs, 0);
    int i;

    if ((status & PORT_ENABLED) != 0)
        return 1;

    write32(regs, 0, (status & PORT_WRITE_MASK) | PORT_RESET);
    for (i = 0; i < 1000; ++i) {
        msleep(1);
        status = read32(regs, 0);
        if ((status & PORT_RESET) == 0 && (status & PORT_ENABLED) != 0) {
            /* Acknowledge the change bits so the next event is a real one. */
            write32(regs, 0,
                    (status & PORT_WRITE_MASK) | (status & PORT_CHANGE_MASK));
            return 1;
        }
    }
    return 0;
}

/* Give a slot its contexts and tell the controller where the device lives. */
static int address_device(unsigned char slot_id, unsigned port, unsigned speed)
{
    struct slot* s = &g_slots[slot_id];
    unsigned *control, *slot_ctx, *ep0;
    unsigned long dequeue;
    unsigned char completed = 0;

    s->context = alloc_dma(g_context_size * 32, &s->context_phys);
    s->input   = alloc_dma(g_context_size * 33, &s->input_phys);
    if (s->context == 0 || s->input == 0)
        return 0;
    if (!init_ring(&s->rings[1]))
        return 0;
    s->ring_ready[1] = 1;

    g_dcbaa[slot_id] = s->context_phys;

    /* Input control context: add the slot context and endpoint 0. */
    control = context_at(s->input, 0);
    control[0] = 0;                         /* drop nothing */
    control[1] = (1u << 0) | (1u << 1);     /* add slot + ep0 */

    slot_ctx = context_at(s->input, 1);
    slot_ctx[0] = (speed << 20) | (1u << 27);   /* 1 context entry */
    slot_ctx[1] = port << 16;

    ep0 = context_at(s->input, 2);
    ep0[1] = (3u << 1)                          /* CErr = 3 */
           | (4u << 3)                          /* EP type: control */
           | ((unsigned)default_packet_size(speed) << 16);
    dequeue = s->rings[1].phys | 1;             /* dequeue cycle state */
    ep0[2] = (unsigned)dequeue;
    ep0[3] = (unsigned)(dequeue >> 32);
    ep0[4] = 8;                                 /* average TRB length */

    return run_command(s->input_phys,
                       (TRB_ADDRESS_DEVICE << 10) | ((unsigned)slot_id << 24),
                       &completed);
}

static long control_transfer(unsigned char slot_id, unsigned char request_type,
                             unsigned char request, unsigned short value,
                             unsigned short index, void* data,
                             unsigned short length)
{
    struct slot* s;
    struct ring* r;
    unsigned long setup;
    unsigned in, transfer_type, completion = 0, residual = 0, moved;
    unsigned char event_slot = 0;

    if (slot_id == 0 || slot_id > MAX_DEVICES)
        return -1;
    s = &g_slots[slot_id];
    if (!s->ring_ready[1])
        return -1;
    r = &s->rings[1];

    if (length > 4096)
        return -1;
    if (length > 0 && (request_type & 0x80) == 0 && data != 0)
        memcpy(g_bounce, data, length);

    /* The setup packet travels inside the TRB itself rather than through a
     * buffer, which is what the immediate-data bit means. */
    setup = (unsigned long)request_type |
            (unsigned long)request << 8 |
            (unsigned long)value << 16 |
            (unsigned long)index << 32 |
            (unsigned long)length << 48;
    in = (request_type & 0x80) != 0 ? 1 : 0;
    transfer_type = length == 0 ? 0u : (in ? 3u : 2u);

    ring_push(r, setup, 8,
              TRB_IDT | (transfer_type << 16) | (TRB_SETUP_STAGE << 10));
    if (length > 0)
        ring_push(r, g_bounce_phys, length,
                  (in << 16) | (TRB_DATA_STAGE << 10));
    /* The status stage runs opposite to the data stage. */
    ring_push(r, 0, 0,
              ((in ? 0u : 1u) << 16) | (TRB_STATUS_STAGE << 10) | TRB_IOC);

    ring_doorbell(slot_id, 1);

    if (!wait_for_event(TRB_TRANSFER_EVENT, &completion, &event_slot, &residual))
        return -1;
    if (completion != COMPLETION_SUCCESS && completion != 13)   /* 13 = short */
        return -1;

    moved = length > residual ? length - residual : length;
    if (in && data != 0)
        memcpy(data, g_bounce, moved);
    return (long)moved;
}

/* Walk a configuration descriptor and set up every endpoint it declares. One
 * Configure Endpoint command adds them all at once. */
static int configure_endpoints(unsigned char slot_id, const unsigned char* config,
                               unsigned short length)
{
    struct slot* s;
    unsigned* control;
    unsigned* slot_ctx;
    const unsigned* live;
    unsigned max_dci = 1;
    unsigned short offset = 0;
    unsigned char completed = 0;

    if (slot_id == 0 || slot_id > MAX_DEVICES || length < 9)
        return 0;
    s = &g_slots[slot_id];

    memset(s->input, 0, g_context_size * 33);
    control = context_at(s->input, 0);
    control[0] = 0;
    control[1] = 1;                              /* the slot context always */

    /* Descriptors are a chain of {length, type, ...} records; endpoint records
     * are type 5. */
    while (offset + 2 <= length) {
        const unsigned char item_length = config[offset];
        const unsigned char item_type   = config[offset + 1];
        if (item_length < 2)
            break;

        if (item_type == 5 && offset + 6 <= length) {      /* endpoint */
            const unsigned char address    = config[offset + 2];
            const unsigned char attributes = config[offset + 3];
            const unsigned short packet =
                (unsigned short)(config[offset + 4] | config[offset + 5] << 8);
            const unsigned char interval = item_length > 6 ? config[offset + 6] : 0;
            const unsigned dci = endpoint_dci(address);

            if (dci < 32 && !s->ring_ready[dci] && init_ring(&s->rings[dci])) {
                unsigned* ep;
                unsigned long dequeue;
                /* xHCI numbers endpoint types with direction folded in: OUT
                 * takes the raw transfer type, IN the same plus four. */
                const unsigned tr = attributes & 0x03;
                const unsigned type = (address & 0x80) != 0 ? tr + 4 : tr;

                s->ring_ready[dci] = 1;
                ep = context_at(s->input, dci + 1);
                ep[0] = (unsigned)interval << 16;
                ep[1] = (3u << 1) | (type << 3) |
                        ((unsigned)(packet & 0x7FF) << 16);
                dequeue = s->rings[dci].phys | 1;
                ep[2] = (unsigned)dequeue;
                ep[3] = (unsigned)(dequeue >> 32);
                ep[4] = packet;

                control[1] |= 1u << dci;
                if (dci > max_dci)
                    max_dci = dci;
            }
        }
        offset += item_length;
    }

    /* The slot context has to say how many endpoint contexts follow, or the
     * controller ignores the ones past its count. */
    slot_ctx = context_at(s->input, 1);
    live = context_at(s->context, 0);
    slot_ctx[0] = (live[0] & 0x07FFFFFF) | (max_dci << 27);
    slot_ctx[1] = live[1];

    return run_command(s->input_phys,
                       (TRB_CONFIGURE_EP << 10) | ((unsigned)slot_id << 24),
                       &completed);
}

static int submit_interrupt(unsigned char slot_id, unsigned char endpoint,
                            unsigned length)
{
    struct slot* s;
    struct pending* p;
    unsigned dci;

    if (slot_id == 0 || slot_id > MAX_DEVICES || length == 0 || length > 64)
        return 0;
    s = &g_slots[slot_id];
    dci = endpoint_dci(endpoint);
    if (dci >= 32 || !s->ring_ready[dci])
        return 0;

    p = &g_pending[slot_id];
    if (p->active)
        return 0;                        /* one in flight per device is enough */

    if (p->bounce == 0) {
        p->bounce = alloc_dma(64, &p->bounce_phys);
        if (p->bounce == 0)
            return 0;
    }

    p->endpoint = endpoint;
    p->length = length;
    p->transferred = 0;
    p->complete = 0;
    p->active = 1;

    ring_push(&s->rings[dci], p->bounce_phys, length,
              (TRB_NORMAL << 10) | TRB_IOC);
    ring_doorbell(slot_id, dci);
    return 1;
}

static long take_interrupt(unsigned char slot_id, void* data, unsigned length)
{
    struct pending* p;
    unsigned moved;

    if (slot_id == 0 || slot_id > MAX_DEVICES)
        return -1;
    p = &g_pending[slot_id];
    if (!p->active)
        return -1;

    drain_events();
    if (!p->complete)
        return 0;                        /* nothing yet, which is not an error */

    moved = p->transferred < length ? p->transferred : length;
    if (data != 0)
        memcpy(data, p->bounce, moved);
    p->active = 0;
    p->complete = 0;
    return (long)moved;
}

/* --- PCI, through the ports the kernel granted -------------------------------- */

static unsigned cfg_read(unsigned bus, unsigned slot, unsigned fn, unsigned off)
{
    outl(PCI_ADDRESS, 0x80000000u | bus << 16 | slot << 11 | fn << 8 | (off & 0xFC));
    return inl(PCI_DATA);
}

static void cfg_write(unsigned bus, unsigned slot, unsigned fn, unsigned off,
                      unsigned value)
{
    outl(PCI_ADDRESS, 0x80000000u | bus << 16 | slot << 11 | fn << 8 | (off & 0xFC));
    outl(PCI_DATA, value);
}

static int bring_up(void)
{
    unsigned b, d, f;
    unsigned bus = 0, dev = 0, fn = 0, found = 0;
    unsigned long bar;
    unsigned bar_lo, bar_hi = 0;
    unsigned char cap_length;
    unsigned hcs1, i;
    unsigned long* erst;

    if (io_permit(PCI_ADDRESS, 8) != 0)
        return -1;

    /* xHCI is class 0x0C (serial bus), subclass 0x03 (USB), programming
     * interface 0x30. The older host controllers share the class and subclass,
     * so the programming interface is what distinguishes them. */
    for (b = 0; b < 4 && !found; ++b)
        for (d = 0; d < 32 && !found; ++d)
            for (f = 0; f < 8; ++f) {
                unsigned cls;
                if ((cfg_read(b, d, f, 0) & 0xFFFF) == 0xFFFF)
                    continue;
                cls = cfg_read(b, d, f, 0x08);
                if ((cls >> 8) == 0x0C0330) {
                    bus = b; dev = d; fn = f; found = 1;
                    break;
                }
            }
    if (!found)
        return -1;

    {
        unsigned command = cfg_read(bus, dev, fn, 0x04);
        command |= (1 << 1) | (1 << 2);          /* memory space, bus master */
        cfg_write(bus, dev, fn, 0x04, command);
    }

    bar_lo = cfg_read(bus, dev, fn, 0x10);
    if ((bar_lo & 1) != 0)                       /* an I/O BAR is not xHCI */
        return -1;
    if ((bar_lo & 0x06) == 0x04)                 /* 64-bit BAR */
        bar_hi = cfg_read(bus, dev, fn, 0x14);
    bar = ((unsigned long)bar_hi << 32) | (bar_lo & ~0xFul);
    if (bar == 0)
        return -1;

    g_regs = (volatile unsigned char*)map_physical(bar, 0x10000);
    if (g_regs == 0)
        return -1;

    cap_length = *(volatile unsigned char*)(g_regs + CAP_LENGTH);
    g_op  = g_regs + cap_length;
    g_run = g_regs + (read32(g_regs, RTS_OFF) & ~0x1Fu);
    g_doorbells = (volatile unsigned*)(g_regs + (read32(g_regs, DB_OFF) & ~0x3u));

    hcs1 = read32(g_regs, HCS_PARAMS1);
    g_max_slots = hcs1 & 0xFF;
    g_max_ports = (hcs1 >> 24) & 0xFF;
    if (g_max_ports > MAX_PORTS)
        g_max_ports = MAX_PORTS;
    /* A context is 32 bytes unless HCCPARAMS1 says otherwise; getting this
     * wrong misaligns every endpoint context in the device context. */
    g_context_size = (read32(g_regs, HCC_PARAMS1) & (1u << 2)) != 0 ? 64 : 32;

    /* Stop the controller, then reset it and wait for it to say it is ready. */
    write32(g_op, USB_CMD, read32(g_op, USB_CMD) & ~CMD_RUN);
    for (i = 0; i < 1000000 && (read32(g_op, USB_STS) & STS_HALTED) == 0; ++i)
        __asm__ volatile("pause");
    write32(g_op, USB_CMD, read32(g_op, USB_CMD) | CMD_RESET);
    for (i = 0; i < 1000000 && (read32(g_op, USB_CMD) & CMD_RESET) != 0; ++i)
        __asm__ volatile("pause");
    for (i = 0; i < 1000000 && (read32(g_op, USB_STS) & STS_NOT_READY) != 0; ++i)
        __asm__ volatile("pause");

    g_bounce = alloc_dma(4096, &g_bounce_phys);
    if (g_bounce == 0)
        return -1;

    /* The device context base address array: one physical pointer per slot,
     * which the controller reads to find each device's context. */
    g_dcbaa = alloc_dma((g_max_slots + 1) * sizeof(unsigned long), &g_dcbaa_phys);
    if (g_dcbaa == 0)
        return -1;
    write32(g_op, CONFIG, g_max_slots);
    write64(g_op, DCBAAP, g_dcbaa_phys);

    if (!init_ring(&g_command))
        return -1;
    write64(g_op, CRCR, g_command.phys | 1);    /* ring cycle state = 1 */

    /* The event ring is described by a segment table rather than pointed at
     * directly, so a controller can be given several discontiguous segments. */
    if (!init_ring(&g_event))
        return -1;
    g_event.cycle = 1;
    g_event_cycle = 1;
    g_event_dequeue = 0;

    erst = alloc_dma(16, &g_erst_phys);
    if (erst == 0)
        return -1;
    erst[0] = g_event.phys;
    erst[1] = RING_SIZE;                        /* entries in this segment */

    write32(g_run, 0x28, 1);                    /* ERSTSZ: one segment */
    write64(g_run, 0x38, g_event.phys);         /* ERDP */
    write64(g_run, 0x30, g_erst_phys);          /* ERSTBA, written last */

    write32(g_op, USB_CMD, read32(g_op, USB_CMD) | CMD_RUN);
    for (i = 0; i < 1000000 && (read32(g_op, USB_STS) & STS_HALTED) != 0; ++i)
        __asm__ volatile("pause");
    if ((read32(g_op, USB_STS) & STS_HALTED) != 0)
        return -1;

    return 0;
}

/* Walk the root ports. Anything connected gets reset, given a slot, and asked
 * what it is. */
static void enumerate(void)
{
    unsigned port;
    for (port = 1; port <= g_max_ports && g_device_count < MAX_DEVICES; ++port) {
        volatile unsigned char* regs = port_regs(port);
        unsigned status = read32(regs, 0);
        unsigned speed;
        unsigned char slot_id = 0;
        unsigned char descriptor[18];
        unsigned char config[256];
        struct device* d;
        long got;

        /* Power the port if it is not already; a port with no power reports
         * nothing connected however hard it is asked. */
        if ((status & PORT_POWER) == 0) {
            write32(regs, 0, (status & PORT_WRITE_MASK) | PORT_POWER);
            msleep(20);
            status = read32(regs, 0);
        }
        if ((status & PORT_CONNECTED) == 0)
            continue;
        if (!reset_port(port))
            continue;

        status = read32(regs, 0);
        speed = (status >> 10) & 0x0F;

        if (!run_command(0, TRB_ENABLE_SLOT << 10, &slot_id) || slot_id == 0 ||
            slot_id > MAX_DEVICES)
            continue;
        if (!address_device(slot_id, port, speed))
            continue;

        /* GET_DESCRIPTOR(device): 0x80 is device-to-host, and descriptor type 1
         * in the high byte of wValue is the device descriptor. */
        memset(descriptor, 0, sizeof(descriptor));
        if (control_transfer(slot_id, 0x80, 6, 0x0100, 0, descriptor,
                             sizeof(descriptor)) < 8)
            continue;

        d = &g_devices[g_device_count++];
        memset(d, 0, sizeof(*d));
        d->present = 1;
        d->slot = slot_id;
        d->port = (unsigned char)port;
        d->speed = (unsigned char)speed;
        d->device_class    = descriptor[4];
        d->device_subclass = descriptor[5];
        d->device_protocol = descriptor[6];
        d->vendor  = (unsigned short)(descriptor[8]  | descriptor[9]  << 8);
        d->product = (unsigned short)(descriptor[10] | descriptor[11] << 8);

        /* A device descriptor of class 0 means "look at the interface", which
         * is the usual case - so read the configuration and take the class from
         * the first interface it declares. */
        memset(config, 0, sizeof(config));
        got = control_transfer(slot_id, 0x80, 6, 0x0200, 0, config, sizeof(config));
        if (got >= 9) {
            const unsigned short total =
                (unsigned short)(config[2] | config[3] << 8);
            const unsigned short have =
                (unsigned short)(got < total ? got : total);
            unsigned short o;
            for (o = 0; o + 2 <= have; o += config[o] == 0 ? 2 : config[o]) {
                if (config[o + 1] == 4 && o + 8 <= have) {       /* interface */
                    if (d->device_class == 0) {
                        d->device_class    = config[o + 5];
                        d->device_subclass = config[o + 6];
                        d->device_protocol = config[o + 7];
                    }
                } else if (config[o + 1] == 5 && o + 6 <= have) { /* endpoint */
                    const unsigned char address = config[o + 2];
                    const unsigned char kind = config[o + 3] & 0x03;
                    if (kind == 3 && (address & 0x80) != 0) {
                        d->interrupt_in = address;
                        d->interrupt_packet =
                            (unsigned short)(config[o + 4] | config[o + 5] << 8);
                    }
                }
            }
            /* SET_CONFIGURATION, then hand the endpoints to the controller. */
            control_transfer(slot_id, 0x00, 9, config[5], 0, 0, 0);
            configure_endpoints(slot_id, config, have);
        }
    }
}

/* --- the HID boot keyboard --------------------------------------------------- */

#define MAX_KEYBOARDS 2

struct keyboard {
    unsigned char slot, endpoint;
    unsigned char previous[6];   /* keys held last report, to find new presses */
};

static struct keyboard g_keyboards[MAX_KEYBOARDS];
static unsigned g_keyboard_count;

/* HID usage codes for the boot keyboard, unshifted and shifted. The table is
 * indexed by usage id, which starts at 4 for 'a' - the layout is the standard
 * one every boot-protocol keyboard reports, whatever the physical keys say. */
static const char kUnshifted[] =
    "\0\0\0\0" "abcdefghijklmnopqrstuvwxyz" "1234567890" "\n\x1b\b\t "
    "-=[]\\\0;'`,./";
static const char kShifted[] =
    "\0\0\0\0" "ABCDEFGHIJKLMNOPQRSTUVWXYZ" "!@#$%^&*()" "\n\x1b\b\t "
    "_+{}|\0:\"~<>?";

/* The four arrows, which live well above the printable range and so were not
 * in the tables at all. The values are the ones the PS/2 decoder produces for
 * the same keys - the two keyboards have to be indistinguishable by the time
 * anything reads them, or every program would need to know which is plugged
 * in. */
#define USAGE_RIGHT 0x4F
#define USAGE_LEFT  0x50
#define USAGE_DOWN  0x51
#define USAGE_UP    0x52
#define USAGE_CAPS  0x39

#define KEY_UP    0x1C
#define KEY_DOWN  0x1D
#define KEY_LEFT  0x1E
#define KEY_RIGHT 0x1F

/* Mirrors keyboard::kModShift / kModCtrl in the kernel. */
#define MOD_SHIFT 1u
#define MOD_CTRL  2u

static int g_caps;

static char translate(unsigned char usage, int shift, int ctrl)
{
    const char* table;
    unsigned long length;
    char c;

    switch (usage) {
    case USAGE_UP:    return KEY_UP;
    case USAGE_DOWN:  return KEY_DOWN;
    case USAGE_LEFT:  return KEY_LEFT;
    case USAGE_RIGHT: return KEY_RIGHT;
    default: break;
    }

    table = shift ? kShifted : kUnshifted;
    length = shift ? sizeof(kShifted) : sizeof(kUnshifted);
    if (usage >= length - 1)
        return 0;
    c = table[usage];
    if (c == 0)
        return 0;

    if (g_caps && !shift && c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    else if (g_caps && shift && c >= 'A' && c <= 'Z')
        c = (char)(c - 'A' + 'a');

    /* Ctrl folds a letter down to its control code, the way a terminal has
     * always done it: Ctrl-C is 3 because C is the third letter. */
    if (ctrl) {
        if (c >= 'a' && c <= 'z')
            return (char)(c - 'a' + 1);
        if (c >= 'A' && c <= 'Z')
            return (char)(c - 'A' + 1);
        return 0;
    }
    return c;
}

static void post_char(char c)
{
    __syscall(SYS_inputpost, 0, (long)(unsigned char)c, 0, 0, 0);
}

static void post_modifiers(unsigned mods)
{
    __syscall(SYS_inputpost, 1, (long)mods, 0, 0, 0);
}

static void find_keyboards(void)
{
    unsigned i;
    for (i = 0; i < g_device_count && g_keyboard_count < MAX_KEYBOARDS; ++i) {
        struct device* d = &g_devices[i];
        /* Class 3 is HID, subclass 1 is "boot interface", protocol 1 is
         * keyboard. Boot protocol is the point: it reports a fixed eight-byte
         * layout, so no report descriptor has to be parsed to read it. */
        if (d->device_class != 3 || d->device_subclass != 1 ||
            d->device_protocol != 1 || d->interrupt_in == 0)
            continue;

        /* SET_PROTOCOL(boot). Class request to the interface: 0x21 out, request
         * 11, value 0 for boot. A keyboard powers up in report protocol and
         * would otherwise send something this does not know how to read. */
        control_transfer(d->slot, 0x21, 11, 0, 0, 0, 0);

        g_keyboards[g_keyboard_count].slot = d->slot;
        g_keyboards[g_keyboard_count].endpoint = d->interrupt_in;
        memset(g_keyboards[g_keyboard_count].previous, 0, 6);
        ++g_keyboard_count;

        submit_interrupt(d->slot, d->interrupt_in, 8);
    }
}

/* Read whatever the keyboards have reported and turn it into characters.
 *
 * A boot report is eight bytes: modifiers, a reserved byte, then up to six
 * usage codes for the keys currently held. It says what is down, not what
 * changed - so a key that was down last time and is still down is not a new
 * press, and comparing against the previous report is what makes auto-repeat
 * the host's decision rather than a stream of duplicates. */
static void poll_keyboards(void)
{
    unsigned k;
    for (k = 0; k < g_keyboard_count; ++k) {
        struct keyboard* kb = &g_keyboards[k];
        unsigned char report[8];
        long got = take_interrupt(kb->slot, report, sizeof(report));
        int shift, ctrl;
        unsigned i;

        if (got <= 0) {
            if (got < 0)                 /* nothing posted; put one up again */
                submit_interrupt(kb->slot, kb->endpoint, 8);
            continue;
        }
        if (got < 8) {
            submit_interrupt(kb->slot, kb->endpoint, 8);
            continue;
        }

        /* Left and right of each modifier: shift is bits 1 and 5, control is
         * bits 0 and 4. Only checking the left-hand one is why the right-hand
         * Ctrl and Shift used to do nothing. */
        shift = (report[0] & 0x22) != 0;
        ctrl  = (report[0] & 0x11) != 0;

        /* Published so that a program asking "is shift held" gets the same
         * answer for a USB keyboard as for a PS/2 one. */
        post_modifiers((shift ? MOD_SHIFT : 0u) | (ctrl ? MOD_CTRL : 0u));

        for (i = 2; i < 8; ++i) {
            const unsigned char usage = report[i];
            int held = 0;
            unsigned j;
            if (usage == 0)
                continue;
            for (j = 0; j < 6; ++j)
                if (kb->previous[j] == usage)
                    held = 1;
            if (held)
                continue;                /* still down, not pressed again */

            if (usage == USAGE_CAPS) {
                g_caps = !g_caps;
                continue;
            }
            {
                const char c = translate(usage, shift, ctrl);
                if (c != 0)
                    post_char(c);
            }
        }
        memcpy(kb->previous, report + 2, 6);
        submit_interrupt(kb->slot, kb->endpoint, 8);
    }
}

int main(void)
{
    int port;
    unsigned i;

    if (bring_up() != 0) {
        printf("usbd: no xHCI controller\n");
        return 1;
    }
    enumerate();
    find_keyboards();

    port = port_create(IPC_PORT_USB);
    if (port < 0) {
        printf("usbd: something already answers for USB\n");
        return 1;
    }

    printf("usbd[%d]: xHCI, %u device(s), %u keyboard(s), in ring 3\n",
           getpid(), g_device_count, g_keyboard_count);
    for (i = 0; i < g_device_count; ++i)
        printf("    usb%u  port %u  %04x:%04x  class %02x\n", i,
               g_devices[i].port, g_devices[i].vendor, g_devices[i].product,
               g_devices[i].device_class);

    /* The tick used to drive this, which is the only reason it had to live in
     * the kernel. A millisecond between polls is far below what anyone types
     * at and far above what busy-waiting would cost. */
    for (;;) {
        struct ipc_message m, r;
        unsigned from = 0;
        int handle;

        poll_keyboards();

        handle = ipc_try_recv(port, &m, &from);
        if (handle >= 0) {
            memset(&r, 0, sizeof(r));
            r.tag = m.tag;
            r.word[0] = g_device_count;
            ipc_reply(handle, &r);
        }
        msleep(1);
    }
}
