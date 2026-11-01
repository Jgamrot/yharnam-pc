/* SPDX-License-Identifier: GPL-2.0-or-later
 * input_bindings.c: the default PC layout, the strong-attack modifier, mouse buttons and the
 * wheel, walking, and bbport.ini key_* lines with one key per action. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "input_bindings.h"

static bool keys[SDL_SCANCODE_COUNT];

static BindOutput press(const char *key, uint32_t mouse) {
    memset(keys, 0, sizeof(keys));
    if (key) keys[SDL_GetScancodeFromName(key)] = true;
    BindOutput o;
    bb_bindings_apply(keys, SDL_SCANCODE_COUNT, mouse, &o);
    return o;
}

int main(void) {
    bb_bindings_load(NULL); /* the defaults */
    BindOutput o = press(NULL, 0);
    assert(!o.any && o.lx < 0 && o.touch_side < 0);
    o = press("E", 0);
    assert(o.buttons == 0x4000);                    /* interact: Cross */
    o = press("Space", 0);
    assert(o.buttons == 0x2000);                    /* dodge: Circle */
    o = press(NULL, 1u << 0);
    assert(o.buttons == 0x800);                     /* left mouse: R1 */
    o = press("Left Shift", 1u << 0);
    assert(o.buttons == 0x200);                     /* Shift + left mouse: R2 */
    o = press(NULL, 1u << 1);
    assert(o.buttons == 0x100);                     /* right mouse: L2 (firearm) */
    assert(o.mouse_buttons == 0x100);               /* ... held by the mouse alone */
    o = press("X", 1u << 1);
    assert(o.buttons == 0x100 && !o.mouse_buttons); /* the firearm key holds it too */
    o = press("E", 1u << 0);
    assert(o.buttons == 0x4800 && o.mouse_buttons == 0x800); /* only R1 is the mouse's */
    o = press("W", 0);
    assert(o.ly == 1 && o.lx == 128);               /* forward: a full push up */
    press(NULL, 0);                                 /* stopped: the next move starts at once */
    memset(keys, 0, sizeof(keys));
    keys[SDL_GetScancodeFromName("W")] = keys[SDL_GetScancodeFromName("D")] = keys[SDL_GetScancodeFromName("Left Alt")] = true;
    bb_bindings_apply(keys, SDL_SCANCODE_COUNT, 0, &o);
    assert(o.lx > 170 && o.lx < 195 && o.ly < 86 && o.ly > 61);  /* a walk, diagonally */
    press(NULL, 0);
    memset(keys, 0, sizeof(keys));
    keys[SDL_GetScancodeFromName("W")] = keys[SDL_GetScancodeFromName("Left Alt")] = true;
    bb_bindings_apply(keys, SDL_SCANCODE_COUNT, 0, &o);
    /* A walk forward: past the game's dead zone (a third of a push stood still, #205), short
     * of a run. */
    assert(o.lx == 128 && o.ly >= 40 && o.ly <= 64);
    o = press("G", 0);
    assert(o.buttons == 0x100000 && o.touch_side == 0);  /* gestures: the touchpad's left */
    /* Arrows sent without the extended-key flag (AutoHotkey, #105) arrive as the numpad's. */
    o = press("Keypad 8", 0);
    assert(o.buttons == 0x10);                      /* D-pad Up */
    o = press("Keypad 4", 0);
    assert(o.buttons == 0x80);                      /* switch left weapon: d-pad left */
    assert(bb_bindings_parse("dpad_up", "Keypad 8"));
    o = press("Up", 0);
    assert(o.buttons == 0x10);                      /* and the other way round */
    assert(bb_bindings_parse("lock_on", "Keypad 4"));
    o = press("Keypad 4", 0);
    assert(o.buttons == 0x4);                       /* a numpad key with its own action keeps it */
    assert(bb_bindings_parse("dpad_up", "Up") && bb_bindings_parse("lock_on", "Q"));

    /* Movement smoothing (issue #4): W, then W+A swings the stick over a few frames. */
    press(NULL, 0);
    o = press("W", 0);
    assert(o.ly == 1 && o.lx == 128);               /* starting to move: at once */
    memset(keys, 0, sizeof(keys));
    keys[SDL_GetScancodeFromName("W")] = keys[SDL_GetScancodeFromName("A")] = true;
    bb_bindings_apply(keys, SDL_SCANCODE_COUNT, 0, &o);
    assert(o.lx > 100);                             /* no time passed: still almost straight */
    SDL_Delay(20);
    bb_bindings_apply(keys, SDL_SCANCODE_COUNT, 0, &o);
    assert(o.lx > 1 && o.lx < 100);                 /* on its way */
    SDL_Delay(50);
    bb_bindings_apply(keys, SDL_SCANCODE_COUNT, 0, &o);
    assert(o.lx == 1 && o.ly == 1);                 /* there */
    /* A sprint is kept: W, then A alone turns along the square's edge, the larger axis full. */
    press(NULL, 0);
    o = press("W", 0);
    for (int i = 0; i < 15; ++i) {
        SDL_Delay(10);
        o = press("A", 0);
        const int ax = o.lx < 128 ? 128 - o.lx : o.lx - 128, ay = o.ly < 128 ? 128 - o.ly : o.ly - 128;
        assert((ax > ay ? ax : ay) == 127);
    }
    assert(o.lx == 1 && o.ly == 128);               /* left */
    /* A half turn (A -> D) goes through forward, never through neutral or back. */
    for (int i = 0; i < 30; ++i) {
        SDL_Delay(10);
        o = press("D", 0);
        assert(o.ly <= 128);
    }
    assert(o.lx == 255 && o.ly == 128);             /* right */

    /* key_firearm=X, WheelUp: the wheel's notch fires; right mouse is free then. */
    assert(bb_bindings_parse("firearm", "X, WheelUp"));
    o = press(NULL, 1u << (MOUSE_WHEEL_UP - 1));
    assert(o.buttons == 0x100);
    o = press(NULL, 1u << 1);
    assert(!o.any);
    /* One key, one action: E moves from interact to blood vial. */
    assert(bb_bindings_parse("blood_vial", "E"));
    o = press("E", 0);
    assert(o.buttons == 0x1000);
    assert(bb_binding_key(BIND_INTERACT) == 0);
    assert(bb_bindings_parse("interact", "none") && !bb_bindings_parse("no_such_action", "E"));

    /* An ini file: only key_ lines are read. */
    const char *path = "test_input_bindings.ini";
    FILE *f = fopen(path, "w");
    assert(bb_bindings_pad_back_side() == 0);       /* View / Back: Gestures by default */
    fputs("upscaler=dlss\nkey_roll = Left Ctrl\n# key_attack=Mouse2\nmove_smoothing=100\npad_back = effects\n", f);
    fclose(f);
    bb_bindings_load(path);
    remove(path);
    o = press("Left Ctrl", 0);
    assert(o.buttons == 0x2000);                    /* roll moved to Left Ctrl, L3 gave it up */
    assert(bb_bindings_pad_back_side() == 1);       /* pad_back=effects: Personal Effects (#228) */
    assert(bb_binding_mouse(BIND_ATTACK) == 1);     /* the commented line changed nothing */
    press(NULL, 0);
    press("W", 0);
    memset(keys, 0, sizeof(keys));
    keys[SDL_GetScancodeFromName("W")] = keys[SDL_GetScancodeFromName("A")] = true;
    bb_bindings_apply(keys, SDL_SCANCODE_COUNT, 0, &o);
    assert(o.lx == 1);                              /* move_smoothing=100: off, at once */

    /* Combinations: Shift + left click is the strong attack on its own, and a plain left click
     * is still the attack - the held combination owns its inputs. */
    assert(bb_bindings_parse("attack", "Mouse1"));
    assert(bb_bindings_parse("strong_attack", "Left Shift+Mouse1"));
    o = press("Left Shift", 1u << 0);
    assert(o.buttons == 0x200);                     /* R2 only, not R1 as well */
    o = press(NULL, 1u << 0);
    assert(o.buttons == 0x800);                     /* left click alone: R1 */
    /* Up to three bindings on one action, a fourth is dropped. */
    BindSlot slots[BIND_SLOTS];
    assert(bb_bindings_parse("interact", "E, F, G"));
    assert(bb_binding_slots(BIND_INTERACT, slots, BIND_SLOTS) == 3);
    o = press("F", 0);
    assert(o.buttons == 0x4000);
    o = press("G", 0);
    assert(o.buttons == 0x4000);
    assert(bb_bindings_parse("interact", "E, F, G, H"));
    assert(bb_binding_slots(BIND_INTERACT, slots, BIND_SLOTS) == 3);
    /* The text the Key Bindings page shows and writes back, and the reset to the defaults. */
    char text[96];
    bb_binding_text(BIND_INTERACT, text, sizeof(text));
    assert(!strcmp(text, "E, F, G"));
    bb_binding_text(BIND_STRONG, text, sizeof(text));
    assert(!strcmp(text, "Left Shift+Mouse1"));
    assert(bb_bindings_parse("gestures", "none"));
    bb_binding_text(BIND_GESTURES, text, sizeof(text));
    assert(!strcmp(text, "none"));
    bb_bindings_reset();
    bb_binding_text(BIND_FIREARM, text, sizeof(text));
    assert(!strcmp(text, "X, Mouse2"));
    bb_binding_text(BIND_GESTURES, text, sizeof(text));
    assert(!strcmp(text, "G"));
    assert(bb_bindings_parse("interact", text) && bb_binding_key(BIND_INTERACT) == SDL_SCANCODE_G);
    puts("input bindings: ok");
    return 0;
}
