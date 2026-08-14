/* Elements - every component the system has, in one window.
 *
 * This used to be a demonstration of there being no toolkit: it drew its own
 * bevelled rectangles and hit-tested them by hand, and said so, because that
 * was honest about what a client was. There is a toolkit now, so what is worth
 * showing is different - each component, doing its real job, wired to
 * something that visibly reacts.
 *
 * It is also the smallest complete example of writing an application here, and
 * is worth reading as one: there is no event loop, no hit-testing, no layout
 * arithmetic and no redraw bookkeeping in this file. What is here is what the
 * window contains and what happens when it is used.
 */

#include <app.h>
#include <stdio.h>
#include <string.h>
#include <ui.h>

/* What the controls have done, shown back so that every one of them visibly
 * does something. */
static char g_said[96] = "nothing yet";
static struct ui_view* g_report;
static struct ui_view* g_progress;
static struct ui_view* g_name;

static void say(const char* what)
{
    snprintf(g_said, sizeof(g_said), "%s", what);
    ui_set_text(g_report, g_said);
}

/* --- what the controls do -------------------------------------------------- */

static void on_click(struct ui_view* v, void* user)
{
    (void)user;
    char line[96];
    static int count;
    ++count;        /* stepped before the call: reading and writing it in the
                     * same argument list is unsequenced */
    snprintf(line, sizeof(line), "%s pressed, %d time%s", ui_text(v), count,
             count == 1 ? "" : "s");
    say(line);
}

static void on_check(struct ui_view* v, void* user)
{
    (void)user;
    char line[96];
    snprintf(line, sizeof(line), "%s is now %s", ui_text(v),
             v->on ? "on" : "off");
    say(line);
}

static void on_radio(struct ui_view* v, void* user)
{
    (void)user;
    char line[96];
    snprintf(line, sizeof(line), "chose %s", ui_text(v));
    say(line);
}

static void on_slide(struct ui_view* v, void* user)
{
    (void)user;
    char line[96];
    snprintf(line, sizeof(line), "slider at %d", v->value);
    say(line);
    /* Two components wired together, which is the point of having handles to
     * them: moving one moves the other. */
    if (g_progress != 0)
        g_progress->value = v->value;
}

static void on_name(struct ui_view* v, void* user)
{
    (void)user;
    char line[96];
    snprintf(line, sizeof(line), "you typed \"%s\"", ui_text(v));
    say(line);
}

static const char* const kFruit[] = { "apples", "pears", "quinces", "figs" };
static const char* fruit_row(void* user, int row)
{
    (void)user;
    return (row >= 0 && row < 4) ? kFruit[row] : "";
}

static void on_pick(struct ui_view* v, void* user)
{
    (void)user;
    char line[96];
    snprintf(line, sizeof(line), "picked %s", fruit_row(0, v->selected));
    say(line);
}

static const char* const kViews[] = { "One", "Two", "Three" };
static const char* view_row(void* user, int row)
{
    (void)user;
    return (row >= 0 && row < 3) ? kViews[row] : "";
}

static void on_segment(struct ui_view* v, void* user)
{
    (void)user;
    char line[96];
    snprintf(line, sizeof(line), "segment %s", view_row(0, v->on));
    say(line);
}

/* A view the application draws itself, for the cases a component does not
 * cover: a chart, a canvas, a colour bar. It gets a frame from the layout like
 * anything else and draws inside it. */
static void draw_swatches(struct ui_view* v, void* user)
{
    (void)user;
    static const uint32_t kColours[6] = {
        0xC0392B, 0xD68910, 0xF1C40F, 0x27AE60, 0x2980B9, 0x8E44AD
    };
    const int each = v->frame.w / 6;
    for (int i = 0; i < 6; ++i)
        wg_fill(v->frame.x + i * each, v->frame.y, each - 2, v->frame.h,
                kColours[i]);
}

int main(int argc, char** argv)
{
    /* The whole interface, as the shape it makes. Every frame on screen is
     * computed from this by the layout pass; no coordinate appears twice, so
     * none of them can disagree. */
    struct ui_view* root = ui_box(0, UI_STACK_H, 0, 0);

    struct ui_view* side = ui_sidebar(root, fruit_row, 4, 0);
    ui_on(side, on_pick, 0);
    ui_size(side, 150, 0);

    struct ui_view* main_col = ui_box(root, UI_STACK_V, 16, 10);

    ui_label(main_col, "Components");

    struct ui_view* seg = ui_segmented(main_col, view_row, 3, 0);
    ui_on(seg, on_segment, 0);

    struct ui_view* row = ui_box(main_col, UI_STACK_H, 0, 8);
    ui_size(row, 0, 26);
    ui_grow(row, 0);
    ui_button(row, "Press me", on_click, 0);
    ui_button(row, "And me", on_click, 0);
    ui_spacer(row);

    g_name = ui_field(main_col, "");
    ui_on(g_name, on_name, 0);
    ui_grow(g_name, 0);

    struct ui_view* checks = ui_box(main_col, UI_STACK_H, 0, 16);
    ui_size(checks, 0, 22);
    ui_grow(checks, 0);
    ui_on(ui_check(checks, "Enabled", 1), on_check, 0);
    ui_on(ui_radio(checks, "First", 1), on_radio, 0);
    ui_on(ui_radio(checks, "Second", 0), on_radio, 0);

    struct ui_view* slider = ui_slider(main_col, 40, 100);
    ui_on(slider, on_slide, 0);
    ui_grow(slider, 0);

    g_progress = ui_progress(main_col, 40, 100);
    ui_grow(g_progress, 0);

    struct ui_view* swatches = ui_custom(main_col, draw_swatches, 0);
    ui_size(swatches, 0, 28);
    ui_grow(swatches, 0);

    g_report = ui_label(main_col, g_said);
    ui_grow(g_report, 0);

    ui_spacer(main_col);

    struct app a = {
        .title = "Elements",
        .width = 640, .height = 440,
        .min_width = 480, .min_height = 380,
        .sidebar = 150,
        .root = root,
    };
    return app_run(&a, argc, argv);
}
