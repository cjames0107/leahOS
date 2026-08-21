/* ps2d - the 8042, its keyboard and its mouse, in ring 3.
 *
 * Two devices behind one controller and two interrupt lines, which is why this
 * is one process rather than two: every byte from either arrives at port 0x60,
 * and the only way to tell them apart is a bit in the status register read in
 * the same breath. Two drivers racing for that would each consume the other's
 * bytes.
 *
 * So one thread waits on each line and both funnel into one drain, under a
 * mutex. What the drain does is decide, per byte, whose it was - and that
 * decision has to be made by whoever took the byte, because reading the port
 * is what clears it.
 *
 * The kernel keeps only the queue a blocked reader sleeps on. It gets decoded
 * characters through the same call usbd uses; there is no privileged producer
 * any more, just two ordinary ones.
 */

#include <display.h>
#include <driver.h>
#include <ipc.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <thread.h>
#include <unistd.h>

#define DATA_PORT    0x60
#define STATUS_PORT  0x64
#define COMMAND_PORT 0x64

#define STATUS_OUTPUT_FULL 0x01
#define STATUS_INPUT_FULL  0x02
#define STATUS_FROM_MOUSE  0x20

#define CMD_DISABLE_PORT1 0xAD
#define CMD_DISABLE_PORT2 0xA7
#define CMD_ENABLE_PORT1  0xAE
#define CMD_ENABLE_PORT2  0xA8
#define CMD_READ_CONFIG   0x20
#define CMD_WRITE_CONFIG  0x60
#define CMD_WRITE_PORT2   0xD4

#define CFG_PORT1_IRQ    0x01
#define CFG_PORT2_IRQ    0x02
#define CFG_PORT1_CLOCK  0x10
#define CFG_TRANSLATION  0x40

#define KBD_ENABLE_SCANNING 0xF4
#define MOUSE_SET_DEFAULTS  0xF6
#define MOUSE_ENABLE        0xF4
#define MOUSE_SET_SAMPLE_RATE 0xF3
#define MOUSE_GET_DEVICE_ID 0xF2
#define ACK                 0xFA

#define POLL_LIMIT 100000

/* MOD_SHIFT and MOD_CTRL come from display.h, which is where the window
 * protocol already agrees with the kernel about them. */

/* Arrows, delivered as control characters in the range Ctrl+letter does not
 * use, so they travel through read() and the window protocol without needing a
 * wider key type. Mirrored in user/libc/include/wproto.h. */
#define KEY_UP    0x1C
#define KEY_DOWN  0x1D
#define KEY_LEFT  0x1E
#define KEY_RIGHT 0x1F

static mutex_t g_lock;

/* --- the controller --------------------------------------------------------- */

static int wait_writable(void)
{
    unsigned i;
    for (i = 0; i < POLL_LIMIT; ++i)
        if ((inb(STATUS_PORT) & STATUS_INPUT_FULL) == 0)
            return 1;
    return 0;
}

static int wait_readable(void)
{
    unsigned i;
    for (i = 0; i < POLL_LIMIT; ++i)
        if ((inb(STATUS_PORT) & STATUS_OUTPUT_FULL) != 0)
            return 1;
    return 0;
}

static void send_command(unsigned char c)
{
    if (wait_writable())
        outb(COMMAND_PORT, c);
}

static void send_data(unsigned char c)
{
    if (wait_writable())
        outb(DATA_PORT, c);
}

static unsigned char read_data(void)
{
    if (!wait_readable())
        return 0;
    return inb(DATA_PORT);
}

/* Anything for the mouse is prefixed, or the controller takes it as a keyboard
 * command. */
static unsigned char send_to_mouse(unsigned char value)
{
    if (wait_writable()) outb(COMMAND_PORT, CMD_WRITE_PORT2);
    if (wait_writable()) outb(DATA_PORT, value);
    return read_data();
}

/* An absent controller floats every port high, so every read is 0xFF - which
 * is not a status any real 8042 reports, every bit set at once including both
 * error bits. It is a reliable way to tell "no controller" from "busy". */
static int controller_present(void)
{
    return inb(STATUS_PORT) != 0xFF;
}

static void drain_output(void)
{
    unsigned i;
    for (i = 0; i < POLL_LIMIT; ++i) {
        if ((inb(STATUS_PORT) & STATUS_OUTPUT_FULL) == 0)
            return;
        (void)inb(DATA_PORT);
    }
}

/* --- the keyboard ----------------------------------------------------------- */

#define RELEASE_FLAG 0x80
#define SC_LSHIFT    0x2A
#define SC_RSHIFT    0x36
#define SC_CTRL      0x1D
#define SC_CAPS      0x3A

static const char kUnshifted[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,   'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,   '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    0,   '*', 0,   ' ',
};

static const char kShifted[128] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
    0,   '*', 0,   ' ',
};

static int g_shift, g_ctrl, g_caps, g_extended;

static void post_char(char c)
{
    if (c != 0)
        __syscall(SYS_inputpost, 0, (long)(unsigned char)c, 0, 0, 0);
}

static void post_mods(void)
{
    __syscall(SYS_inputpost, 1,
              (long)((g_shift ? MOD_SHIFT : 0u) | (g_ctrl ? MOD_CTRL : 0u)),
              0, 0, 0);
}

static void handle_scancode(unsigned char scancode)
{
    char c;

    /* The grey keys arrive as 0xE0 then an ordinary code, and that code means
     * something different from the same byte alone - 0x48 is 'up' after a
     * prefix and the keypad's 8 without one. */
    if (scancode == 0xE0) {
        g_extended = 1;
        return;
    }
    if (g_extended) {
        g_extended = 0;
        if ((scancode & RELEASE_FLAG) == 0) {
            switch (scancode) {
            case 0x48: post_char(KEY_UP);    break;
            case 0x50: post_char(KEY_DOWN);  break;
            case 0x4B: post_char(KEY_LEFT);  break;
            case 0x4D: post_char(KEY_RIGHT); break;
            default: break;
            }
        }
        return;
    }

    if (scancode & RELEASE_FLAG) {
        const unsigned char released = (unsigned char)(scancode & ~RELEASE_FLAG);
        if (released == SC_LSHIFT || released == SC_RSHIFT) {
            g_shift = 0; post_mods();
        } else if (released == SC_CTRL) {
            g_ctrl = 0; post_mods();
        }
        return;
    }

    switch (scancode) {
    case SC_LSHIFT:
    case SC_RSHIFT: g_shift = 1; post_mods(); return;
    case SC_CTRL:   g_ctrl  = 1; post_mods(); return;
    case SC_CAPS:   g_caps  = !g_caps;        return;
    default: break;
    }

    if (scancode >= 128)
        return;
    c = g_shift ? kShifted[scancode] : kUnshifted[scancode];
    if (c == 0)
        return;

    /* Caps affects letters only, and inverts rather than overrides shift. */
    if (g_caps) {
        if (c >= 'a' && c <= 'z')      c = (char)(c - 'a' + 'A');
        else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    if (g_ctrl && c >= 'a' && c <= 'z')      c = (char)(c - 'a' + 1);
    else if (g_ctrl && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 1);
    /* The three that are not letters and still mean something. Ctrl-\ is the
     * quit key, and a terminal wanting to offer it has to be given it. */
    else if (g_ctrl && c == '[')  c = 0x1B;
    else if (g_ctrl && c == '\\') c = 0x1C;
    else if (g_ctrl && c == ']')  c = 0x1D;

    post_char(c);
}

/* --- the mouse -------------------------------------------------------------- */

static int g_mx, g_my;
static int g_max_x = 1023, g_max_y = 767;   /* until the framebuffer says */
/* Four bytes when the mouse has a wheel, three when it has not.
 *
 * A plain PS/2 mouse sends three and knows nothing about a fourth. The
 * IntelliMouse extension adds one carrying the wheel, and it is asked for by
 * the knock below: set the sample rate to 200, then 100, then 80, and read the
 * device id. A mouse that understands answers 3; one that does not answers 0
 * and keeps sending three-byte packets, which is why the length is remembered
 * rather than assumed. */
static unsigned char g_packet[4];
static int g_index;
static int g_packet_len = 3;

static void handle_mouse_byte(unsigned char byte)
{
    unsigned flags;
    int dx, dy;

    /* Byte 0 always has bit 3 set. If it is clear we are out of phase - drop
     * it rather than decoding garbage into cursor movement. */
    if (g_index == 0 && (byte & 0x08) == 0)
        return;

    g_packet[g_index++] = byte;
    if (g_index < g_packet_len)
        return;
    g_index = 0;

    flags = g_packet[0];
    if ((flags & 0xC0) != 0)
        return;                     /* the counters saturated; deltas are junk */

    /* 9-bit two's complement: the sign lives in the flags byte. */
    dx = g_packet[1];
    dy = g_packet[2];
    if (flags & 0x10) dx |= ~0xFF;
    if (flags & 0x20) dy |= ~0xFF;

    g_mx += dx;
    g_my -= dy;                     /* the mouse calls up positive; screens do not */

    /* Clamped *here*, not just where it is stored. The accumulator is the
     * thing that has to stop at the edge: let it run past and a pointer pushed
     * into a corner has to be dragged all the way back before it appears to
     * move again. It looks stuck, and it is stuck for exactly as far as it was
     * pushed. */
    if (g_mx < 0) g_mx = 0;
    if (g_my < 0) g_my = 0;
    if (g_mx > g_max_x) g_mx = g_max_x;
    if (g_my > g_max_y) g_my = g_max_y;

    /* The wheel, when there is one. The low four bits are a signed count of
     * detents since the last packet - almost always one - and the high bits
     * are the fourth and fifth buttons, which nothing here uses. */
    int wheel = 0;
    if (g_packet_len == 4) {
        wheel = (int)(signed char)(g_packet[3] & 0x0F);
        if (wheel > 7)
            wheel -= 16;            /* four bits, two's complement */
    }

    __syscall(SYS_inputpost, 2, (long)g_mx, (long)g_my,
              (long)(flags & 0x07), (long)wheel);
}

/* --- the shared drain -------------------------------------------------------- */

static void drain(void)
{
    unsigned guard;
    mutex_lock(&g_lock);
    for (guard = 0; guard < 64; ++guard) {
        const unsigned char status = inb(STATUS_PORT);
        if ((status & STATUS_OUTPUT_FULL) == 0)
            break;
        /* Whose byte this is has to be decided before it is read, because
         * reading is what clears it - and both threads are looking at the
         * same port. */
        {
            const int from_mouse = (status & STATUS_FROM_MOUSE) != 0;
            const unsigned char byte = inb(DATA_PORT);
            if (from_mouse)
                handle_mouse_byte(byte);
            else
                handle_scancode(byte);
        }
    }
    mutex_unlock(&g_lock);
}

static void mouse_thread(void* arg)
{
    (void)arg;
    if (irq_listen(12) != 0)
        return;
    for (;;) {
        irq_wait(12);
        drain();
    }
}

/* --- bring-up ---------------------------------------------------------------- */

static int bring_up(void)
{
    unsigned char config;

    if (io_permit(DATA_PORT, 1) != 0 || io_permit(STATUS_PORT, 1) != 0)
        return -1;
    if (!controller_present())
        return -2;

    /* Quiet both ports while reconfiguring, so nothing arrives mid-sequence
     * and gets mistaken for a command response. */
    send_command(CMD_DISABLE_PORT1);
    send_command(CMD_DISABLE_PORT2);
    drain_output();

    send_command(CMD_READ_CONFIG);
    config = read_data();
    config |= CFG_PORT1_IRQ | CFG_PORT2_IRQ;
    config &= (unsigned char)~CFG_PORT1_CLOCK;
    /* Translation makes the controller turn the set 2 codes the hardware
     * really sends into the set 1 codes the tables above are written against.
     * Turning it off would silently reinterpret every key. */
    config |= CFG_TRANSLATION;
    send_command(CMD_WRITE_CONFIG);
    send_data(config);

    send_command(CMD_ENABLE_PORT1);
    send_command(CMD_ENABLE_PORT2);

    send_data(KBD_ENABLE_SCANNING);
    if (read_data() != ACK)
        drain_output();             /* not fatal; some emulated 8042s stay quiet */

    send_to_mouse(MOUSE_SET_DEFAULTS);

    /* Ask for the wheel. The knock is a sequence of sample rates that means
     * nothing to a mouse without one and switches on the fourth packet byte
     * for a mouse with one; the device id afterwards says which happened. */
    send_to_mouse(MOUSE_SET_SAMPLE_RATE); send_to_mouse(200);
    send_to_mouse(MOUSE_SET_SAMPLE_RATE); send_to_mouse(100);
    send_to_mouse(MOUSE_SET_SAMPLE_RATE); send_to_mouse(80);
    /* The command is acknowledged and the id follows it, so the answer is the
     * byte after the one send_to_mouse hands back. */
    send_to_mouse(MOUSE_GET_DEVICE_ID);
    const unsigned char id = read_data();
    if (id == 3)
        g_packet_len = 4;
    printf("ps2d: mouse id %u, %d-byte packets%s\n", id, g_packet_len,
           g_packet_len == 4 ? ", wheel" : ", no wheel");

    send_to_mouse(MOUSE_ENABLE);
    drain_output();
    return 0;
}

int main(void)
{
    int port;
    const int rc = bring_up();

    if (rc == -1) {
        printf("ps2d: cannot take the controller's ports\n");
        return 1;
    }
    if (rc == -2) {
        printf("ps2d: no PS/2 controller on this machine\n");
        return 1;
    }

    port = port_create(IPC_PORT_PS2);
    if (port < 0) {
        printf("ps2d: something already drives the 8042\n");
        return 1;
    }
    {
        struct fb_info fb;
        if (fb_info(&fb) == 0 && fb.width > 0 && fb.height > 0) {
            g_max_x = (int)fb.width - 1;
            g_max_y = (int)fb.height - 1;
            g_mx = g_max_x / 2;
            g_my = g_max_y / 2;
        }
    }
    printf("ps2d[%d]: keyboard and mouse on IRQ 1 and 12, in ring 3\n", getpid());

    if (irq_listen(1) != 0) {
        printf("ps2d: cannot claim IRQ 1\n");
        return 1;
    }
    thread_create(mouse_thread, 0);

    /* Anything already sitting in the buffer from before we were listening. */
    drain();

    for (;;) {
        irq_wait(1);
        drain();
    }
}
