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
#include <app.h>
#include <ui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <widget.h>
#include <window.h>


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

/* --- the interface ---------------------------------------------------------
 *
 * Two buttons, a stepper for the volume, and the position as a level. The bar
 * was drawn by hand and seeked by arithmetic on its own rectangle; a level
 * knows where it is, and the layout knows where the level is.
 */

/* Whether there is a sound device at all. A local in the old main, which is
 * why every function that wants it had to be part of that loop. */
static int have_audio;

static struct app g_app;
static struct ui_view* g_name_label;
static struct ui_view* g_note_label;
static struct ui_view* g_pos;
static struct ui_view* g_times;
static struct ui_view* g_play_button;
static struct ui_view* g_vol;
static char g_time_text[48];

static void sync_state(void)
{
    const unsigned long at = g_sound ? snd_position(g_sound) : 0;
    char left[24], right[24];
    draw_time(left, sizeof(left), at);
    draw_time(right, sizeof(right), g_total);
    snprintf(g_time_text, sizeof(g_time_text), "%s  of  %s", left, right);
    ui_set_text(g_times, g_time_text);
    ui_set_text(g_name_label, g_name[0] ? g_name : "no file");
    ui_set_text(g_note_label, g_note);
    ui_set_text(g_play_button, g_playing ? "Pause" : "Play");
    g_pos->max = g_total > 0 ? (int)(g_total / 1000 + 1) : 1;
    g_pos->value = g_total > 0 ? (int)(at / 1000) : 0;
    g_vol->value = g_volume;
}

/* Play and pause are the same button, which is why they are the same
 * function: pausing drops what is queued rather than stopping the device, so
 * resuming picks up where the position already is. */
static void toggle_play(void)
{
    if (g_sound == 0 || !have_audio)
        return;
    g_playing = !g_playing;
    if (!g_playing)
        audio_stop();
    say(g_playing ? "playing" : "paused");
}

static void on_play(struct ui_view* v, void* u)
{
    (void)v; (void)u;
    toggle_play();
    sync_state();
}

static void on_stop(struct ui_view* v, void* u)
{
    (void)v; (void)u;
    g_playing = 0;
    audio_stop();
    seek_to(0);
    say("stopped");
    sync_state();
}

static void on_volume(struct ui_view* v, void* user)
{
    (void)user;
    g_volume = v->value;
    audio_set_volume(g_volume);
    sync_state();
}

static int on_tick(struct app* a)
{
    (void)a;
    pump();
    sync_state();
    return 1;
}

static int on_event(struct app* a, const struct win_event* e)
{
    (void)a;
    if (e->type != WIN_EVENT_KEY)
        return 0;
    if (e->key == ' ')                    toggle_play();
    else if (e->key == WIN_KEY_LEFT)      seek_by(-5);
    else if (e->key == WIN_KEY_RIGHT)     seek_by(5);
    else return 0;
    sync_state();
    return 1;
}

int main(int argc, char** argv)
{
    struct ui_view* root = ui_box(0, UI_STACK_V, 16, 8);
    g_name_label = ui_label(root, "no file");  ui_grow(g_name_label, 0);
    g_note_label = ui_label(root, "");         ui_grow(g_note_label, 0);

    g_pos = ui_level(root, 0, 1, 0);
    ui_size(g_pos, 0, 14);
    ui_grow(g_pos, 0);
    g_times = ui_label(root, "");
    ui_grow(g_times, 0);

    struct ui_view* row = ui_box(root, UI_STACK_H, 0, 10);
    ui_size(row, 0, 26);
    ui_grow(row, 0);
    g_play_button = ui_button(row, "Play", on_play, 0);
    ui_grow(g_play_button, 0);
    ui_grow(ui_button(row, "Stop", on_stop, 0), 0);
    ui_grow(ui_label(row, "volume"), 0);
    g_vol = ui_stepper(row, 80, 100);
    ui_on(g_vol, on_volume, 0);
    ui_grow(g_vol, 0);
    ui_spacer(row);

    ui_grow(ui_label(root, "space plays, arrows seek"), 0);
    ui_spacer(root);

    struct audio_info info;
    have_audio = (audio_info(&info) == 0 && info.present);
    if (!have_audio)
        say("no sound device");

    if (argc > 1 && argv[1][0] != '\0' && argv[1][0] != '-') {
        open_file(argv[1]);
        if (g_sound != 0 && have_audio)
            g_playing = 1;      /* a file named on the command line plays */
    } else {
        say(snd_formats());
    }
    sync_state();

    g_app.title = "Music";
    g_app.width = 440; g_app.height = 240;
    g_app.min_width = 380; g_app.min_height = 200;
    g_app.tick_ms = 200;
    g_app.tick = on_tick;
    g_app.event = on_event;
    g_app.root = root;
    return app_run(&g_app, argc, argv);
}
