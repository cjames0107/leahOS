/* Archiver - what is inside an archive, and how to get it out.
 *
 * `tar -t` answers the same question, and answers it into a terminal as a list
 * that scrolls past. The thing a person actually does with an archive is look
 * at it, decide whether they want it, and put some or all of it somewhere -
 * which is three commands and the memory of what the first one said. A window
 * holds all three at once.
 *
 * It reads what tar reads, including gzipped archives, because the reading is
 * the same library. See user/libc/archive.c.
 */

#include <app.h>
#include <ui.h>
#include <archive.h>
#include <paths.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_ENTRIES 2048

static struct app g_app;
static struct ui_view* g_table;
static struct ui_view* g_note;

static struct ar_entry g_entry[MAX_ENTRIES];
static unsigned long   g_body[MAX_ENTRIES];   /* offset of each body */
static int g_count;

static unsigned char* g_data;
static unsigned long  g_len;
static char g_path[256];
static char g_message[192];

/* What is being asked of the file dialogue, since one sheet serves three
 * questions: which archive to open, where to extract it, and what to pack. */
enum { ASK_NOTHING, ASK_OPEN, ASK_EXTRACT_TO, ASK_PACK, ASK_SAVE_AS };
static int g_asking;
static char g_packing[256];

static void say(const char* text)
{
    snprintf(g_message, sizeof(g_message), "%s", text);
    ui_set_text(g_note, g_message);
}

static void human(unsigned long bytes, char* out, unsigned max)
{
    if (bytes >= 1024UL * 1024UL)
        snprintf(out, max, "%lu.%lu MB", bytes / (1024 * 1024),
                 (bytes % (1024 * 1024)) * 10 / (1024 * 1024));
    else if (bytes >= 1024)
        snprintf(out, max, "%lu KB", bytes / 1024);
    else
        snprintf(out, max, "%lu B", bytes);
}

/* --- the listing ------------------------------------------------------------- */

static int collect(void* user, const struct ar_entry* e, const unsigned char* body)
{
    (void)user;
    if (g_count >= MAX_ENTRIES)
        return 1;
    g_entry[g_count] = *e;
    g_body[g_count] = (unsigned long)(body - g_data);
    ++g_count;
    return 0;
}

static const char* cell(void* user, int row, int col)
{
    (void)user;
    static char text[24];
    if (row < 0 || row >= g_count)
        return "";
    switch (col) {
    case 0: return g_entry[row].path;
    case 1:
        if (g_entry[row].kind == AR_DIR)
            return "";
        human(g_entry[row].size, text, sizeof(text));
        return text;
    default:
        return g_entry[row].kind == AR_DIR  ? "folder"
             : g_entry[row].kind == AR_FILE ? "file"
                                            : "other";
    }
}

static void open_archive(const char* path)
{
    free(g_data);
    g_data = 0;
    g_count = 0;

    g_data = ar_read(path, &g_len);
    if (g_data == 0) {
        g_table->rows = 0;
        say("that archive could not be read");
        return;
    }
    const long members = ar_walk(g_data, g_len, collect, 0);
    if (members < 0) {
        free(g_data);
        g_data = 0;
        g_count = 0;
        g_table->rows = 0;
        say("that is not a tar archive");
        return;
    }
    snprintf(g_path, sizeof(g_path), "%s", path);
    g_table->rows = g_count;
    g_table->selected = g_count > 0 ? 0 : -1;
    g_table->scroll = 0;

    unsigned long total = 0;
    for (int i = 0; i < g_count; ++i)
        total += g_entry[i].size;
    char size[24];
    human(total, size, sizeof(size));
    char note[192];
    snprintf(note, sizeof(note), "%s - %d item%s, %s uncompressed",
             path, g_count, g_count == 1 ? "" : "s", size);
    say(note);
}

/* --- what the buttons do ------------------------------------------------------ */

static void on_open(struct ui_view* v, void* u)
{
    (void)v; (void)u;
    g_asking = ASK_OPEN;
    app_sheet_file(&g_app, "/root");
}

static void extract_into(const char* dir, int only_selected)
{
    if (g_data == 0 || g_count == 0) {
        say("there is nothing open to extract");
        return;
    }
    int done = 0, failed = 0;
    for (int i = 0; i < g_count; ++i) {
        if (only_selected && i != g_table->selected)
            continue;
        if (g_entry[i].kind == AR_OTHER)
            continue;
        if (ar_extract(&g_entry[i], &g_data[g_body[i]], dir) == 0)
            ++done;
        else
            ++failed;
    }
    char note[192];
    if (failed > 0)
        snprintf(note, sizeof(note), "%d put into %s, %d refused",
                 done, dir, failed);
    else
        snprintf(note, sizeof(note), "%d item%s put into %s",
                 done, done == 1 ? "" : "s", dir);
    say(note);
}

static int g_only_selected;

static void on_extract_all(struct ui_view* v, void* u)
{
    (void)v; (void)u;
    if (g_data == 0) { say("open an archive first"); return; }
    g_only_selected = 0;
    g_asking = ASK_EXTRACT_TO;
    app_sheet_file(&g_app, "/root");
}

static void on_extract_one(struct ui_view* v, void* u)
{
    (void)v; (void)u;
    if (g_data == 0 || g_table->selected < 0) {
        say("choose an item first");
        return;
    }
    g_only_selected = 1;
    g_asking = ASK_EXTRACT_TO;
    app_sheet_file(&g_app, "/root");
}

static void on_pack(struct ui_view* v, void* u)
{
    (void)v; (void)u;
    g_asking = ASK_PACK;
    app_sheet_file(&g_app, "/root");
}

/* The leaf of a path, which is what a folder should be called inside the
 * archive - not the whole path it happened to be found at. */
static const char* leaf_of(const char* path)
{
    const char* leaf = path;
    for (const char* p = path; *p != '\0'; ++p)
        if (*p == '/' && p[1] != '\0')
            leaf = p + 1;
    return leaf;
}

static void pack_into(const char* archive)
{
    struct ar_out* out = ar_create(archive);
    if (out == 0) {
        say("that archive could not be created");
        return;
    }
    struct stat st;
    int ok;
    if (stat(g_packing, &st) == 0 && st.st_type == S_IFDIR)
        ok = ar_add_tree(out, g_packing, leaf_of(g_packing)) == 0;
    else
        ok = ar_add(out, g_packing, leaf_of(g_packing)) == 0;
    ok = (ar_finish(out) == 0) && ok;

    if (!ok) {
        say("that archive could not be written");
        return;
    }
    open_archive(archive);
    app_relayout(&g_app);
}

static void on_sheet(struct app* a, int result)
{
    const int asking = g_asking;
    g_asking = ASK_NOTHING;
    if (!result)
        return;
    const char* chosen = app_sheet_path(a);

    if (asking == ASK_OPEN) {
        open_archive(chosen);
        app_relayout(a);
    } else if (asking == ASK_EXTRACT_TO) {
        /* A folder was asked for; if a file was chosen, its folder is meant. */
        char dir[256];
        snprintf(dir, sizeof(dir), "%s", chosen);
        struct stat st;
        if (stat(dir, &st) == 0 && st.st_type != S_IFDIR) {
            char* cut = dir;
            for (char* p = dir; *p != '\0'; ++p)
                if (*p == '/')
                    cut = p;
            *cut = '\0';
        }
        extract_into(dir[0] != '\0' ? dir : "/", g_only_selected);
    } else if (asking == ASK_PACK) {
        snprintf(g_packing, sizeof(g_packing), "%s", chosen);
        g_asking = ASK_SAVE_AS;
        char suggested[128];
        snprintf(suggested, sizeof(suggested), "%s.tar", leaf_of(g_packing));
        app_sheet_save(a, "/root", suggested);
    } else if (asking == ASK_SAVE_AS) {
        pack_into(chosen);
    }
}

static const char* const kMenu[] = { "Open...", "Extract All...", "New Archive..." };

static int on_menu(struct app* a, int pick)
{
    (void)a;
    if (pick == 0)      on_open(0, 0);
    else if (pick == 1) on_extract_all(0, 0);
    else if (pick == 2) on_pack(0, 0);
    return 1;
}

int main(int argc, char** argv)
{
    struct ui_view* root = ui_box(0, UI_STACK_V, 8, 8);

    struct ui_view* bar = ui_box(root, UI_STACK_H, 0, 8);
    ui_grow(ui_size(bar, 0, 26), 0);
    ui_grow(ui_size(ui_button(bar, "Open...", on_open, 0), 88, 24), 0);
    ui_grow(ui_size(ui_button(bar, "Extract All", on_extract_all, 0), 106, 24), 0);
    ui_grow(ui_size(ui_button(bar, "Extract Item", on_extract_one, 0), 112, 24), 0);
    ui_spacer(bar);
    ui_grow(ui_size(ui_button(bar, "New Archive...", on_pack, 0), 124, 24), 0);

    g_table = ui_table(root, cell, 0, 0);
    ui_column(g_table, "Name", 380);
    ui_column(g_table, "Size", 96);
    ui_column(g_table, "Kind", 90);
    ui_grow(g_table, 1);

    g_note = ui_grow(ui_size(ui_label(root, ""), 0, 20), 0);

    const char* open_this = 0;
    for (int i = 1; i < argc; ++i)
        if (argv[i][0] != '\0' && (argv[i][0] < '0' || argv[i][0] > '9'))
            open_this = argv[i];

    g_app.title = "Archiver";
    g_app.width = 640; g_app.height = 440;
    g_app.min_width = 520; g_app.min_height = 280;
    g_app.root = root;
    g_app.menu = kMenu;
    g_app.menu_count = (int)(sizeof(kMenu) / sizeof(kMenu[0]));
    g_app.menu_pick = on_menu;
    g_app.sheet_done = on_sheet;

    if (open_this != 0)
        open_archive(open_this);
    else
        say("open an archive, or make one from a folder");
    return app_run(&g_app, argc, argv);
}
