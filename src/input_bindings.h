/* SPDX-License-Identifier: GPL-2.0-or-later
 * Keyboard and mouse bindings, the way Dark Souls III PC keeps them (after droogie's bbhost,
 * host/bindings.cpp): every action takes up to three bindings, each one a key or a mouse input
 * on its own or a combination held together, and each sets the pad bit (or stick direction) the
 * game reads for it. They apply on top of a connected gamepad, so both work at once.
 *
 * bbport.ini: key_<action>=<binding>, <binding>, <binding>
 *   A binding is one input, or up to three joined with '+' that must all be held ("Left
 *   Shift+Mouse2": Shift and the right button together). An input is a key's SDL name
 *   ("W", "Left Alt", "Keypad 4") or Mouse1..Mouse5 (left, right, middle, the two side
 *   buttons) and WheelUp / WheelDown; "none" unbinds. Commas separate bindings, so the first
 *   one can be a key and the next a mouse button:
 *     key_interact=E                          one binding
 *     key_firearm=X, Mouse2                   either one presses L2
 *     key_strong_attack=Left Shift+Mouse1     Shift and left click together press R2
 *     key_gestures=none                       unbound
 *   A binding that is held wins over the bindings on its own inputs: with
 *   key_strong_attack=Left Shift+Mouse1 and key_attack=Mouse1, Shift + left click is R2 only.
 *   A key given to one action is taken from any other (combinations keep their parts, so a
 *   modifier can serve several of them). */
#ifndef BB_INPUT_BINDINGS_H
#define BB_INPUT_BINDINGS_H

#include <stdbool.h>
#include <stdint.h>

enum {
    BIND_MOVE_F, BIND_MOVE_B, BIND_MOVE_L, BIND_MOVE_R, BIND_WALK, BIND_ROLL, BIND_LOCK_ON,
    BIND_ATTACK, BIND_STRONG, BIND_STRONG_MOD, BIND_TRANSFORM, BIND_FIREARM, BIND_BLOOD_VIAL, BIND_USE_ITEM,
    BIND_INTERACT, BIND_SWITCH_ITEM, BIND_SWITCH_RIGHT, BIND_SWITCH_LEFT, BIND_DPAD_UP, BIND_GESTURES, BIND_EFFECTS,
    BIND_LOOK_U, BIND_LOOK_D, BIND_LOOK_L, BIND_LOOK_R, BIND_MENU, BIND_CONFIRM, BIND_BACK, BIND_L3,
    BIND_COUNT
};
/* Mouse inputs: 1..5 the buttons, 6 and 7 the wheel turned up and down (a short press a notch). */
enum { MOUSE_WHEEL_UP = 6, MOUSE_WHEEL_DOWN = 7 };

/* Bindings per action, and inputs in one combination (the launcher's Controls page shows the
 * same three slots; a fourth is dropped). */
#define BIND_SLOTS 3
#define BIND_COMBO_PARTS 3

typedef struct {
    const char *key;     /* the bbport.ini name after "key_" */
    const char *label;
    uint32_t button;     /* the pad bit; 0 for stick directions, walk and the modifier */
    const char *def_key; /* SDL scancode name, or NULL */
    int def_mouse;       /* 1..7, or 0 */
} BindInfo;
const BindInfo *bb_bind_info(int action);

/* One binding: the inputs that must be held together. in[] holds an SDL scancode when it is
 * positive, and a mouse input (1..7) as its negative (-1 is Mouse1, -6 WheelUp). */
typedef struct {
    int n;                     /* inputs in this binding; 0 = unbound */
    int in[BIND_COMBO_PARTS];
} BindSlot;

/* The defaults, then each key_<action> line of the ini file (missing file: the defaults). */
void bb_bindings_load(const char *ini_path);
/* One key_<action>=<value>; false for an unknown action. */
bool bb_bindings_parse(const char *action, const char *value);
/* bbport.ini pad_back=effects: an Xbox pad's View / Back button alone opens Personal Effects
 * (the touchpad's right half) instead of Gestures (#228). 1 for the right half, 0 the left. */
int bb_bindings_pad_back_side(void);
int bb_binding_key(int action);   /* SDL scancode of the first plain key binding, 0 when none */
int bb_binding_mouse(int action); /* 1..7 of the first plain mouse binding, 0 when none */
/* The bindings of one action: how many are set (up to max_slots). */
int bb_binding_slots(int action, BindSlot *out, int max_slots);
/* The bindings of one action the way bbport.ini writes them ("E, Mouse2", "Left Shift+Mouse1",
 * "none"), for the Key Bindings page of the port's menu. */
void bb_binding_text(int action, char *out, int size);
/* Every action back to its default bindings (the ini file is not read). */
void bb_bindings_reset(void);

/* What the held keys and mouse inputs do to the pad: buttons to set, a touchpad side to press
 * (-1 none, 0 left, 1 right), and stick axes (0..255, or -1 to leave the gamepad's). `keys` is
 * SDL's key state (NULL: none), `mouse` bit n-1 for mouse input n. */
typedef struct {
    uint32_t buttons;
    uint32_t mouse_buttons; /* the part of buttons only mouse inputs hold */
    int touch_side;
    int lx, ly, rx, ry;
    bool any;
} BindOutput;
void bb_bindings_apply(const bool *keys, int nkeys, uint32_t mouse, BindOutput *out);

#endif
