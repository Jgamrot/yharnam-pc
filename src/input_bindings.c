/* SPDX-License-Identifier: GPL-2.0-or-later
 * Keyboard and mouse bindings (input_bindings.h). Actions, defaults and the way they reach the
 * pad after droogie's bbhost (https://github.com/droogie/bbhost, GPL-3.0-or-later,
 * host/bindings.cpp), which follows Dark Souls III PC. */
#include "input_bindings.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

/* PS4 pad bits (runtime_pad.c). */
enum {
    PAD_L3 = 0x2, PAD_R3 = 0x4, PAD_OPTIONS = 0x8, PAD_UP = 0x10, PAD_RIGHT = 0x20, PAD_DOWN = 0x40,
    PAD_LEFT = 0x80, PAD_L2 = 0x100, PAD_R2 = 0x200, PAD_L1 = 0x400, PAD_R1 = 0x800, PAD_TRIANGLE = 0x1000,
    PAD_CIRCLE = 0x2000, PAD_CROSS = 0x4000, PAD_SQUARE = 0x8000, PAD_TOUCHPAD = 0x100000,
};
/* Menus confirm with Cross and go back with Circle (the Western layout the game runs with). */
enum { CONFIRM = PAD_CROSS, BACK = PAD_CIRCLE };
enum { STICK_LOW = 0, STICK_REST = 128, STICK_HIGH = 255 };
/* A walk's share of a full push. The game's own dead zone swallows a push of about a third (0.35
 * left the character standing still while Walk was held, #88 / #205); 0.6 walks, as in
 * LanternEngine's bindings (kWalkPush), and stays below the run. */
static const float WALK_PUSH = 0.6f;

static const BindInfo INFO[BIND_COUNT] = {
    {"move_forward", "Move Forward", 0, "W", 0},
    {"move_back", "Move Back", 0, "S", 0},
    {"move_left", "Move Left", 0, "A", 0},
    {"move_right", "Move Right", 0, "D", 0},
    {"walk", "Walk", 0, "Left Alt", 0},
    {"roll", "Dodge / Sprint", PAD_CIRCLE, "Space", 0},
    {"lock_on", "Lock On", PAD_R3, "Q", 0},
    {"attack", "Attack", PAD_R1, NULL, 1},
    {"strong_attack", "Strong Attack", PAD_R2, "V", 0},
    {"strong_modifier", "Strong Attack Modifier", 0, "Left Shift", 0},
    {"transform", "Transform", PAD_L1, "C", 3},
    {"firearm", "Firearm / Parry", PAD_L2, "X", 2},
    {"blood_vial", "Blood Vial", PAD_TRIANGLE, "R", 0},
    {"use_item", "Use Item", PAD_SQUARE, "F", 0},
    {"interact", "Interact", PAD_CROSS, "E", 0},
    {"switch_item", "Switch Item", PAD_DOWN, "Down", 0},
    {"switch_right", "Switch Right Weapon", PAD_RIGHT, "Right", 0},
    {"switch_left", "Switch Left Weapon", PAD_LEFT, "Left", 0},
    {"dpad_up", "D-pad Up", PAD_UP, "Up", 0},
    {"gestures", "Gestures", PAD_TOUCHPAD, "G", 0},
    {"personal_effects", "Personal Effects", PAD_TOUCHPAD, "T", 0},
    {"look_up", "Look Up", 0, "I", 0},
    {"look_down", "Look Down", 0, "K", 0},
    {"look_left", "Look Left", 0, "J", 0},
    {"look_right", "Look Right", 0, "L", 0},
    {"menu", "Menu", PAD_OPTIONS, "Escape", 0},
    {"confirm", "Menu Confirm", CONFIRM, "Return", 0},
    {"back", "Menu Back", BACK, "Backspace", 0},
    {"l3", "Left Stick Press", PAD_L3, "Left Ctrl", 0},
};

static BindSlot g_slot[BIND_COUNT][BIND_SLOTS];
/* Movement smoothing: travel per 1/60 s as a share of a full push (1: off), and the stick. */
#define MOVE_SMOOTHING_DEFAULT 0.6f
static float g_move_step = MOVE_SMOOTHING_DEFAULT, g_move_x, g_move_y;
static double g_move_angle;
static bool g_move_active;
static uint64_t g_move_time;
static int g_pad_back_side;

static const char *const MOUSE_NAMES[] = {NULL, "mouse1", "mouse2", "mouse3", "mouse4", "mouse5", "wheelup", "wheeldown"};

int bb_bindings_pad_back_side(void) { return g_pad_back_side; }

const BindInfo *bb_bind_info(int action) { return action >= 0 && action < BIND_COUNT ? &INFO[action] : NULL; }

static bool is_mouse(int input) { return input < 0 && input >= -MOUSE_WHEEL_DOWN; }

int bb_binding_key(int action) {
    if (action < 0 || action >= BIND_COUNT) return 0;
    for (int s = 0; s < BIND_SLOTS; ++s) {
        const BindSlot *b = &g_slot[action][s];
        if (b->n == 1 && b->in[0] > 0) return b->in[0];
    }
    return 0;
}

int bb_binding_mouse(int action) {
    if (action < 0 || action >= BIND_COUNT) return 0;
    for (int s = 0; s < BIND_SLOTS; ++s) {
        const BindSlot *b = &g_slot[action][s];
        if (b->n == 1 && is_mouse(b->in[0])) return -b->in[0];
    }
    return 0;
}

int bb_binding_slots(int action, BindSlot *out, int max_slots) {
    if (action < 0 || action >= BIND_COUNT || !out || max_slots <= 0) return 0;
    int count = 0;
    for (int s = 0; s < BIND_SLOTS; ++s) {
        if (!g_slot[action][s].n) continue;
        if (count < max_slots) out[count] = g_slot[action][s];
        ++count;
    }
    return count;
}

static void clear_slots(int action) {
    for (int s = 0; s < BIND_SLOTS; ++s) {
        g_slot[action][s].n = 0;
        for (int i = 0; i < BIND_COMBO_PARTS; ++i) g_slot[action][s].in[i] = 0;
    }
}

static void defaults(void) {
    for (int a = 0; a < BIND_COUNT; ++a) {
        clear_slots(a);
        const int key = INFO[a].def_key ? (int)SDL_GetScancodeFromName(INFO[a].def_key) : 0;
        if (key) {
            g_slot[a][0].n = 1;
            g_slot[a][0].in[0] = key;
        }
        if (INFO[a].def_mouse) {
            /* A key and a mouse button both bound: either one presses it, as before. */
            const int slot = key ? 1 : 0;
            g_slot[a][slot].n = 1;
            g_slot[a][slot].in[0] = -INFO[a].def_mouse;
        }
    }
}

void bb_bindings_reset(void) { defaults(); }

void bb_binding_text(int action, char *out, int size) {
    static const char *const MOUSE_TEXT[] = {NULL, "Mouse1", "Mouse2", "Mouse3", "Mouse4", "Mouse5", "WheelUp", "WheelDown"};
    if (!out || size <= 0) return;
    out[0] = 0;
    if (action < 0 || action >= BIND_COUNT) return;
    int used = 0;
    for (int s = 0; s < BIND_SLOTS; ++s) {
        const BindSlot *b = &g_slot[action][s];
        for (int i = 0; i < b->n && used < size; ++i) {
            const char *name = is_mouse(b->in[i]) ? MOUSE_TEXT[-b->in[i]] : SDL_GetScancodeName((SDL_Scancode)b->in[i]);
            used += snprintf(out + used, (size_t)(size - used), "%s%s",
                             i ? "+" : used ? ", " : "", name);
        }
    }
    if (!used) snprintf(out, (size_t)size, "none");
}

static void trim(char *s) {
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    size_t i = 0;
    while (isspace((unsigned char)s[i])) ++i;
    memmove(s, s + i, strlen(s + i) + 1);
}

/* One input of a binding: its SDL scancode when it is a key, minus its number for a mouse
 * input, 0 for "none" or a name SDL does not know. */
static int input_from_name(const char *part) {
    char lower[64];
    size_t i = 0;
    for (; part[i] && i + 1 < sizeof(lower); ++i) lower[i] = (char)tolower((unsigned char)part[i]);
    lower[i] = 0;
    if (!strcmp(lower, "none") || !*lower) return 0;
    for (int k = 1; k <= MOUSE_WHEEL_DOWN; ++k)
        if (!strcmp(lower, MOUSE_NAMES[k])) return -k;
    const int scancode = (int)SDL_GetScancodeFromName(part);
    return scancode > 0 ? scancode : 0;
}

bool bb_bindings_parse(const char *action, const char *value) {
    int a = 0;
    while (a < BIND_COUNT && strcmp(INFO[a].key, action)) ++a;
    if (a == BIND_COUNT) return false;
    clear_slots(a);
    char copy[192];
    snprintf(copy, sizeof(copy), "%s", value);
    /* Commas separate bindings, '+' joins the inputs of one: split by hand, so the outer and
     * the inner split do not fight over strtok's state. */
    int slot = 0;
    for (char *cursor = copy; *cursor && slot < BIND_SLOTS;) {
        char *comma = strchr(cursor, ',');
        if (comma) *comma = 0;
        BindSlot next = {0, {0}};
        for (char *part = cursor; *part && next.n < BIND_COMBO_PARTS;) {
            char *plus = strchr(part, '+');
            if (plus) *plus = 0;
            trim(part);
            const int input = input_from_name(part);
            if (input) next.in[next.n++] = input;
            if (!plus) break;
            part = plus + 1;
        }
        if (next.n) g_slot[a][slot++] = next;
        if (!comma) break;
        cursor = comma + 1;
    }
    /* One input, one action: whatever else had it gives it up. A combination keeps its parts,
     * so a modifier can be part of several of them. */
    for (int s = 0; s < BIND_SLOTS; ++s) {
        const BindSlot *mine = &g_slot[a][s];
        if (mine->n != 1) continue;
        for (int other = 0; other < BIND_COUNT; ++other) {
            if (other == a) continue;
            for (int t = 0; t < BIND_SLOTS; ++t) {
                BindSlot *theirs = &g_slot[other][t];
                if (theirs->n == 1 && theirs->in[0] == mine->in[0]) theirs->n = 0;
            }
        }
    }
    return true;
}

void bb_bindings_load(const char *ini_path) {
    defaults();
    g_move_step = MOVE_SMOOTHING_DEFAULT;
    g_pad_back_side = 0;
    FILE *file = ini_path ? fopen(ini_path, "r") : NULL;
    if (!file) return;
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        char *eq = strchr(line, '=');
        if (eq && !strncmp(line, "move_smoothing", 14)) {
            const int percent = atoi(eq + 1);
            g_move_step = percent > 0 && percent < 100 ? (float)percent / 100.0f : 1.0f;
            continue;
        }
        if (eq && !strncmp(line, "pad_back", 8)) {
            char value[32];
            snprintf(value, sizeof(value), "%s", eq + 1);
            trim(value);
            g_pad_back_side = !strcmp(value, "effects");
            continue;
        }
        if (line[0] == '#' || !eq || strncmp(line, "key_", 4)) continue;
        *eq = 0;
        char *name = line + 4, *value = eq + 1;
        trim(name);
        trim(value);
        if (!bb_bindings_parse(name, value)) fprintf(stderr, "Keys: unknown action %s in %s\n", name, ini_path);
    }
    fclose(file);
}

/* The numpad key an arrow or navigation key arrives as when it is sent without the extended-key
 * flag: AutoHotkey and other remappers (GitHub issue #105) send scan code 0x48 for Up, which SDL
 * reads as Keypad 8. A binding of either key is held by both, unless the twin has its own. */
static int numpad_twin(int key) {
    static const int PAIRS[][2] = {
        {SDL_SCANCODE_UP, SDL_SCANCODE_KP_8}, {SDL_SCANCODE_DOWN, SDL_SCANCODE_KP_2},
        {SDL_SCANCODE_LEFT, SDL_SCANCODE_KP_4}, {SDL_SCANCODE_RIGHT, SDL_SCANCODE_KP_6},
        {SDL_SCANCODE_HOME, SDL_SCANCODE_KP_7}, {SDL_SCANCODE_END, SDL_SCANCODE_KP_1},
        {SDL_SCANCODE_PAGEUP, SDL_SCANCODE_KP_9}, {SDL_SCANCODE_PAGEDOWN, SDL_SCANCODE_KP_3},
        {SDL_SCANCODE_INSERT, SDL_SCANCODE_KP_0}, {SDL_SCANCODE_DELETE, SDL_SCANCODE_KP_PERIOD},
    };
    for (size_t i = 0; i < sizeof(PAIRS) / sizeof(*PAIRS); ++i) {
        if (PAIRS[i][0] == key) return PAIRS[i][1];
        if (PAIRS[i][1] == key) return PAIRS[i][0];
    }
    return 0;
}

/* Is a numpad key some slot's whole binding? Then it is not also its arrow's twin. */
static bool twin_bound(int twin) {
    for (int a = 0; a < BIND_COUNT; ++a)
        for (int s = 0; s < BIND_SLOTS; ++s)
            if (g_slot[a][s].n == 1 && g_slot[a][s].in[0] == twin) return true;
    return false;
}

/* Is one input held? Keys come from SDL's state (an arrow also by its numpad twin), mouse inputs
 * from the pad sample's bits. */
static bool input_held(int input, const bool *keys, int nkeys, uint32_t mouse) {
    if (input > 0) {
        if (!keys || input >= nkeys) return false;
        if (keys[input]) return true;
        const int twin = numpad_twin(input);
        return twin && twin < nkeys && keys[twin] && !twin_bound(twin);
    }
    if (is_mouse(input)) return ((mouse >> (-input - 1)) & 1u) != 0;
    return false;
}

static bool slot_held(const BindSlot *b, const bool *keys, int nkeys, uint32_t mouse) {
    for (int i = 0; i < b->n; ++i)
        if (!input_held(b->in[i], keys, nkeys, mouse)) return false;
    return b->n > 0;
}

void bb_bindings_apply(const bool *keys, int nkeys, uint32_t mouse, BindOutput *out) {
    out->buttons = 0;
    out->mouse_buttons = 0;
    out->touch_side = -1;
    out->lx = out->ly = out->rx = out->ry = -1;
    out->any = false;
    /* A combination that is held owns its inputs: the plain bindings on them stay quiet while
     * it is held (Shift + left click is R2 only, not R1 as well). */
    int consumed[BIND_COUNT * BIND_SLOTS * BIND_COMBO_PARTS];
    int nconsumed = 0;
    for (int a = 0; a < BIND_COUNT; ++a)
        for (int s = 0; s < BIND_SLOTS; ++s) {
            const BindSlot *b = &g_slot[a][s];
            if (b->n < 2 || !slot_held(b, keys, nkeys, mouse)) continue;
            for (int i = 0; i < b->n && nconsumed < (int)(sizeof(consumed) / sizeof(*consumed)); ++i)
                consumed[nconsumed++] = b->in[i];
        }
    bool held[BIND_COUNT], by_key[BIND_COUNT];
    for (int a = 0; a < BIND_COUNT; ++a) {
        held[a] = false;
        by_key[a] = false;
        for (int s = 0; s < BIND_SLOTS; ++s) {
            const BindSlot *b = &g_slot[a][s];
            if (!slot_held(b, keys, nkeys, mouse)) continue;
            if (b->n == 1) {
                bool owned = false;
                for (int i = 0; i < nconsumed; ++i)
                    if (consumed[i] == b->in[0]) {
                        owned = true;
                        break;
                    }
                if (owned) continue;
            }
            held[a] = true;
            bool keyboard_only = true;
            for (int i = 0; i < b->n; ++i)
                if (is_mouse(b->in[i])) keyboard_only = false;
            if (keyboard_only) by_key[a] = true;
            break;
        }
    }
    uint32_t key_buttons = 0;
    for (int a = 0; a < BIND_COUNT; ++a) {
        if (!held[a]) continue;
        out->any = true;
        uint32_t bit = INFO[a].button;
        /* The modifier turns Attack into the strong attack, as Dark Souls III's Shift does. */
        if (a == BIND_ATTACK && held[BIND_STRONG_MOD]) bit = PAD_R2;
        out->buttons |= bit;
        if (by_key[a]) key_buttons |= bit;
        switch (a) {
        case BIND_LOOK_U: out->ry = STICK_LOW; break;
        case BIND_LOOK_D: out->ry = STICK_HIGH; break;
        case BIND_LOOK_L: out->rx = STICK_LOW; break;
        case BIND_LOOK_R: out->rx = STICK_HIGH; break;
        case BIND_GESTURES: out->touch_side = 0; break;
        case BIND_EFFECTS: out->touch_side = 1; break;
        default: break;
        }
    }
    out->mouse_buttons = out->buttons & ~key_buttons;
    /* The movement keys: a full push each way, a diagonal the square's corner; with Walk held
     * a gentle push, a diagonal's two axes shortened so its reach is the same. Opposite keys
     * cancel. */
    const int x = held[BIND_MOVE_R] - held[BIND_MOVE_L], y = held[BIND_MOVE_B] - held[BIND_MOVE_F];
    if (!x && !y) {
        g_move_active = false;
        return;
    }
    float reach = (float)(STICK_HIGH - STICK_REST);
    const bool walk = held[BIND_WALK];
    if (walk) reach *= WALK_PUSH;
    /* Movement smoothing (Jgamrot/bloodborne_pc#4, after MKB2Controller's setting): a key
     * pressed or released while moving turns the stick to its new direction over a few frames
     * instead of at once - holding W, then A, otherwise snapped the stick 45 degrees in a frame
     * and the character stopped to turn. Starting to move and letting go stay immediate.
     * The stick turns along the gate rather than across it (measured by cassioKenji,
     * Supermedo/bloodborne_pc PR #74): the game picks walk or run by the larger axis and ends a
     * sprint when the direction moves 45 degrees or more between two reads, so a run stays on
     * the square (the larger axis full at every angle) and no read turns more than 30 degrees.
     * A half turn (A <-> D, W <-> S) goes through forward, or through the right when both ways
     * are sideways. A walk keeps the circle of its push (a diagonal's two axes shortened).
     * bbport.ini move_smoothing: how fast it turns (0.6 by default: 900 degrees a second). */
    const double pi = 3.14159265358979323846;
    const double target = atan2((double)y, (double)x);
    const uint64_t now = SDL_GetTicksNS();
    if (!g_move_active || g_move_step >= 1.0f) {
        g_move_angle = target;
    } else {
        double dt = (double)(now - g_move_time) / 1e9;
        dt = dt < 0.0 ? 0.0 : dt;
        double step = (double)g_move_step * 1500.0 * pi / 180.0 * dt;
        if (step > 30.0 * pi / 180.0) step = 30.0 * pi / 180.0;
        const double diff = fmod(target - g_move_angle + 3.0 * pi, 2.0 * pi) - pi; /* -pi..pi */
        double dir = diff > 0 ? 1.0 : -1.0;
        if (fabs(diff) > 170.0 * pi / 180.0) {
            /* Half way round counter-clockwise is angle + pi/2; forward is y < 0. */
            const double hy = sin(g_move_angle + pi / 2.0), hx = cos(g_move_angle + pi / 2.0);
            dir = hy < -1e-9 ? 1.0 : hy > 1e-9 ? -1.0 : hx > 0 ? 1.0 : -1.0;
        }
        if (fabs(diff) <= step) g_move_angle = target;
        else g_move_angle = atan2(sin(g_move_angle + dir * step), cos(g_move_angle + dir * step));
    }
    const double c = cos(g_move_angle), sn = sin(g_move_angle);
    const double k = walk ? reach : reach / fmax(fabs(c), fabs(sn));
    g_move_x = (float)(c * k);
    g_move_y = (float)(sn * k);
    g_move_active = true;
    g_move_time = now;
    out->any = true;
    out->lx = STICK_REST + (int)lroundf(g_move_x);
    out->ly = STICK_REST + (int)lroundf(g_move_y);
    if (out->lx > STICK_HIGH) out->lx = STICK_HIGH;
    if (out->ly > STICK_HIGH) out->ly = STICK_HIGH;
}
