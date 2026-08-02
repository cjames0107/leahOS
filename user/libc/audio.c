/* Sound, as a client of the driver rather than of the kernel.
 *
 * Every one of these used to be a system call into an AC'97 driver compiled
 * into the kernel. They are now a shared ring and a port, and the shape of the
 * interface has not changed - which is the point: nothing above this file knew
 * where the driver was, so nothing above it had to be edited when it moved.
 */

#include <aud.h>
#include <audio.h>
#include <ipc.h>
#include <shm.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static int g_port = -2;             /* -2 not tried, -1 no driver */
static struct aud_shared* g_ring;

static int connect(void)
{
    if (g_port == -2) {
        g_port = port_open(IPC_PORT_AUDIO);
        if (g_port >= 0) {
            const int shm = shm_open(AUD_SHM_KEY, sizeof(struct aud_shared), 0);
            g_ring = shm < 0 ? 0 : (struct aud_shared*)shm_map(shm);
            if (g_ring == 0)
                g_port = -1;
        }
    }
    return g_port;
}

static int ask(struct ipc_message* q, struct ipc_message* a)
{
    if (connect() < 0)
        return -1;
    memset(a, 0, sizeof(*a));
    return ipc_call(g_port, q, a);
}

int audio_info(struct audio_info* out)
{
    struct ipc_message q, a;
    memset(&q, 0, sizeof(q));
    q.tag = AUD_INFO;
    if (out == 0)
        return -1;
    memset(out, 0, sizeof(*out));
    if (ask(&q, &a) != 0) {
        out->present = 0;
        return -1;
    }
    out->present = 1;
    out->rate = (unsigned)a.word[0];
    out->channels = (unsigned)a.word[1];
    unsigned n = 0;
    while (n < sizeof(out->name) - 1 && a.data[n] != '\0') {
        out->name[n] = a.data[n];
        ++n;
    }
    out->name[n] = '\0';
    return 0;
}

long audio_space(void)
{
    struct ipc_message q, a;
    memset(&q, 0, sizeof(q));
    q.tag = AUD_SPACE;
    if (ask(&q, &a) != 0)
        return 0;
    return (long)a.word[0];
}

long audio_play(const int16_t* samples, long count)
{
    if (connect() < 0 || samples == 0 || count <= 0)
        return -1;

    /* Write straight into the ring the driver drains. The head moves last, so
     * a driver that looks in the middle of this sees only whole samples. */
    const unsigned head = g_ring->head;
    const unsigned tail = g_ring->tail;
    const unsigned used = (head + AUD_RING - tail) % AUD_RING;
    unsigned room = AUD_RING - 1 - used;
    if (room == 0) {
        /* Full. This is the one place a writer should wait, and waiting here is
         * the sound card setting the pace - which is what it should be doing. */
        msleep(1);
        return 0;
    }
    if ((unsigned)count < room)
        room = (unsigned)count;

    unsigned done = 0;
    unsigned at = head;
    while (done < room) {
        unsigned run = AUD_RING - at;
        if (run > room - done)
            run = room - done;
        memcpy((void*)&g_ring->samples[at], samples + done, run * 2);
        at = (at + run) % AUD_RING;
        done += run;
    }
    __atomic_store_n(&g_ring->head, at, __ATOMIC_RELEASE);

    /* No message. The driver is already looking at this ring every couple of
     * milliseconds, and a synchronous round trip per write costs more than the
     * audio it delivers: at 512 samples a write that is five milliseconds of
     * sound bought with two context switches and a scheduler pass, which is
     * how a tone comes out in pieces. Writing to shared memory and letting the
     * driver find it is the whole reason the segment is shared. */
    return (long)done;
}

void audio_flush(void)
{
    struct ipc_message q, a;
    memset(&q, 0, sizeof(q));
    q.tag = AUD_FLUSH;
    ask(&q, &a);
}

void audio_stop(void)
{
    struct ipc_message q, a;
    memset(&q, 0, sizeof(q));
    q.tag = AUD_STOP;
    ask(&q, &a);
}

int audio_volume(void)
{
    struct ipc_message q, a;
    memset(&q, 0, sizeof(q));
    q.tag = AUD_VOLUME;
    q.word[0] = -1;
    if (ask(&q, &a) != 0)
        return 0;
    return (int)a.word[0];
}

int audio_set_volume(int percent)
{
    struct ipc_message q, a;
    memset(&q, 0, sizeof(q));
    q.tag = AUD_VOLUME;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    q.word[0] = percent;
    if (ask(&q, &a) != 0)
        return 0;
    return (int)a.word[0];
}
