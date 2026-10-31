/* SPDX-License-Identifier: GPL-2.0-or-later
 * The in-game PC menu: PC Graphics, PC Display, PC Effects, PC Settings, PC Gameplay
 * and Achievements rows in the game's own System menu, each opening a section drawn by the
 * game like Controls or Language, plus Restart to Apply and the title menu's Quit Game. It
 * offers what the L3+R3 overlay offers, which stays available.
 *
 * Ported from droogie's bbhost (https://github.com/droogie/bbhost, GPL-3.0-or-later,
 * engine/option_menu.cpp and engine/menu_pointer.cpp), which has the full map of the engine's
 * option screens. In short: System is a command list - rows appended to a builder by
 * sub_1f4e2a0 and finalised by sub_1fea630. A row's std::function opens a section through
 * sub_1fb4c70, whose opener calls sub_1f20900(root, params, "<Name>", handler): that binds the
 * movie's sprite of that name, makes the dialog and calls handler(dialog, params), which builds
 * the rows. The engine owns the cursor, widgets, key guide and fade.
 *
 * So the menu adds its own rows to System (the one call that finalises the builder goes through
 * a hook) and its own opener and handlers. sub_1f20900 only opens names in a six-entry table, so
 * slot [6] - unreferenced padding - points at the name being opened and the loop walks seven.
 * The sections' sprites and strings come from scripts/menu_assets.py (BB_PC_MENU_ASSETS=1).
 *
 * A row writes its value through a pointer into this file; once a frame the changes become
 * bbport.ini settings, the same ones the Insert menu and the launcher change.
 *
 * Pick lists (Upscaler, Output Resolution, ...) open as a translucent drop-down over the rows
 * below them, whose text would read through it. Every pick list is placed below a section's
 * toggles, and while one is open the captions and values under it are blanked: the rows draw
 * the raw UTF-16 pointers they were laid out with, so the menu lays them out with buffers of its
 * own and empties those (the menus' list update, hooked, says which list has the input). */
#include "pc_menu.h"
#include "engine_hooks.h"
#include "menu_pointer.h"
#include "gpu/bbgpu.h"
#include "runtime.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Binary Ninja addresses of CUSA03173 1.09 (eboot at 0x400000). */
enum {
    OPEN_SECTION = 0x1f20900,      /* sub_1f20900(root, params, name, handler, 0, 0) */
    SCRATCH_INIT = 0x208af20,      /* a handler's scratch block */
    ADD_CHOICE_ROW = 0x1f2a100,    /* (dialog, captions, value*, list*, default*) */
    BUILD_ON_OFF = 0x1f2b3b0,      /* the game's two-entry On/Off list */
    ADD_LIST_ROW = 0x1f29370,      /* a pick list: (dialog, captions, int32*, list*, default*) */
    LIST_APPEND = 0x1f1c0c0,       /* (pick-list container, entry*) */
    WSTR_COPY = 0x2d67040,         /* builds a wstring in place */
    LAYOUT_ROWS = 0x1f28de0,       /* (dialog, 0): rows from the record vector */
    LIST_REDRAW = 0x1ed06e0,       /* a list component's visible items, from their pointers */
    LIST_UPDATE = 0x1ecff30,       /* a list component's update (pad, cursor) */
    LIST_VTABLE = 0x5799fb0,       /* slot 3 is LIST_UPDATE */
    DROPDOWN_VTABLE = 0x5739e90,   /* a pick-list row's widget */
    DROPDOWN_REDRAW = 0x1f2b7c0,   /* its value text, from its entries */
    MSG_REPOSITORY = 0x1ee8cc0,    /* (caption*, group, id) */
    ADD_COMMAND_ROW = 0x1f4e2a0,   /* (builder, captions, std::function*, flag*) */
    ADD_EXIT_ROW = 0x1f4c9a0,      /* Exit Game's own adder (builder, captions, std::function*) */
    EXIT_GAME_OPENER = 0x200c9f0,
    SYSTEM_FINALIZE = 0x1fea630,
    SYSTEM_FINALIZE_CALL = 0x1fb4978,
    OPEN_SECTION_FLOW = 0x1fb4c70, /* (out, ctx, std::function*) */
    ROW_FUNCTOR_VTABLE = 0x573d120,
    OPENER_FUNCTOR_VTABLE = 0x5743880,
    SECTION_NAME_TABLE = 0x57398b0,
    SECTION_COUNT_IMM = 0x1f20e21, /* the 6 in cmp rdx, 6 */
    TITLE_FINALIZE = 0x1fea820, TITLE_FINALIZE_CALL = 0x1f4adb3,
    BOX_FROM_CAPTION = 0x201ed10, COMMAND_FROM_ACTION = 0x1d5db60, POPUP_FROM_BOX = 0x201a070,
};
/* The callers of LIST_UPDATE that do not go through the vtable (bbhost menu_pointer.cpp). */
static const struct { uint32_t at; unsigned char op; } LIST_UPDATE_CALLS[] = {
    {0x1f25226, 0xe8}, {0x1f26066, 0xe8}, {0x1f26f28, 0xe8}, {0x1f72c80, 0xe8}, {0x1f108c7, 0xe9},
};
enum { MSG_MENU_TEXT = 0xc8, MSG_LINE_HELP = 0xc9, MSG_DIALOG_TEXT = 0xcc, EXIT_GAME_ROW_ID = 0x1adb4 };
enum { BUILDER_BEGIN = 0x68, BUILDER_END = 0x70, BUILDER_ROW = 0x180 };
/* A section dialog: its row records (a caption pair and the widget), the rows' list component;
 * a pick-list widget's drop-down list and the value pointer it is bound to. */
enum { ITEMS_BEGIN = 0xe58, ITEMS_END = 0xe60, ITEM_STRIDE = 0x90, ITEM_WIDGET = 0x80,
       DIALOG_ROWS = 0xa70, PICK_ITEMS = 0x100, PICK_BOUND = 0x3c8 };
enum { ROW_QUIT = 122000, QUIT_QUESTION = 920001, ROW_RESTART = 110017, RESTART_QUESTION = 920002 };

typedef int64_t (BB_GUEST_ABI *GuestFn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

static int g_installed;
static char g_open_name[32] = "PCSetting";

static GuestFn fn(uint64_t bn) { return (GuestFn)bb_guest(BB_BN(bn)); }
static uint64_t addr(uint64_t bn) { return (uint64_t)(uintptr_t)bb_guest(BB_BN(bn)); }
static uint64_t ptr(const void *p) { return (uint64_t)(uintptr_t)p; }
static void *at(uint64_t a) { return (void *)(uintptr_t)a; }
static uint64_t rd64(const void *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}
static void wr64(void *p, uint64_t v) { memcpy(p, &v, 8); }

/* --- Engine objects ----------------------------------------------------------------------- */

/* A row's caption pair: the label at +0x00 and its line help at +0x40, each the raw UTF-16
 * pointer drawn at +0x00 and an MSVC wstring copy at +0x08. */
typedef struct {
    _Alignas(16) unsigned char buf[0x80];
} Captions;

/* MSVC _String_val at `ws`: the buffer pointer at +0, the capacity at +0x18 (below 8 is the
 * inline buffer), the allocator at +0x20 whose vtable slot +0x70 frees. */
static void free_wstring(unsigned char *ws) {
    const uint64_t capacity = rd64(ws + 0x18), buffer = rd64(ws), allocator = rd64(ws + 0x20);
    if (capacity < 8 || !buffer || !allocator) return;
    const uint64_t vtable = rd64(at(allocator));
    ((GuestFn)at(rd64(at(vtable + 0x70))))(allocator, buffer, 0, 0, 0, 0);
}

static void captions_init(Captions *c, uint32_t id) {
    memset(c, 0, sizeof(*c));
    fn(MSG_REPOSITORY)(ptr(c->buf), MSG_MENU_TEXT, id, 0, 0, 0);
    fn(MSG_REPOSITORY)(ptr(c->buf + 0x40), MSG_LINE_HELP, id, 0, 0, 0);
}

static void captions_free(Captions *c) {
    free_wstring(c->buf + 0x08);
    free_wstring(c->buf + 0x48);
}

/* Copies a message's text (the repository's) into `out` (at most n - 1 units). */
static void message_copy(uint32_t id, uint16_t *out, size_t n) {
    Captions c;
    captions_init(&c, id);
    const uint16_t *text = at(rd64(c.buf));
    size_t i = 0;
    for (; text && text[i] && i + 1 < n; ++i) out[i] = text[i];
    out[i] = 0;
    captions_free(&c);
}

/* An engine std::function (MSVC): 0x20 bytes of inline storage holding the callable - its
 * vtable and the wrapped function pointer - then a pointer to that storage at +0x20. */
typedef struct {
    _Alignas(16) unsigned char buf[0x28];
} GuestFunction;

static void function_init(GuestFunction *f, uint64_t vtable_bn, const void *target) {
    memset(f, 0, sizeof(*f));
    wr64(f->buf, addr(vtable_bn));
    wr64(f->buf + 8, ptr(target));
    wr64(f->buf + 0x20, ptr(f->buf));
}

/* DLReferenceCountObject: the count at +8; the last reference calls the deleting destructor. */
static void release_ref(uint64_t object) {
    if (!object) return;
    int32_t count;
    memcpy(&count, at(object + 8), 4);
    const int32_t left = count - 1;
    memcpy(at(object + 8), &left, 4);
    if (count == 1) ((GuestFn)at(rd64(at(rd64(at(object))))))(object, 0, 0, 0, 0, 0);
}

/* --- Rows and sections -------------------------------------------------------------------- */

enum { TOGGLE, PICK };
enum { MAX_OPTIONS = 8, TEXT = 48, MAX_ROWS = 6 };

typedef struct {
    int32_t value;
    uint32_t id;  /* the choice's message */
} Option;

typedef struct {
    const char *key; /* the bbport.ini setting (bbgpu_setting_*) */
    uint32_t id;     /* the row's caption and line help */
    int kind;
    const Option *options; /* PICK: every choice, in order */
    int option_count;
    /* While its section is open: the value the row writes, what was last pushed, the choices
     * shown (a GPU without DLSS gets no DLSS) and their text, which the menu owns. */
    uint8_t toggle;
    int32_t pick, last;
    Option shown[MAX_OPTIONS];
    int shown_count;
    uint16_t text[MAX_OPTIONS][TEXT];
    uint16_t first[MAX_OPTIONS]; /* each text's first unit while blanked */
} Row;

typedef struct {
    const char *name;    /* the movie's sprite (menu_assets.py SECTION_ROWS) */
    uint32_t system_row; /* the System row that opens it */
    Row *rows;
    int count;
} Section;

static const Option UPSCALERS[] = {{0, 117100}, {1, 117101}, {2, 117102}, {3, 117103}, {4, 117104}, {5, 117105}};
static const Option PRESETS[] = {{0, 117110}, {1, 117111}, {2, 117112}, {3, 117113}, {4, 117114}};
/* The 16:9 outputs, 1920x1200, and the two common ultrawide ones; other outputs show only
 * while chosen (from the launcher), so the list stays as short as the others. Values index
 * BbSettings::OutputWidths. */
enum { OUTPUTS_ALWAYS = 8 };
static const Option OUTPUTS[] = {{0, 125100}, {13, 125114}, {1, 125101}, {2, 125102}, {3, 125103},
                                 {12, 125113}, {5, 125104}, {6, 125105}, {10, 125106}, {8, 125107},
                                 {11, 125108}, {7, 125109}};
static const Option LIVE[] = {{-1, 125110}, {0, 125111}, {1, 125112}};
static const Option LODS[] = {{-2, 125120}, {-1, 125124}, {0, 125121}, {1, 125122}, {2, 125123}};
#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define TOGGLE_ROW(key, id) {key, id, TOGGLE, NULL, 0, 0, 0, 0, {{0, 0}}, 0, {{0}}, {0}}
#define PICK_ROW(key, id, list) {key, id, PICK, list, COUNT(list), 0, 0, 0, {{0, 0}}, 0, {{0}}, {0}}

/* Pick lists last in every section (their drop-down covers what is below them). Growing PC
 * Settings or PC Gameplay past these rows (for the mouse settings) crashed the menu movie
 * (guest 0xdcf79/0xd67bb): the menus' Scaleform heap ran out (menu_heap_patch.h). The mouse settings stay in the
 * launcher and bbport.ini. */
static Row g_graphics[] = {
    TOGGLE_ROW("sharpen", 117000), TOGGLE_ROW("jitter", 117001), TOGGLE_ROW("object_motion", 117002),
    PICK_ROW("upscaler", 117003, UPSCALERS), PICK_ROW("preset", 117004, PRESETS),
};
static Row g_display[] = {
    TOGGLE_ROW("fullscreen", 125004), TOGGLE_ROW("show_fps", 125000), PICK_ROW("output_res", 125001, OUTPUTS),
    PICK_ROW("live_resolution", 125002, LIVE), PICK_ROW("model_lod", 125003, LODS),
};
static Row g_effects[] = {
    TOGGLE_ROW("effect_chromatic_aberration", 120000), TOGGLE_ROW("effect_dof", 120001),
    TOGGLE_ROW("effect_motion_blur", 120002), TOGGLE_ROW("effect_ssao", 120003),
    TOGGLE_ROW("effect_game_aa", 120004), TOGGLE_ROW("effect_dynamic_shadows", 120005),
};
static Row g_settings[] = {
    TOGGLE_ROW("skip_intro", 116004), TOGGLE_ROW("effect_ssr", 116008),
    TOGGLE_ROW("debug_camera", 116009), TOGGLE_ROW("debug_menu", 116010),
};
static Row g_gameplay[] = {
    TOGGLE_ROW("tweak_no_rally", 126000), TOGGLE_ROW("tweak_camera_distance", 126001),
    TOGGLE_ROW("tweak_no_camera_rotation", 126002), TOGGLE_ROW("tweak_easy_run", 126003),
    TOGGLE_ROW("tweak_ragdoll", 126004),
};
static Row g_cheats[] = {
    TOGGLE_ROW("cheat_no_death", 127000), TOGGLE_ROW("cheat_stealth", 127001),
    TOGGLE_ROW("cheat_silent", 127002), TOGGLE_ROW("cheat_rally_no_decay", 127003),
    TOGGLE_ROW("cheat_enemy_control", 127004),
};
static Row g_enhance[] = {
    TOGGLE_ROW("pc_mirror_appearance", 124001), TOGGLE_ROW("pc_two_invaders", 124003),
    TOGGLE_ROW("pc_rebirth", 124002),
};
static Section g_sections[] = {
    {"PCGraphics", 110007, g_graphics, COUNT(g_graphics)},
    {"PCDisplay", 110014, g_display, COUNT(g_display)},
    {"PCEffects", 110010, g_effects, COUNT(g_effects)},
    {"PCSetting", 110006, g_settings, COUNT(g_settings)},
    {"PCGameplay", 110015, g_gameplay, COUNT(g_gameplay)},
    {"PCCheats", 110016, g_cheats, COUNT(g_cheats)},
    {"PCEnhance", 110013, g_enhance, COUNT(g_enhance)},
};

static int setting(const char *key) {
    int value = 0;
    bbgpu_setting_get(key, &value);
    return value;
}

static int setting_default(const char *key) {
    int value = 0;
    bbgpu_setting_default(key, &value);
    return value;
}

/* The row opens showing the setting as it is now. */
static void seed(Row *r) {
    if (r->kind == TOGGLE) {
        r->toggle = setting(r->key) > 0;
        r->last = r->toggle;
        return;
    }
    r->shown_count = 0;
    const int now = setting(r->key);
    for (int i = 0; i < r->option_count && r->shown_count < MAX_OPTIONS; ++i) {
        if (!strcmp(r->key, "upscaler") && !bbgpu_upscaler_supported(r->options[i].value)) continue;
        if (r->options == OUTPUTS && i >= OUTPUTS_ALWAYS && r->options[i].value != now) continue;
        r->shown[r->shown_count++] = r->options[i];
    }
    r->pick = r->shown_count ? r->shown[0].value : 0;
    for (int i = 0; i < r->shown_count; ++i)
        if (r->shown[i].value == now) r->pick = now;
    r->last = r->pick;
}

static void push(Row *r) {
    const int32_t value = r->kind == TOGGLE ? r->toggle : r->pick;
    if (value == r->last) return;
    r->last = value;
    bbgpu_setting_set(r->key, value);
    printf("PC menu: %s = %d\n", r->key, (int)value);
}

/* An On/Off row from the game's own list; its default is what the section's Defaults row
 * restores (the base's default for the setting). */
static void add_toggle_row(void *dialog, Row *r) {
    Captions captions;
    captions_init(&captions, r->id);
    _Alignas(16) unsigned char list[0x100];
    memset(list, 0, sizeof(list));
    uint8_t def = setting_default(r->key) > 0;
    fn(BUILD_ON_OFF)(ptr(list), 0, 0, 0, 0, 0);
    fn(ADD_CHOICE_ROW)(ptr(dialog), ptr(captions.buf), ptr(&r->toggle), ptr(list), ptr(&def), 0);
    const uint64_t entries = rd64(list + 0x98);
    for (uint64_t k = 0; k < entries && k < 2; ++k) free_wstring(list + k * 0x48 + 0x18);
    captions_free(&captions);
}

/* A pick list. Its entries are 0x48 bytes: the int32 value at +0, the raw UTF-16 pointer drawn
 * at +8 (the menu's own buffer), a wstring copy at +0x10 and a flag at +0x40; the container is
 * an inline array with its count at +0x908 and deep-copies each entry. */
static void add_pick_row(void *dialog, Row *r) {
    Captions captions;
    captions_init(&captions, r->id);
    static _Alignas(16) unsigned char list[0x1000];
    memset(list, 0, sizeof(list));
    int32_t def = r->shown_count ? r->shown[0].value : 0;
    const int wanted = setting_default(r->key);
    for (int i = 0; i < r->shown_count; ++i) {
        message_copy(r->shown[i].id, r->text[i], TEXT);
        if (r->shown[i].value == wanted) def = wanted;
        _Alignas(16) unsigned char entry[0x48];
        memset(entry, 0, sizeof(entry));
        memcpy(entry, &r->shown[i].value, 4);
        wr64(entry + 8, ptr(r->text[i]));
        fn(WSTR_COPY)(ptr(entry + 0x10), ptr(r->text[i]), 0, 0, 0, 0);
        entry[0x40] = 1;
        fn(LIST_APPEND)(ptr(list), ptr(entry), 0, 0, 0, 0);
        free_wstring(entry + 0x18);
    }
    fn(ADD_LIST_ROW)(ptr(dialog), ptr(captions.buf), ptr(&r->pick), ptr(list), ptr(&def), 0);
    for (int i = 0; i < r->shown_count; ++i) free_wstring(list + i * 0x48 + 0x18);
    captions_free(&captions);
}

/* --- The open section, and blanking under a pick list ----------------------------------- */

static struct {
    Section *section;
    unsigned char *dialog;
    unsigned char *widget[MAX_ROWS];         /* each row's widget */
    uint16_t caption[MAX_ROWS + 1][TEXT];     /* the rows' captions, then Defaults', ours */
    uint16_t caption_first[MAX_ROWS + 1];
    int records;                              /* rows laid out with our captions */
    int blank_from;                           /* the first row blanked, or -1 */
} g_open = {NULL, NULL, {NULL}, {{0}}, {0}, 0, -1};

static unsigned char *record(int row) {
    return (unsigned char *)at(rd64(g_open.dialog + ITEMS_BEGIN)) + (size_t)row * ITEM_STRIDE;
}

static int record_count(void) {
    const uint64_t begin = rd64(g_open.dialog + ITEMS_BEGIN), end = rd64(g_open.dialog + ITEMS_END);
    if (!begin || end < begin || (end - begin) % ITEM_STRIDE) return 0;
    return (int)((end - begin) / ITEM_STRIDE);
}

/* The dialog is freed when it closes and its memory reused: it is still ours while every pick
 * row's widget is a drop-down bound to that row's value. */
static int open_alive(void) {
    if (!g_open.dialog || !g_open.section || record_count() < g_open.section->count) return 0;
    for (int r = 0; r < g_open.section->count; ++r) {
        Row *row = &g_open.section->rows[r];
        unsigned char *w = at(rd64(record(r) + ITEM_WIDGET));
        if (w != g_open.widget[r]) return 0;
        if (row->kind == PICK && (rd64(w) != addr(DROPDOWN_VTABLE) || rd64(w + PICK_BOUND) != ptr(&row->pick)))
            return 0;
    }
    return 1;
}

/* After the dialog is made: the rows (and Defaults) draw captions from buffers of the menu's
 * own, laid out again, so they can be emptied under an open pick list. */
static void take_captions(void) {
    const int n = record_count();
    g_open.records = 0;
    if (n < g_open.section->count || n > MAX_ROWS + 1) return;
    for (int r = 0; r < n; ++r) {
        unsigned char *rec = record(r);
        const uint16_t *text = at(rd64(rec));
        size_t i = 0;
        for (; text && text[i] && i + 1 < TEXT; ++i) g_open.caption[r][i] = text[i];
        g_open.caption[r][i] = 0;
        wr64(rec, ptr(g_open.caption[r]));
        if (r < g_open.section->count) g_open.widget[r] = at(rd64(rec + ITEM_WIDGET));
    }
    g_open.records = n;
    fn(LAYOUT_ROWS)(ptr(g_open.dialog), 0, 0, 0, 0, 0);
}

static void set_blank(int from) {
    if (from == g_open.blank_from) return;
    const int restore = g_open.blank_from;
    g_open.blank_from = from;
    for (int r = 0; r < g_open.records; ++r) {
        const int was = restore >= 0 && r >= restore, now = from >= 0 && r >= from;
        if (was == now) continue;
        if (now) {
            g_open.caption_first[r] = g_open.caption[r][0];
            g_open.caption[r][0] = 0;
        } else {
            g_open.caption[r][0] = g_open.caption_first[r];
        }
        if (r < g_open.section->count && g_open.section->rows[r].kind == PICK) {
            Row *row = &g_open.section->rows[r];
            for (int i = 0; i < row->shown_count; ++i) {
                if (now) {
                    row->first[i] = row->text[i][0];
                    row->text[i][0] = 0;
                } else {
                    row->text[i][0] = row->first[i];
                }
            }
            fn(DROPDOWN_REDRAW)(ptr(g_open.widget[r]), 0, 0, 0, 0, 0);
        }
    }
    fn(LIST_REDRAW)(ptr(g_open.dialog + DIALOG_ROWS), 0, 0, 0, 0, 0);
}

static void ach_list_update(unsigned char *comp);  /* the Achievements section's paging */

/* Every menu list's update: a pick list taking the input blanks the rows under it; the rows
 * taking it back restore them. Achievements turns its pages. Then the game's own update. */
static BB_GUEST_ABI int64_t list_update(unsigned char *comp, unsigned char *input) {
    bb_menu_pointer_step(comp, input); /* before the pad, so a direction moves on from the pointer's row */
    if (input && *input) ach_list_update(comp);
    if (g_open.dialog && input && *input) {
        if (!open_alive()) {
            g_open.dialog = NULL;
            g_open.blank_from = -1;
        } else if (comp == g_open.dialog + DIALOG_ROWS) {
            set_blank(-1);
        } else {
            for (int r = 0; r < g_open.section->count; ++r)
                if (g_open.section->rows[r].kind == PICK && comp == g_open.widget[r] + PICK_ITEMS)
                    set_blank(r + 1);
        }
    }
    return fn(LIST_UPDATE)(ptr(comp), ptr(input), 0, 0, 0, 0);
}

/* --- Opening a section -------------------------------------------------------------------- */

static void build_section(Section *s, void *dialog) {
    _Alignas(16) unsigned char scratch[0x100];
    memset(scratch, 0, sizeof(scratch));
    fn(SCRATCH_INIT)(ptr(scratch), 0, 0, 0, 0, 0);
    for (int i = 0; i < s->count; ++i) {
        seed(&s->rows[i]);
        if (s->rows[i].kind == TOGGLE) add_toggle_row(dialog, &s->rows[i]);
        else add_pick_row(dialog, &s->rows[i]);
    }
}

static int64_t open_section(Section *s, void *root, void *params, const void *handler) {
    snprintf(g_open_name, sizeof(g_open_name), "%s", s->name);
    g_open.dialog = NULL;
    g_open.blank_from = -1;
    const int64_t dialog = fn(OPEN_SECTION)(ptr(root), ptr(params), ptr(g_open_name), ptr(handler), 0, 0);
    if (dialog) {
        g_open.section = s;
        g_open.dialog = at((uint64_t)dialog);
        take_captions();
        printf("PC menu: %s opened\n", s->name);
    }
    return dialog;
}

static void *row_open(void *out, void *ctx, const void *opener) {
    GuestFunction f;
    function_init(&f, OPENER_FUNCTOR_VTABLE, opener);
    fn(OPEN_SECTION_FLOW)(ptr(out), ptr(ctx), ptr(f.buf), 0, 0, 0);
    return out;
}

/* One handler, opener and System row function per section. */
#define SECTION_FUNCTIONS(n)                                                                      \
    static BB_GUEST_ABI void handler_##n(void *dialog, void *params) {                            \
        (void)params;                                                                             \
        build_section(&g_sections[n], dialog);                                                    \
    }                                                                                             \
    static BB_GUEST_ABI int64_t opener_##n(void *root, void *params) {                            \
        return open_section(&g_sections[n], root, params, (const void *)handler_##n);             \
    }                                                                                             \
    static BB_GUEST_ABI void *row_##n(void *out, void *ctx) {                                     \
        return row_open(out, ctx, (const void *)opener_##n);                                     \
    }
SECTION_FUNCTIONS(0)
SECTION_FUNCTIONS(1)
SECTION_FUNCTIONS(2)
SECTION_FUNCTIONS(3)
SECTION_FUNCTIONS(4)
SECTION_FUNCTIONS(5)
SECTION_FUNCTIONS(6)
static const void *const SECTION_ROWS[] = {
    (const void *)row_0, (const void *)row_1, (const void *)row_2, (const void *)row_3,
    (const void *)row_4, (const void *)row_5, (const void *)row_6,
};
static const int NATIVE_SECTION_INDEX[] = {0, 1, 2, 3, 4};  /* Cheats and Enhancements stay in the overlay. */
_Static_assert(sizeof(SECTION_ROWS) / sizeof(SECTION_ROWS[0]) == sizeof(g_sections) / sizeof(g_sections[0]),
               "one row function per section");

/* --- Achievements -------------------------------------------------------------------------- */
/* A section of six one-entry choice rows whose caption, line help and value are the menu's own
 * buffers, as bbhost's Key Bindings screen does it: a summary row, then five achievements. Left
 * or Right on any row turns the page, which refills the buffers and redraws the lists - the
 * rows draw the raw pointers they were built with. Read only: the source of truth is
 * BbTrophies (the game's own trophy pack and the local unlock history). */
enum {
    ACH_ROWS = 6, ACH_PER_PAGE = 5, ACH_TEXT = 96, ACH_VALUE = 32,
    CHOICE_APPEND = 0x1f2c200,  /* (two-entry container, entry*) */
    CHOICE_LIST = 0xa0, CHOICE_VALUE = 0x360, DEFAULTS_ROW_ID = 0x9d8a,
    ROW_ACHIEVEMENTS = 110018, ACH_FIRST_ROW = 128000,
    PAD_RIGHT = 0x20, PAD_LEFT = 0x80,
};
static struct {
    unsigned char *dialog;
    unsigned char *widget[ACH_ROWS];
    uint8_t value[ACH_ROWS];
    uint16_t label[ACH_ROWS][ACH_TEXT], help[ACH_ROWS][ACH_TEXT], text[ACH_ROWS][ACH_VALUE];
    int page;
    uint32_t pad_was;
} g_ach;

/* UTF-8 into UTF-16, cut to fit `n` units (with "..." when cut). */
static void to_utf16(uint16_t *out, size_t n, const char *in) {
    size_t o = 0;
    const unsigned char *s = (const unsigned char *)in;
    while (*s && o + 1 < n) {
        uint32_t c = *s++;
        int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        c &= extra == 3 ? 0x07 : extra == 2 ? 0x0f : extra == 1 ? 0x1f : 0x7f;
        for (; extra && (*s & 0xc0) == 0x80; --extra) c = (c << 6) | (*s++ & 0x3f);
        if (c >= 0x10000) {
            if (o + 2 >= n) break;
            c -= 0x10000;
            out[o++] = (uint16_t)(0xd800 + (c >> 10));
            out[o++] = (uint16_t)(0xdc00 + (c & 0x3ff));
        } else {
            out[o++] = (uint16_t)c;
        }
    }
    if (*s && n > 4) {
        o = o < n - 4 ? o : n - 4;
        out[o++] = '.', out[o++] = '.', out[o++] = '.';
    }
    out[o] = 0;
}

static void ach_fill(int page) {
    const int count = bbgpu_achievement_count();
    const int pages = count ? (count + ACH_PER_PAGE - 1) / ACH_PER_PAGE : 1;
    g_ach.page = ((page % pages) + pages) % pages;
    int earned = 0;
    BbAchievement a;
    for (int i = 0; i < count; ++i) earned += bbgpu_achievement_get(i, &a) && a.unlocked_at > 0;
    char line[256];
    to_utf16(g_ach.label[0], ACH_TEXT, "Achievements");
    snprintf(line, sizeof(line), "Page %d / %d", g_ach.page + 1, pages);
    to_utf16(g_ach.text[0], ACH_VALUE, line);
    int score = 0, total = 0;
    if (!count)
        snprintf(line, sizeof(line), "No trophy pack in the game folder.");
    else if (bbgpu_achievement_score(&score, &total))
        snprintf(line, sizeof(line), "%d of %d unlocked (%d%%), %d / 1000G", earned, count, earned * 100 / count, score);
    else
        snprintf(line, sizeof(line), "%d of %d unlocked (%d%%). Left/right: more", earned, count, earned * 100 / count);
    to_utf16(g_ach.help[0], 53, line);
    for (int r = 1; r < ACH_ROWS; ++r) {
        const int index = g_ach.page * ACH_PER_PAGE + r - 1;
        if (index >= count || !bbgpu_achievement_get(index, &a)) {
            g_ach.label[r][0] = g_ach.help[r][0] = g_ach.text[r][0] = 0;
            continue;
        }
        const int got = a.unlocked_at > 0, secret = a.hidden && !got;
        if (secret) {
            to_utf16(g_ach.label[r], ACH_TEXT, "Secret Achievement");
            to_utf16(g_ach.help[r], 53, "Unlock it to reveal it.");
        } else if (!a.name[0]) {
            snprintf(line, sizeof(line), "Achievement %d", a.id + 1);
            to_utf16(g_ach.label[r], ACH_TEXT, line);
            to_utf16(g_ach.help[r], 53, "Names need your trophy key (launcher, Game).");
        } else {
            to_utf16(g_ach.label[r], 41, a.name);
            to_utf16(g_ach.help[r], 53, a.description);  /* the help bar shows about 52 */
        }
        if (got) {
            const time_t when = (time_t)a.unlocked_at;
            const struct tm *local = localtime(&when);
            if (!local || !strftime(line, sizeof(line), "%d %b %Y", local)) snprintf(line, sizeof(line), "Unlocked");
        } else {
            snprintf(line, sizeof(line), "Locked");
        }
        to_utf16(g_ach.text[r], ACH_VALUE, line);
    }
}

/* A row whose caption, help and only value are the menu's own text. */
static void add_text_row(void *dialog, int r) {
    Captions captions;
    captions_init(&captions, ACH_FIRST_ROW + (uint32_t)r);
    wr64(captions.buf, ptr(g_ach.label[r]));
    wr64(captions.buf + 0x40, ptr(g_ach.help[r]));
    _Alignas(16) unsigned char list[0x100], entry[0x48];
    memset(list, 0, sizeof(list));
    memset(entry, 0, sizeof(entry));
    wr64(entry + 8, ptr(g_ach.text[r]));
    fn(WSTR_COPY)(ptr(entry + 0x10), ptr(g_ach.text[r]), 0, 0, 0, 0);
    entry[0x40] = 1;
    fn(CHOICE_APPEND)(ptr(list), ptr(entry), 0, 0, 0, 0);
    free_wstring(entry + 0x18);
    g_ach.value[r] = 0;
    uint8_t def = 0;
    fn(ADD_CHOICE_ROW)(ptr(dialog), ptr(captions.buf), ptr(&g_ach.value[r]), ptr(list), ptr(&def), 0);
    free_wstring(list + 0x18);
    captions_free(&captions);
}

static unsigned char *dialog_record(unsigned char *dialog, int row) {
    return (unsigned char *)at(rd64(dialog + ITEMS_BEGIN)) + (size_t)row * ITEM_STRIDE;
}

static int dialog_records(unsigned char *dialog) {
    const uint64_t begin = rd64(dialog + ITEMS_BEGIN), end = rd64(dialog + ITEMS_END);
    if (!begin || end < begin || (end - begin) % ITEM_STRIDE) return 0;
    return (int)((end - begin) / ITEM_STRIDE);
}

/* The section has nothing to restore: the game's Defaults row comes off again, the way a
 * vector element is destroyed (its caption copies freed, its widget unreferenced). */
static void drop_defaults_row(unsigned char *dialog) {
    const int n = dialog_records(dialog);
    if (n != ACH_ROWS + 1) return;
    Captions defaults;
    captions_init(&defaults, DEFAULTS_ROW_ID);
    const uint64_t text = rd64(defaults.buf);
    captions_free(&defaults);
    unsigned char *rec = dialog_record(dialog, n - 1);
    if (rd64(rec) != text) {
        puts("PC menu: Achievements: the last row is not Defaults; left alone");
        return;
    }
    free_wstring(rec + 0x08);
    free_wstring(rec + 0x48);
    release_ref(rd64(rec + ITEM_WIDGET));
    wr64(dialog + ITEMS_END, rd64(dialog + ITEMS_END) - ITEM_STRIDE);
    fn(LAYOUT_ROWS)(ptr(dialog), 0, 0, 0, 0, 0);
}

static int ach_alive(void) {
    if (!g_ach.dialog || dialog_records(g_ach.dialog) < ACH_ROWS) return 0;
    for (int r = 0; r < ACH_ROWS; ++r) {
        unsigned char *w = at(rd64(dialog_record(g_ach.dialog, r) + ITEM_WIDGET));
        if (w != g_ach.widget[r] || rd64(w + CHOICE_VALUE) != ptr(&g_ach.value[r])) return 0;
    }
    return 1;
}

/* From the list update: Left or Right, newly pressed on any of the section's lists, turns the
 * page. */
static void ach_update(unsigned char *comp) {
    int ours = comp == g_ach.dialog + DIALOG_ROWS;
    for (int r = 0; r < ACH_ROWS && !ours; ++r) ours = comp == g_ach.widget[r] + CHOICE_LIST;
    if (!ours) return;
    const uint32_t pad = runtime_pad_buttons(), edge = pad & ~g_ach.pad_was;
    g_ach.pad_was = pad;
    if (!(edge & (PAD_LEFT | PAD_RIGHT))) return;
    ach_fill(g_ach.page + ((edge & PAD_LEFT) ? -1 : 1));
    fn(LIST_REDRAW)(ptr(g_ach.dialog + DIALOG_ROWS), 0, 0, 0, 0, 0);
    for (int r = 0; r < ACH_ROWS; ++r) fn(LIST_REDRAW)(ptr(g_ach.widget[r] + CHOICE_LIST), 0, 0, 0, 0, 0);
}

static void ach_list_update(unsigned char *comp) {
    if (!g_ach.dialog) return;
    if (!ach_alive()) {
        g_ach.dialog = NULL;
        return;
    }
    ach_update(comp);
}

static BB_GUEST_ABI void ach_handler(void *dialog, void *params) {
    (void)params;
    _Alignas(16) unsigned char scratch[0x100];
    memset(scratch, 0, sizeof(scratch));
    fn(SCRATCH_INIT)(ptr(scratch), 0, 0, 0, 0, 0);
    g_ach.pad_was = runtime_pad_buttons();  /* the press that opened the section is not a page turn */
    ach_fill(g_ach.page);
    for (int r = 0; r < ACH_ROWS; ++r) add_text_row(dialog, r);
}

static BB_GUEST_ABI int64_t ach_opener(void *root, void *params) {
    snprintf(g_open_name, sizeof(g_open_name), "%s", "PCAchieve");
    g_open.dialog = NULL;
    g_ach.dialog = NULL;
    const int64_t dialog = fn(OPEN_SECTION)(ptr(root), ptr(params), ptr(g_open_name), ptr((const void *)ach_handler), 0, 0);
    if (dialog) {
        unsigned char *d = at((uint64_t)dialog);
        drop_defaults_row(d);
        for (int r = 0; r < ACH_ROWS && r < dialog_records(d); ++r) g_ach.widget[r] = at(rd64(dialog_record(d, r) + ITEM_WIDGET));
        g_ach.dialog = d;
        if (!ach_alive()) {
            puts("PC menu: Achievements: the dialog is not laid out as expected; no paging");
            g_ach.dialog = NULL;
        }
        puts("PC menu: Achievements opened");
    }
    return dialog;
}

static BB_GUEST_ABI void *ach_row(void *out, void *ctx) { return row_open(out, ctx, (const void *)ach_opener); }

/* --- Yes/no commands: Quit Game and Restart to Apply ------------------------------------- */
/* Built like the game's own Exit Game opener (sub_200c9f0): a message box from a dialog caption
 * (sub_201ed10), a YES command around an action (sub_1d5db60) and the popup of the two
 * (sub_201a070), so the question is the menu's own, modal, with YES and NO. */

static BB_GUEST_ABI int64_t quit_yes(int64_t a, int64_t b) {
    (void)a, (void)b;
    puts("PC menu: Quit Game");
    bbgpu_request_quit();
    return 0;
}

static BB_GUEST_ABI int64_t restart_yes(int64_t a, int64_t b) {
    (void)a, (void)b;
    puts("PC menu: Restart to Apply");
    runtime_restart();
    return 0;
}

static void *question(void *out, void *factory, uint32_t message, const void *yes) {
    _Alignas(16) unsigned char caption[0x40];
    memset(caption, 0, sizeof(caption));
    fn(MSG_REPOSITORY)(ptr(caption), MSG_DIALOG_TEXT, message, 0, 0, 0);
    uint64_t box = 0, command = 0;
    fn(BOX_FROM_CAPTION)(ptr(&box), ptr(factory), ptr(caption), 0, 0, 0);
    GuestFunction action;
    function_init(&action, OPENER_FUNCTOR_VTABLE, yes);
    fn(COMMAND_FROM_ACTION)(ptr(&command), ptr(action.buf), 2, 0, 0, 0);
    fn(POPUP_FROM_BOX)(ptr(out), ptr(&box), ptr(&command), 0, 0, 0);
    release_ref(command);
    release_ref(box);
    free_wstring(caption + 0x08);
    return out;
}

static BB_GUEST_ABI void *quit_opener(void *out, void *factory) {
    return question(out, factory, QUIT_QUESTION, (const void *)quit_yes);
}

static BB_GUEST_ABI void *restart_opener(void *out, void *factory) {
    return question(out, factory, RESTART_QUESTION, (const void *)restart_yes);
}

static void add_command(void *builder, uint32_t id, const void *open, int section_opener) {
    Captions captions;
    GuestFunction f;
    int64_t flag = 0;
    captions_init(&captions, id);
    function_init(&f, ROW_FUNCTOR_VTABLE, open);
    if (section_opener)
        fn(ADD_COMMAND_ROW)(ptr(builder), ptr(captions.buf), ptr(f.buf), ptr(&flag), 0, 0);
    else
        fn(ADD_EXIT_ROW)(ptr(builder), ptr(captions.buf), ptr(f.buf), 0, 0, 0);
    captions_free(&captions);
}

/* --- The System and title lists ----------------------------------------------------------- */

static unsigned char *builder_last_row(void *builder) {
    const unsigned char *b = builder;
    const uint64_t begin = rd64(b + BUILDER_BEGIN), end = rd64(b + BUILDER_END);
    if (!begin || end <= begin || (end - begin) % BUILDER_ROW) return NULL;
    return at(end - BUILDER_ROW);
}

/* Exit Game's row holds its opener somewhere in its 0x180 bytes. */
static int row_is_exit_game(const unsigned char *row) {
    const uint64_t want = addr(EXIT_GAME_OPENER);
    for (size_t i = 0; i + 8 <= BUILDER_ROW; i += 8)
        if (rd64(row + i) == want) return 1;
    return 0;
}

/* The game's own pop: slot 1 of the row's vtable, then the end pointer back one row. */
static void pop_last_row(void *builder) {
    unsigned char *row = builder_last_row(builder);
    if (!row) return;
    ((GuestFn)at(rd64(at(rd64(row) + 8))))(ptr(row), 0, 0, 0, 0, 0);
    wr64((unsigned char *)builder + BUILDER_END, rd64((unsigned char *)builder + BUILDER_END) - BUILDER_ROW);
}

/* The System builder, just before it becomes a dialog: the menu's rows go last, except that
 * Exit Game (in game) comes off first and goes back on at the end, as the game's own code adds
 * and pops these rows. The list scrolls (the movie gives it a scroll bar). */
static BB_GUEST_ABI void *system_finalize(void *out, void *builder, void *arg3) {
    unsigned char *last = builder_last_row(builder);
    const int move_exit = last && row_is_exit_game(last);
    if (move_exit) pop_last_row(builder);
    add_command(builder, ROW_ACHIEVEMENTS, (const void *)ach_row, 1);
    for (int k = 0; k < COUNT(NATIVE_SECTION_INDEX); ++k) {
        const int i = NATIVE_SECTION_INDEX[k];
        add_command(builder, g_sections[i].system_row, SECTION_ROWS[i], 1);
    }
    add_command(builder, ROW_RESTART, (const void *)restart_opener, 0);
    if (move_exit) add_command(builder, EXIT_GAME_ROW_ID, bb_guest(BB_BN(EXIT_GAME_OPENER)), 0);
    fn(SYSTEM_FINALIZE)(ptr(out), ptr(builder), ptr(arg3), 0, 0, 0);
    return out;
}

/* The title's command list (Continue, Load Game, New Game, System, ...): Quit Game last. Its
 * line comes from title.gfx / title_dlc.gfx (menu_assets.py, BB_PC_MENU_QUIT=1). */
static BB_GUEST_ABI void *title_finalize(void *out, void *builder, void *arg3) {
    add_command(builder, ROW_QUIT, (const void *)quit_opener, 0);
    fn(TITLE_FINALIZE)(ptr(out), ptr(builder), ptr(arg3), 0, 0, 0);
    return out;
}

/* --- Install and frame -------------------------------------------------------------------- */

/* The menus' list update: its vtable slot and its five direct callers go to list_update. All
 * are checked before anything is written; a refusal leaves pick lists without blanking. */
static int g_lists_hooked;
static int hook_list_update(void) {
    static int done = -1;
    if (done >= 0) return 0;
    done = 0;
    const uint64_t slot = BB_BN(LIST_VTABLE) + 3 * 8, original = addr(LIST_UPDATE), ours = ptr((const void *)list_update);
    for (int i = 0; i < COUNT(LIST_UPDATE_CALLS); ++i) {
        const unsigned char *site = bb_guest(BB_BN(LIST_UPDATE_CALLS[i].at));
        int32_t rel;
        memcpy(&rel, site + 1, 4);
        if (site[0] != LIST_UPDATE_CALLS[i].op ||
            BB_BN(LIST_UPDATE_CALLS[i].at) + 5 + (int64_t)rel != BB_BN(LIST_UPDATE)) {
            puts("PC menu: the list update's callers are not as expected; pick lists are not blanked");
            return 0;
        }
    }
    if (bb_patch(slot, &original, &ours, 8)) {
        printf("PC menu: list vtable: %s; pick lists are not blanked\n", bb_hooks_error());
        return 0;
    }
    int hooks = 1;
    for (int i = 0; i < COUNT(LIST_UPDATE_CALLS); ++i)
        hooks += !bb_hook_call(BB_BN(LIST_UPDATE_CALLS[i].at), BB_BN(LIST_UPDATE), (void *)list_update);
    done = hooks;
    g_lists_hooked = 1;
    return hooks;
}

/* The menu pointer needs the list update even when the PC menu is off (its part then does
 * nothing: no section is ever open). */
int bb_menu_lists_hook(void) { return hook_list_update(); }
int bb_menu_lists_hooked(void) { return g_lists_hooked; }

int bb_pc_menu_install(void) {
    const char *menu = getenv("BB_PC_MENU"), *assets = getenv("BB_PC_MENU_ASSETS");
    if (menu && !strcmp(menu, "0")) {
        puts("PC menu: off (BB_PC_MENU=0)");
        return 0;
    }
    if (!assets || strcmp(assets, "1")) {
        puts("PC menu: off - its sections and strings were not made (see the Enhancements lines)");
        return 0;
    }
    /* Slot [6] of the section-name table points at the name being opened; the loop that walks
     * the table goes to seven. Both are checked before anything is written. */
    static const unsigned char six = 6, seven = 7, empty[16] = {0};
    unsigned char slot[16] = {0};
    wr64(slot, ptr(g_open_name));
    if (bb_patch(BB_BN(SECTION_COUNT_IMM), &six, &seven, 1)) {
        printf("PC menu: off - section loop: %s\n", bb_hooks_error());
        return 0;
    }
    if (bb_patch(BB_BN(SECTION_NAME_TABLE) + 6 * 8, empty, slot, 16)) {
        printf("PC menu: off - section table: %s\n", bb_hooks_error());
        bb_patch(BB_BN(SECTION_COUNT_IMM), &seven, &six, 1);
        return 0;
    }
    if (bb_hook_call(BB_BN(SYSTEM_FINALIZE_CALL), BB_BN(SYSTEM_FINALIZE), (void *)system_finalize)) {
        printf("PC menu: off - System list: %s\n", bb_hooks_error());
        bb_patch(BB_BN(SECTION_NAME_TABLE) + 6 * 8, slot, empty, 16);
        bb_patch(BB_BN(SECTION_COUNT_IMM), &seven, &six, 1);
        return 0;
    }
    g_installed = 1;
    int hooks = 3 + hook_list_update();
    printf("PC menu: System gains Achievements, %d PC sections and Restart to Apply\n", COUNT(NATIVE_SECTION_INDEX));
    const char *quit = getenv("BB_PC_MENU_QUIT");
    if (!quit || strcmp(quit, "1"))
        puts("PC menu: no Quit Game on the title (its line was not made)");
    else if (bb_hook_call(BB_BN(TITLE_FINALIZE_CALL), BB_BN(TITLE_FINALIZE), (void *)title_finalize))
        printf("PC menu: no Quit Game on the title - %s\n", bb_hooks_error());
    else {
        puts("PC menu: the title menu gains Quit Game");
        ++hooks;
    }
    return hooks;
}

void bb_pc_menu_frame(void) {
    if (!g_installed || !g_open.section) return;
    /* Only the open (or last opened) section's rows can change. */
    for (int i = 0; i < g_open.section->count; ++i) push(&g_open.section->rows[i]);
}
