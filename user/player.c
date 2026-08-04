/* player - plays a sound file.
 *
 * The decoding is libc's, in <sound.h>, which streams rather than loading: a
 * three-minute song is thirty-four megabytes at the rate the hardware wants,
 * and a player that read all of it before making a noise would look broken for
 * several seconds every time.
 *
 * So the loop here has two jobs at once - answer the window server, and keep
 * the card's queue from running dry. Neither may block. audio_play takes what
 * it can and says how much, so the rule is simply to offer it more whenever it
 * has room, in between polling for events.
 */

#include <audio.h>
#include <sound.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <widget.h>
#include <window.h>

static uint32_t* g_px;
static unsigned  g_w = 460, g_h = 210;

static struct sound* g_sound;
static char g_path[256];
static char g_name[128];
static char g_note[160];

static unsigned      g_rate, g_channels;
static unsigned long g_total;               /* output frames, 0 if unknown */
static const char*   g_format = "";

static int g_playing;
static int g_volume = 70;

/* Samples handed to audiod but not yet heard. The card's queue is what makes
 * the sound continuous, and it is also why the position shown has to be the
 * position *played* rather than the position decoded - those differ by however
 * much is buffered, which at this size is about a fifth of a second. */
#define FEED_SAMPLES 8192
static int16_t g_feed[FEED_SAMPLES];

#define BAR_X 16
#define BAR_Y 96
#define BAR_H 14

struct box { int x, y, w, h; };
static struct box g_b_play = {  16, 130, 62, 24 };
static struct box g_b_stop = {  84, 130, 62, 24 };
static struct box g_b_vdn  = { 300, 130, 26, 24 };
static struct box g_b_vup  = { 330, 130, 26, 24 };

static int inside(const struct box* b, int x, int y)
{
    return x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h;
}

static int bar_w(void) { return (int)g_w - 2 * BAR_X; }

static void say(const char* text)
{
    snprintf(g_note, sizeof(g_note), "%s", text);
}

/* --- the file ------------------------------------------------------------- */

static void base_name(const char* path, char* out, int max)
{
    int cut = -1;
    for (int i = 0; path[i] != '\0'; ++i)
        if (path[i] == '/')
            cut = i;
    snprintf(out, (unsigned)max, "%s", path + cut + 1);
}

static int open_file(const char* path)
{
    if (g_sound != 0) {
        audio_stop();
        snd_close(g_sound);
        g_sound = 0;
    }
    g_playing = 0;
    g_total = 0;
    g_rate = g_channels = 0;
    g_format = "";

    snprintf(g_path, sizeof(g_path), "%s", path);
    base_name(path, g_name, sizeof(g_name));

    g_sound = snd_open(path);
    if (g_sound == 0) {
        /* The library's own words. "MP3: no decoder for one yet" is a
         * different problem from "cannot open the file", and the person
         * needs to be able to tell them apart. */
        say(snd_error());
        return -1;
    }
    snd_info(g_sound, &g_rate, &g_channels, &g_total, &g_format);

    char line[160];
    snprintf(line, sizeof(line), "%s, %u Hz, %s", g_format, g_rate,
             g_channels == 1 ? "mono" : "stereo");
    say(line);
    return 0;
}

/* --- keeping the queue full ------------------------------------------------ */

static void pump(void)
{
    if (!g_playing || g_sound == 0)
        return;

    /* Only as much as the card will take. Offering more would mean holding
     * samples here that snd_read has already consumed, and then having to
     * remember them - the queue is audiod's job and it already does it. */
    long room = audio_space();
    while (room >= 2) {
        long want = room;
        if (want > FEED_SAMPLES)
            want = FEED_SAMPLES;
        const long got = snd_read(g_sound, g_feed, want);
        if (got <= 0) {
            /* End of the file. The queue still holds whatever was handed over,
             * so it keeps playing for a moment after this - flush tells audiod
             * to send the last part-filled buffer rather than sit on it. */
            audio_flush();
            g_playing = 0;
            say("finished");
            return;
        }
        const long taken = audio_play(g_feed, got);
        if (taken < got) {
            /* It took less than offered, which means the queue filled between
             * asking and telling. The rest would have to be held here, so
             * rather than carry a second buffer, wind the file back by what
             * was not taken and read it again next time round. */
            snd_seek(g_sound, snd_position(g_sound) - (unsigned long)((got - taken) / 2));
            return;
        }
        room -= got;
        if (got < want)
            break;                      /* the file gave less than asked for */
    }
}

/* --- drawing ---------------------------------------------------------------- */

static void draw_time(char* out, int max, unsigned long frames)
{
    const unsigned long seconds = frames / AUDIO_RATE;
    snprintf(out, (unsigned)max, "%lu:%02lu", seconds / 60, seconds % 60);
}

static void draw(void)
{
    wg_fill(0, 0, (int)g_w, (int)g_h, WG_FACE);

    wg_text_clipped(16, 14, g_name[0] ? g_name : "no file", WG_INK, (int)g_w - 32);
    wg_text_clipped(16, 36, g_note, WG_DIM, (int)g_w - 32);

    /* The progress bar, sunken, with the played part filled. */
    const int bw = bar_w();
    wg_fill(BAR_X, BAR_Y, bw, BAR_H, WG_PAPER);
    wg_bevel(BAR_X, BAR_Y, bw, BAR_H, 0);
    const unsigned long at = g_sound ? snd_position(g_sound) : 0;
    if (g_total > 0 && bw > 4) {
        long filled = (long)((double)(bw - 4) * (double)at / (double)g_total);
        if (filled < 0) filled = 0;
        if (filled > bw - 4) filled = bw - 4;
        wg_fill(BAR_X + 2, BAR_Y + 2, (int)filled, BAR_H - 4, wg_sel_colour());
    }

    char left[24], right[24];
    draw_time(left, sizeof(left), at);
    draw_time(right, sizeof(right), g_total);
    wg_text(BAR_X, BAR_Y + BAR_H + 6, left, WG_DIM);
    wg_text((int)g_w - BAR_X - 8 * (int)strlen(right), BAR_Y + BAR_H + 6,
            right, WG_DIM);

    wg_button(g_b_play.x, g_b_play.y, g_b_play.w, g_b_play.h,
              g_playing ? "Pause" : "Play", g_playing);
    wg_button(g_b_stop.x, g_b_stop.y, g_b_stop.w, g_b_stop.h, "Stop", 0);

    wg_text(190, g_b_play.y + 6, "volume", WG_DIM);
    wg_button(g_b_vdn.x, g_b_vdn.y, g_b_vdn.w, g_b_vdn.h, "-", 0);
    wg_button(g_b_vup.x, g_b_vup.y, g_b_vup.w, g_b_vup.h, "+", 0);
    char vol[16];
    snprintf(vol, sizeof(vol), "%d%%", g_volume);
    wg_text(364, g_b_play.y + 6, vol, WG_INK);

    wg_text(16, (int)g_h - 22, "space plays, arrows seek", WG_DIM);
}

/* --- seeking ---------------------------------------------------------------- */

static void seek_to(unsigned long frame)
{
    if (g_sound == 0 || g_total == 0)
        return;
    if (frame > g_total)
        frame = g_total;
    /* Drop what is queued, or the old position keeps playing for a fifth of a
     * second after the bar has moved. */
    audio_stop();
    snd_seek(g_sound, frame);
}

static void seek_by(long seconds)
{
    if (g_sound == 0)
        return;
    const long at = (long)snd_position(g_sound) + seconds * (long)AUDIO_RATE;
    seek_to(at < 0 ? 0 : (unsigned long)at);
}

int main(int argc, char** argv)
{
    if (wg_font() != 0)
        return 1;

    struct audio_info info;
    const int have_audio = (audio_info(&info) == 0 && info.present);

    const int id = win_create(240, 140, g_w, g_h, "Music");
    if (id < 0) {
        printf("player: no window server\n");
        return 1;
    }
    g_px = win_map(id);
    if (g_px == 0)
        return 1;
    win_set_min_size(id, 380, 190);
    wg_target(g_px, g_w, g_h);

    if (argc > 1) {
        open_file(argv[1]);
        if (g_sound != 0 && have_audio)
            g_playing = 1;
    } else {
        say(snd_formats());
    }
    if (!have_audio)
        say("no sound device: nothing to play through");
    g_volume = audio_volume();

    draw();
    win_present(id);

    unsigned long last_shown = ~0ul;
    for (;;) {
        struct win_event e;
        int dirty = 0;

        while (win_poll(id, &e)) {
            if (e.type == WIN_EVENT_CLOSE) {
                audio_stop();
                win_destroy(id);
                return 0;
            }
            if (e.type == WIN_EVENT_RESIZE) {
                g_w = (unsigned)e.x; g_h = (unsigned)e.y;
                g_px = win_map(id);
                if (g_px == 0) return 1;
                wg_target(g_px, g_w, g_h);
                /* The controls sit along the bottom, so they move with it. */
                const int row = (int)g_h - 80;
                g_b_play.y = g_b_stop.y = g_b_vdn.y = g_b_vup.y = row;
                g_b_vup.x = (int)g_w - 40;
                g_b_vdn.x = (int)g_w - 70;
                dirty = 1;
            } else if (e.type == WIN_EVENT_MOUSE_DOWN) {
                if (inside(&g_b_play, e.x, e.y)) {
                    if (g_sound != 0 && have_audio) {
                        g_playing = !g_playing;
                        if (!g_playing)
                            audio_stop();       /* pause: drop what is queued */
                        say(g_playing ? "playing" : "paused");
                    }
                } else if (inside(&g_b_stop, e.x, e.y)) {
                    g_playing = 0;
                    audio_stop();
                    seek_to(0);
                    say("stopped");
                } else if (inside(&g_b_vdn, e.x, e.y)) {
                    g_volume = audio_set_volume(g_volume - 10);
                } else if (inside(&g_b_vup, e.x, e.y)) {
                    g_volume = audio_set_volume(g_volume + 10);
                } else if (e.y >= BAR_Y - 4 && e.y < BAR_Y + BAR_H + 4 &&
                           g_total > 0) {
                    /* Click the bar to go there. */
                    const int bw = bar_w();
                    long rel = e.x - BAR_X - 2;
                    if (rel < 0) rel = 0;
                    if (rel > bw - 4) rel = bw - 4;
                    seek_to((unsigned long)((double)g_total * (double)rel /
                                            (double)(bw - 4)));
                }
                dirty = 1;
            } else if (e.type == WIN_EVENT_KEY) {
                switch (e.key) {
                case ' ':
                    if (g_sound != 0 && have_audio) {
                        g_playing = !g_playing;
                        if (!g_playing)
                            audio_stop();
                        say(g_playing ? "playing" : "paused");
                    }
                    break;
                case 'k': seek_by(-5); break;   /* the arrows the server sends */
                case 'l': seek_by(5); break;
                case '[': g_volume = audio_set_volume(g_volume - 10); break;
                case ']': g_volume = audio_set_volume(g_volume + 10); break;
                default: continue;
                }
                dirty = 1;
            }
        }

        pump();

        /* Repaint when the second changes rather than every time round: the
         * bar moves in seconds and a busy repaint would compete with the very
         * loop that has to keep the audio queue fed. */
        const unsigned long now = g_sound ? snd_position(g_sound) / AUDIO_RATE : 0;
        if (now != last_shown) {
            last_shown = now;
            dirty = 1;
        }
        if (dirty) {
            draw();
            win_present(id);
        }
        msleep(10);
    }
}
