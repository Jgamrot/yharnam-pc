/* libScePad on SDL3 gamepads, with keyboard and mouse on top. SDL events are pumped
 * by the window thread (gpu/shim/window.cpp); here state is only sampled.
 *
 * Keyboard and mouse follow bindings (input_bindings.h; bbport.ini key_<action>=...), by
 * default Dark Souls III PC's: WASD move, Left Alt walk, Space dodge/sprint, Q lock on, left
 * mouse attack (Left Shift + it: strong), V strong attack, C / middle mouse transform, X / right
 * mouse firearm, R blood vial, F item, E interact, arrows d-pad, G gestures, T personal effects,
 * IJKL camera, Escape menu, Enter / Backspace menu confirm / back, Left Ctrl L3.
 *
 * After Bainianzzz's deadinside28/bloodborne_pc#71:
 * Stick neutral: SDL exposes no way to read a pad's calibration and some clones report a
 * biased neutral (a Switch-style pad was seen returning both sticks at a constant ~ +/-16380).
 * The neutral of each axis is taken from a quiet window after the pad opens (SDL returns zero
 * until the first report arrives, so those samples are skipped) and subtracted; a genuine pad
 * settles at 0, so the subtraction is a no-op.
 *
 * Travel: such a clone also uses only part of SDL's -32768..32767 span (its neutral sits in the
 * middle of one half), so reading the axis as -128..127 would reach only half deflection and a
 * full push would never run. Each axis is instead scaled by its own travel: a biased axis starts
 * from its neutral's magnitude and is refined by the largest push seen per direction, so both
 * directions reach full deflection even when the two travels differ by a few percent (that
 * difference is what makes one direction run and the other only walk). A genuine pad keeps the
 * plain -32768..32767 -> -128..127 read. The stick is then converted as shadPS4 does, with an
 * inner/outer dead zone (BB_PAD_DEADZONE, default 0 here, so a genuine pad reads as before; BB_PAD_DEADZONE_OUTER, default 127) mapping
 * the axis' travel up to full deflection. BB_PAD_CENTER=lx,ly,rx,ry overrides the neutral,
 * BB_PAD_CENTER_CAL=0 disables the measurement.
 *
 * Xbox pads (#182): XInput reports sticks the driver already centres, but worn sticks rest a
 * little off centre and SDL applies no dead zone, so the character crept and the camera turned
 * slowly. Such a pad gets an inner dead zone of PAD_XBOX_DEADZONE (BB_PAD_DEADZONE overrides),
 * and only a biased neutral (a clone) is subtracted: a smaller "neutral" is the stick held while
 * the pad connected, which used to turn into a slow push one way or another each session. */
#define _GNU_SOURCE
#include "runtime.h"
#include "runtime_atlas_release.h"
#include "mouse_camera.h"
#include "menu_pointer.h"
#include "gpu/bbgpu.h"
#include "input_bindings.h"
#include "runtime_stamina.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <SDL3/SDL.h>
#include <sys/stat.h>

#define ERR_INVALID_ARG ((int32_t)0x80920001)
#define ERR_INVALID_HANDLE ((int32_t)0x80920003)
#define ERR_ALREADY_OPENED ((int32_t)0x80920004)
#define ERR_NOT_INITIALIZED ((int32_t)0x80920005)
#define PAD_HANDLE 1

enum {
    BTN_L3=0x2, BTN_R3=0x4, BTN_OPTIONS=0x8, BTN_UP=0x10, BTN_RIGHT=0x20, BTN_DOWN=0x40, BTN_LEFT=0x80,
    BTN_L2=0x100, BTN_R2=0x200, BTN_L1=0x400, BTN_R1=0x800, BTN_TRIANGLE=0x1000, BTN_CIRCLE=0x2000,
    BTN_CROSS=0x4000, BTN_SQUARE=0x8000, BTN_TOUCHPAD=0x100000,
};
typedef struct { uint16_t x, y; uint8_t id, reserve[3]; } PadTouch;
typedef struct {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t l2, r2, analog_padding[2];
    float orientation[4], acceleration[3], angular_velocity[3];
    uint8_t touch_count, touch_reserve[3];
    uint32_t touch_held_time;
    PadTouch touches[2];
    uint8_t connected, pad0[3];
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t connected_count, reserve[2], unique_length, unique[12];
} PadData;
typedef struct {
    float pixel_density; uint16_t resolution_x, resolution_y;
    uint8_t dead_zone_left, dead_zone_right, connection_type, connected_count;
    uint8_t connected, pad[3];
    int32_t device_class;
    uint8_t reserve[8];
} ControllerInfo;
_Static_assert(sizeof(PadData)==120,"OrbisPadData layout");
_Static_assert(sizeof(PadTouch)==8,"OrbisPadTouch layout");
_Static_assert(__builtin_offsetof(PadData,touches)==60,"OrbisPadData touch offset");
_Static_assert(__builtin_offsetof(PadData,timestamp)==80,"OrbisPadData timestamp offset");
_Static_assert(sizeof(ControllerInfo)==28,"OrbisPadControllerInformation layout");

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized, opened, sdl_ready;
static SDL_Gamepad *gamepad;
static size_t reads;
static uint8_t connected_count;

/* Per-controller stick neutral (see the file header): the value each axis holds while the
 * sticks are untouched, taken from a quiet window after the pad opens. Works for any pad; a
 * genuine one settles at 0 and the subtraction is a no-op. */
#define PAD_CAL_QUIET 512              /* an axis is quiet when it moved no more than this */
#define PAD_CAL_QUIET_SAMPLES 8
#define PAD_CAL_TIMEOUT_US 3000000u    /* after this, an all-zero reading is accepted as the neutral */
#define PAD_CAL_BIAS 8192              /* |neutral| past this: a biased neutral and a reduced travel */
#define PAD_CAL_REFINE 90              /* % of the base travel a push must reach to count as the full scale */
#define PAD_XBOX_DEADZONE 20           /* of 128: an Xbox pad's inner dead zone (about 16%) */
static int cal_enabled=-1, pad_deadzone=-1, pad_deadzone_outer=-1, pad_deadzone_set, cal_manual;
static int pad_xbox;                   /* the pad read is an Xbox 360 / One / Series pad */
static int cal_have_center, cal_started, cal_quiet_count;
static int cal_center[4], cal_prev[4], cal_manual_center[4];
static int cal_base[4];                /* 0: normal pad; else |neutral| of a biased axis */
static int cal_max[4][2];              /* largest push seen per direction on a biased axis */
static uint64_t cal_since;

static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u; }
static uint8_t trigger(int16_t v) { int x=v>>7; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static void cal_load(void) {
    static int loaded;
    if (loaded) return;
    loaded=1;
    const char *v=getenv("BB_PAD_CENTER_CAL"); cal_enabled=!(v && *v=='0');
    /* Inner/outer dead zone, as shadPS4's analog_deadzone: [inner, outer] maps linearly to the
     * full deflection, so an axis with a reduced travel (outer below 127) reaches full. */
    v=getenv("BB_PAD_DEADZONE"); pad_deadzone_set=v && *v; pad_deadzone=pad_deadzone_set ? atoi(v) : 0; /* bbport: 0 keeps the old read */
    if (pad_deadzone<0) pad_deadzone=0; else if (pad_deadzone>126) pad_deadzone=126;
    v=getenv("BB_PAD_DEADZONE_OUTER"); pad_deadzone_outer=v && *v ? atoi(v) : 127;
    if (pad_deadzone_outer<=pad_deadzone) pad_deadzone_outer=pad_deadzone<127 ? pad_deadzone+1 : 127;
    v=getenv("BB_PAD_CENTER");
    if (v && *v && sscanf(v,"%d,%d,%d,%d",&cal_manual_center[0],&cal_manual_center[1],
                          &cal_manual_center[2],&cal_manual_center[3])==4) {
        cal_manual=1;
        printf("Runtime: pad center (BB_PAD_CENTER): lx=%d ly=%d rx=%d ry=%d\n",
               cal_manual_center[0],cal_manual_center[1],cal_manual_center[2],cal_manual_center[3]);
    }
}
static void cal_set_base(void) {
    int biased=0;
    for (int i=0;i<4;++i) {
        const int a=abs(cal_center[i]);
        cal_base[i]=a>=PAD_CAL_BIAS ? a : 0;   /* a biased axis spans one half of the int16 range */
        cal_max[i][0]=cal_max[i][1]=0;
        biased|=cal_base[i]!=0;
    }
    if (biased)
        printf("Runtime: pad travel (biased neutral, per-axis scaling): lx=%d ly=%d rx=%d ry=%d\n",
               cal_base[0]?cal_base[0]:32768,cal_base[1]?cal_base[1]:32768,
               cal_base[2]?cal_base[2]:32768,cal_base[3]?cal_base[3]:32768);
}
/* Full-scale travel of one direction. A biased axis starts from its neutral's magnitude and is
 * refined by the largest push seen (once that push is close to the base travel), so a direction
 * whose physical travel is a few percent shorter still reaches full deflection. */
static int cal_travel(int axis,int dir) {
    const int base=cal_base[axis] ? cal_base[axis] : 32768;
    return cal_base[axis] && cal_max[axis][dir]*100>=base*PAD_CAL_REFINE ? cal_max[axis][dir] : base;
}
static void cal_reset(void) {
    cal_load();
    cal_started=0; cal_quiet_count=0; cal_since=0; memset(cal_prev,0,sizeof cal_prev);
    if (cal_manual) { memcpy(cal_center,cal_manual_center,sizeof cal_center); cal_have_center=1; }
    else if (!cal_enabled) { memset(cal_center,0,sizeof cal_center); cal_have_center=1; }
    else cal_have_center=0;
    if (cal_have_center) cal_set_base();
}
/* The value each axis holds while the sticks are untouched (SDL's pre-report zeros are skipped);
 * once it is known, the largest push seen per direction on a biased axis (see cal_travel). */
static void cal_sample(const int16_t raw[4]) {
    if (cal_have_center) {
        for (int i=0;i<4;++i) {
            if (!cal_base[i]) continue;
            const int d=raw[i]-cal_center[i], dir=d>=0, travel=dir ? d : -d;
            if (travel>cal_max[i][dir]) cal_max[i][dir]=travel;
        }
        return;
    }
    int allzero=1; for (int i=0;i<4;++i) if (raw[i]) allzero=0;
    int quiet=1; for (int i=0;i<4;++i) if (cal_started && abs(raw[i]-cal_prev[i])>PAD_CAL_QUIET) quiet=0;
    const uint64_t now=now_us();
    if (!cal_started) { cal_started=1; cal_since=now; }
    if (allzero && now-cal_since<PAD_CAL_TIMEOUT_US) quiet=0;
    if (quiet) {
        if (++cal_quiet_count>=PAD_CAL_QUIET_SAMPLES) {
            for (int i=0;i<4;++i) cal_center[i]=pad_xbox && abs(raw[i])<PAD_CAL_BIAS ? 0 : raw[i];
            cal_have_center=1;
            printf("Runtime: pad neutral: lx=%d ly=%d rx=%d ry=%d\n",cal_center[0],cal_center[1],cal_center[2],cal_center[3]);
            cal_set_base();
        }
    } else cal_quiet_count=0;
    for (int i=0;i<4;++i) cal_prev[i]=raw[i];
}
/* One stick axis in the PS4 0..255 scale. The neutral is subtracted and the axis' own travel
 * (cal_travel) is read as -128..127, so a full push reaches full deflection in both directions
 * whatever part of SDL's range the pad uses; the inner/outer dead zone (BB_PAD_DEADZONE /
 * BB_PAD_DEADZONE_OUTER) then maps that travel to the full deflection, as shadPS4 does. */
static uint8_t stick_axis(int axis,int16_t raw) {
    const int d=raw-cal_center[axis];
    const int travel=cal_travel(axis,d>=0 ? 1 : 0);
    int v=travel>0 ? (int)((long long)d*128/travel) : 0;
    if (v>127) v=127; else if (v<-128) v=-128;
    const int mag=abs(v);
    const int inner=pad_deadzone_set || !pad_xbox ? pad_deadzone : PAD_XBOX_DEADZONE;
    if (mag<=inner || inner>=pad_deadzone_outer) v=0;
    else {
        int scaled=(int)(128.0*(mag-inner)/(float)(pad_deadzone_outer-inner));
        if (scaled>128) scaled=128;
        v = v>=0 ? scaled : -scaled;
    }
    int x=v+128;
    return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x);
}
static uint16_t touch_axis(float v, int max) {
    return (uint16_t)(v<=0.0f ? 0 : v>=1.0f ? max : (int)(v*max+0.5f));
}
static void touch_click(PadData *d, int right) {
    d->buttons|=BTN_TOUCHPAD;
    d->touch_count=1;
    d->touches[0]=(PadTouch){.x=right ? 1440 : 480,.y=471,.id=0};
}

/* The byte used while sampling is the SDL finger slot (synthetic clicks use slot 0).
 * A DualShock assigns a new nonzero ID to each contact and reports how long a
 * contact has been held. Keep those fields across pad reads instead of returning
 * an ID and hold time of zero for every sample. */
static void finish_touch(PadData *d) {
    static uint8_t active[2], ids[2], next_id=1;
    static uint64_t down_since;
    uint8_t seen[2]={0,0};
    for (int i=0;i<d->touch_count;++i) {
        const uint8_t slot=d->touches[i].id;
        if (slot>=2) continue;
        seen[slot]=1;
        if (!active[slot]) {
            ids[slot]=next_id;
            next_id=next_id==127 ? 1 : next_id+1;
        }
        d->touches[i].id=ids[slot];
    }
    if (d->touch_count) {
        if (!active[0] && !active[1]) down_since=d->timestamp;
        const uint64_t held=d->timestamp-down_since;
        d->touch_held_time=(uint32_t)(held>UINT32_MAX ? UINT32_MAX : held);
    } else down_since=0;
    active[0]=seen[0]; active[1]=seen[1];
}

/* Every gamepad SDL knows about is kept open and the game reads one of them. With Steam Input
 * (Big Picture, Bazzite, Steam Deck) or DS4Windows a pad shows up twice: the physical one, which
 * the tool keeps quiet, and the virtual pad it drives. SDL's order of the two changes from start
 * to start, and reading only the first pad sometimes read the quiet one (GitHub issue #113).
 * The read stays on a pad while it is in use; once it has been idle for a second and another pad
 * has a button or a stick pushed, it moves there. Two pads sending the same input are both busy
 * and never trade places (Awaken's port, 0.9.34-beta.8: "controller input flipping between two
 * devices (DS4Windows, Steam Input)"). */
#define PAD_MAX 4
#define PAD_IDLE_US 1000000u
static SDL_Gamepad *pads[PAD_MAX];
static uint64_t pad_busy_at[PAD_MAX], pads_listed_at;
static int pad_busy(SDL_Gamepad *g) {
    for (int b=0;b<SDL_GAMEPAD_BUTTON_COUNT;++b) if (SDL_GetGamepadButton(g,(SDL_GamepadButton)b)) return 1;
    for (int a=0;a<SDL_GAMEPAD_AXIS_COUNT;++a) {
        const int v=SDL_GetGamepadAxis(g,(SDL_GamepadAxis)a);
        if (v>16000 || v<-16000) return 1;
    }
    return 0;
}
/* The gamepad the game reads (see above); called under lock. */
static SDL_Gamepad *current_gamepad(void) {
    if (!sdl_ready) sdl_ready = SDL_WasInit(SDL_INIT_GAMEPAD) ? 1 : SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : -1;
    if (sdl_ready<0) return NULL;
    const uint64_t now=now_us();
    for (int i=0;i<PAD_MAX;++i) if (pads[i] && !SDL_GamepadConnected(pads[i])) {
        if (pads[i]==gamepad) gamepad=NULL;
        SDL_CloseGamepad(pads[i]); pads[i]=NULL;
    }
    if (!gamepad || now-pads_listed_at>PAD_IDLE_US) {
        pads_listed_at=now;
        int count=0;
        SDL_JoystickID *ids=SDL_GetGamepads(&count);
        for (int k=0;ids && k<count;++k) {
            int known=0, free_slot=-1;
            for (int i=0;i<PAD_MAX;++i) {
                if (pads[i] && SDL_GetGamepadID(pads[i])==ids[k]) known=1;
                if (!pads[i] && free_slot<0) free_slot=i;
            }
            if (known || free_slot<0) continue;
            pads[free_slot]=SDL_OpenGamepad(ids[k]);
            pad_busy_at[free_slot]=0;
            if (pads[free_slot]) printf("Runtime: gamepad found: %s\n",SDL_GetGamepadName(pads[free_slot]));
        }
        SDL_free(ids);
    }
    int current=-1, pick=-1;
    for (int i=0;i<PAD_MAX;++i) if (pads[i]) {
        if (pad_busy(pads[i])) pad_busy_at[i]=now;
        if (pads[i]==gamepad) current=i;
    }
    pick=current;
    if (current<0 || now-pad_busy_at[current]>PAD_IDLE_US) {
        for (int i=0;i<PAD_MAX;++i) if (pads[i] && i!=current && pad_busy_at[i]==now) { pick=i; break; }
        for (int i=0;i<PAD_MAX && pick<0;++i) if (pads[i]) pick=i;
    }
    if (pick>=0 && pads[pick]!=gamepad) {
        gamepad=pads[pick];
        const SDL_GamepadType type=SDL_GetGamepadType(gamepad);
        pad_xbox=type==SDL_GAMEPAD_TYPE_XBOX360 || type==SDL_GAMEPAD_TYPE_XBOXONE;
        cal_reset(); /* a new pad has its own neutral */
        ++connected_count;
        printf("Runtime: gamepad connected: %s\n",SDL_GetGamepadName(gamepad));
        if (pad_xbox && !pad_deadzone_set)
            printf("Runtime: Xbox pad: stick dead zone %d of 128 (BB_PAD_DEADZONE sets another)\n",PAD_XBOX_DEADZONE);
    }
    return gamepad;
}
static uint32_t host_mouse_buttons; /* the pad bits only mouse bindings set (sample_host) */

/* The Key Bindings page of the port's menu edits the bindings through these (bbgpu.h).
 * The menu's thread changes them while the pad read uses them, unlocked: a read in the middle of
 * a change can see an action half rebound, for that one read. */
static const char *binding_key(int action) { return bb_bind_info(action)->key; }
static const char *binding_label(int action) { return bb_bind_info(action)->label; }
static void binding_set(int action, const char *value) { bb_bindings_parse(bb_bind_info(action)->key, value); }
static const BbBindingsApi BINDINGS_API = {BIND_COUNT, binding_key, binding_label, bb_binding_text, binding_set,
                                           bb_bindings_reset};

static void sample_host(PadData *d) {
    memset(d,0,sizeof(*d));
    d->left_x=d->left_y=d->right_x=d->right_y=128;
    d->orientation[3]=1.0f;
    d->connected=1; d->connected_count=connected_count ? connected_count : 1;
    d->timestamp=now_us();
    SDL_Gamepad *g=current_gamepad();
    /* Sampled even while the settings menu or Hunter's Atlas's map owns input: the map reads
     * it (bbgpu_atlas_route_pad), and sample_game hands the game neutral input afterwards. */
    int nkeys=0;
    const bool *k=SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(&nkeys) : NULL;
    if (g) {
        static const struct { SDL_GamepadButton sdl; uint32_t ps; } map[]={
            {SDL_GAMEPAD_BUTTON_SOUTH,BTN_CROSS}, {SDL_GAMEPAD_BUTTON_EAST,BTN_CIRCLE},
            {SDL_GAMEPAD_BUTTON_WEST,BTN_SQUARE}, {SDL_GAMEPAD_BUTTON_NORTH,BTN_TRIANGLE},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,BTN_L1}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,BTN_R1},
            {SDL_GAMEPAD_BUTTON_LEFT_STICK,BTN_L3}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK,BTN_R3},
            {SDL_GAMEPAD_BUTTON_START,BTN_OPTIONS}, {SDL_GAMEPAD_BUTTON_TOUCHPAD,BTN_TOUCHPAD},
            {SDL_GAMEPAD_BUTTON_DPAD_UP,BTN_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN,BTN_DOWN},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT,BTN_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT,BTN_RIGHT},
        };
        for (size_t i=0;i<sizeof(map)/sizeof(*map);++i) if (SDL_GetGamepadButton(g,map[i].sdl)) d->buttons|=map[i].ps;
        static const SDL_GamepadAxis stick_axes[4]={SDL_GAMEPAD_AXIS_LEFTX,SDL_GAMEPAD_AXIS_LEFTY,
                                                    SDL_GAMEPAD_AXIS_RIGHTX,SDL_GAMEPAD_AXIS_RIGHTY};
        int16_t raw[4];
        for (int i=0;i<4;++i) raw[i]=(int16_t)SDL_GetGamepadAxis(g,stick_axes[i]);
        cal_load();
        cal_sample(raw);
        uint8_t *axis_out[4]={&d->left_x,&d->left_y,&d->right_x,&d->right_y};
        for (int i=0;i<4;++i)
            *axis_out[i]=cal_have_center ? stick_axis(i,raw[i]) : 128;
        d->l2=trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFT_TRIGGER)); d->r2=trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
        if (d->l2>30) d->buttons|=BTN_L2;
        if (d->r2>30) d->buttons|=BTN_R2;
        if (SDL_GetNumGamepadTouchpads(g)>0) {
            const int fingers=SDL_GetNumGamepadTouchpadFingers(g,0);
            for (int finger=0;finger<fingers && d->touch_count<2;++finger) {
                bool down=false;
                float x=0, y=0;
                if (SDL_GetGamepadTouchpadFinger(g,0,finger,&down,&x,&y,NULL) && down) {
                    d->touches[d->touch_count++]=(PadTouch){.x=touch_axis(x,1919),
                        .y=touch_axis(y,942),.id=(uint8_t)finger};
                }
            }
        }
        // Back/Select stands in for the touchpad on pads without one (Xbox): Back + d-pad left or
        // right clicks that half (left: gestures, right: key items and the right-side choices of
        // some prompts) while the d-pad is held; Back alone clicks the left half for 150 ms when
        // it is released, so that a Back + right combo does not open the gestures first.
        // (parsifal295, Supermedo/bloodborne_pc#39.) BB_PAD_BACK_COMBO=0: Back is the left half at once.
        // bbport.ini pad_back=effects makes Back alone the right half, Personal Effects (#228).
        static int back_combo_on=-1;
        if (back_combo_on<0) { const char *e=getenv("BB_PAD_BACK_COMBO"); back_combo_on=!(e && !strcmp(e,"0")); }
        static bool back_was_down, back_combo;
        static uint64_t back_click_until;
        const bool back=SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_BACK);
        int back_side=-1;
        const int back_alone=bb_bindings_pad_back_side();
        if (!back_combo_on) {
            if (back) back_side=back_alone;
        } else if (back) {
            if (!back_was_down) back_combo=false;
            if (d->buttons & (BTN_LEFT|BTN_RIGHT)) {
                back_side=(d->buttons & BTN_RIGHT) ? 1 : 0;
                back_combo=true;
            }
            d->buttons&=~(uint32_t)(BTN_LEFT|BTN_RIGHT);
        } else if (back_was_down && !back_combo) {
            back_click_until=d->timestamp+150000;
        }
        back_was_down=back;
        if (back_combo_on && back_side<0 && d->timestamp<back_click_until) back_side=back_alone;
        if (back_side>=0 && !d->touch_count) touch_click(d,back_side);
        // The touchpad button of a pad whose touch surface SDL does not report: a left-side click.
        if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0);
    }
    /* Keyboard and mouse (input_bindings.h, bbport.ini key_*), on top of the gamepad: both
     * work at once. Mouse input counts only while the game window has the focus. */
    static int bindings_loaded;
    if (!bindings_loaded) {
        bindings_loaded=1;
        bb_bindings_load(getenv("BB_CONFIG"));
        bbgpu_set_bindings(&BINDINGS_API);
    }
    uint32_t mouse=0;
    if (SDL_WasInit(SDL_INIT_VIDEO) && SDL_GetKeyboardFocus()) {
        const SDL_MouseButtonFlags b=SDL_GetMouseState(NULL,NULL);
        if (b & SDL_BUTTON_LMASK) mouse|=1u<<0;
        if (b & SDL_BUTTON_RMASK) mouse|=1u<<1;
        if (b & SDL_BUTTON_MMASK) mouse|=1u<<2;
        if (b & SDL_BUTTON_X1MASK) mouse|=1u<<3;
        if (b & SDL_BUTTON_X2MASK) mouse|=1u<<4;
        mouse|=bbgpu_mouse_wheel_take()<<5; /* a notch is a short press (bit 5 up, 6 down) */
    } else {
        k=NULL;
    }
    /* In a menu the pointer owns the mouse (menu_pointer.h): a left click goes to the row under
     * it, a right click is the back button, the wheel scrolls; the buttons' gameplay bindings
     * (Mouse1 attack is R1, a tab switch in menus) wait until the menu is closed. With the strong
     * attack modifier held, the clicks keep their bindings instead: Shift (Ctrl with the keyboard
     * prompts) + left click is R2 and + right click L2, the menus' shortcut buttons (the Storage
     * switch at a messenger, #64), which a menu could not otherwise get from the mouse. */
    static uint32_t mouse_was;
    const int mod_key=bb_binding_key(BIND_STRONG_MOD), mod_mouse=bb_binding_mouse(BIND_STRONG_MOD);
    const int modifier=(k && mod_key>0 && mod_key<nkeys && k[mod_key]) || (mod_mouse && (mouse>>(mod_mouse-1))&1);
    if (bb_menu_pointer_owns_mouse() && !(modifier && (mouse & 3u))) {
        const uint32_t edge=mouse & ~mouse_was;
        if (edge & 1u) bb_menu_pointer_click();
        if (edge & 2u) bb_menu_pointer_back();
        if (mouse & (1u<<5)) d->buttons|=BTN_UP;
        if (mouse & (1u<<6)) d->buttons|=BTN_DOWN;
        mouse_was=mouse;
        mouse=0;
    } else {
        mouse_was=mouse;
    }
    d->buttons|=bb_menu_pointer_buttons();
    BindOutput o;
    bb_bindings_apply(k,nkeys,mouse,&o);
    /* What only the mouse holds (left click is R1, attack): neither gamepad nor key gives it. */
    host_mouse_buttons=o.mouse_buttons & ~d->buttons;
    if (o.any) {
        d->buttons|=o.buttons;
        if (o.buttons & BTN_L2) d->l2=255;
        if (o.buttons & BTN_R2) d->r2=255;
        if (o.touch_side>=0) touch_click(d,o.touch_side);
        if (o.lx>=0) d->left_x=(uint8_t)o.lx;
        if (o.ly>=0) d->left_y=(uint8_t)o.ly;
        if (o.rx>=0) d->right_x=(uint8_t)o.rx;
        if (o.ry>=0) d->right_y=(uint8_t)o.ry;
    }
    /* The mouse camera (mouse_camera.h) gives way to the stick; locked on, a mouse flick is a
     * right-stick flick. */
    bb_mouse_camera_note_stick(d->right_x,d->right_y);
    const int flick=bb_mouse_camera_stick_x();
    if (flick>=0) d->right_x=(uint8_t)flick;
}

/* BB_PAD_FILE=<file>: scripted input for automated runs. The file holds whitespace-separated
 * tokens, re-read when it changes: button names (cross circle square triangle l1 r1 l2 r2 l3 r3
 * options touchpad touchpad_left touchpad_right up down left right) are held while listed;
 * touchpad defaults to a left-side click; lx= ly= rx= ry= (0..255) override
 * the sticks. An empty file releases everything. */
static struct { uint32_t buttons; int stick[4]; int touch_side; } injected={0,{-1,-1,-1,-1},-1};
static int replay_armed;      /* 1 while a BB_PAD_REPLAY recording plays, 2 once it ended */
static uint64_t replay_start; /* 0: (re)start at the next sample */
static void read_inject(void) {
    static const char *path; static int checked; static uint64_t last_check; static struct timespec mtime;
    if (!checked) { path=getenv("BB_PAD_FILE"); checked=1; }
    if (!path || !*path) return;
    uint64_t now=now_us();
    if (now-last_check<20000) return;
    last_check=now;
    struct stat st;
    if (stat(path,&st)!=0) return;
#ifdef _WIN32
    /* Whole-second mtimes: the size tells two edits within a second apart. */
    if (st.st_mtime==mtime.tv_sec && st.st_size==mtime.tv_nsec) return;
    mtime=(struct timespec){st.st_mtime,(long)st.st_size};
#else
    if (st.st_mtim.tv_sec==mtime.tv_sec && st.st_mtim.tv_nsec==mtime.tv_nsec) return;
    mtime=st.st_mtim;
#endif
    FILE *f=fopen(path,"r");
    if (!f) return;
    static const struct { const char *name; uint32_t ps; } names[]={
        {"cross",BTN_CROSS}, {"circle",BTN_CIRCLE}, {"square",BTN_SQUARE}, {"triangle",BTN_TRIANGLE},
        {"l1",BTN_L1}, {"r1",BTN_R1}, {"l2",BTN_L2}, {"r2",BTN_R2}, {"l3",BTN_L3}, {"r3",BTN_R3},
        {"options",BTN_OPTIONS}, {"touchpad",BTN_TOUCHPAD},
        {"up",BTN_UP}, {"down",BTN_DOWN}, {"left",BTN_LEFT}, {"right",BTN_RIGHT},
    };
    static const char *sticks[]={"lx=","ly=","rx=","ry="};
    injected.buttons=0;
    injected.touch_side=-1;
    for (int i=0;i<4;++i) injected.stick[i]=-1;
    char token[64];
    while (fscanf(f,"%63s",token)==1) {
        if (!strcmp(token,"replay") && replay_armed!=1) { replay_armed=1; replay_start=0; } /* BB_PAD_REPLAY */
        if (!strcmp(token,"touchpad_left") || !strcmp(token,"touchpad_right")) {
            injected.buttons|=BTN_TOUCHPAD;
            injected.touch_side=!strcmp(token,"touchpad_right");
        }
        for (size_t i=0;i<sizeof(names)/sizeof(*names);++i) if (!strcmp(token,names[i].name)) injected.buttons|=names[i].ps;
        for (int i=0;i<4;++i) if (!strncmp(token,sticks[i],3)) { int v=atoi(token+3); injected.stick[i]=v<0 ? 0 : v>255 ? 255 : v; }
    }
    fclose(f);
    printf("Runtime: pad file: buttons 0x%x sticks %d %d %d %d\n",injected.buttons,
           injected.stick[0],injected.stick[1],injected.stick[2],injected.stick[3]);
}
/* BB_PAD_RECORD=<file>: F9 starts and stops recording the pad state (gamepad or keyboard) with
 * the time since F9; BB_PAD_REPLAY=<file> plays such a recording back, started by the token
 * "replay" in BB_PAD_FILE (scripted tests repeat a route the player ran once). Lines: ms buttons
 * lx ly rx ry l2 r2, written when the state changes. */
typedef struct { uint32_t ms, buttons; uint8_t axes[4], l2, r2; } PadSample;
static FILE *record_file;
static uint64_t record_start;
static PadSample record_last;
static void record_sample(const PadData *d) {
    static const char *path; static int checked, f9_was_down;
    if (!checked) { path=getenv("BB_PAD_RECORD"); checked=1; }
    if (!path || !*path || !sdl_ready) return;
    const bool *k=SDL_GetKeyboardState(NULL);
    const int f9=k && k[SDL_SCANCODE_F9];
    if (f9 && !f9_was_down) {
        if (record_file) {
            fclose(record_file); record_file=NULL;
            printf("Runtime: pad recording stopped (%s)\n",path);
        } else if ((record_file=fopen(path,"w"))) {
            record_start=now_us();
            memset(&record_last,0xff,sizeof(record_last));
            printf("Runtime: pad recording started (%s, F9 stops)\n",path);
        }
    }
    f9_was_down=f9;
    if (!record_file) return;
    PadSample s={(uint32_t)((now_us()-record_start)/1000),d->buttons,
                 {d->left_x,d->left_y,d->right_x,d->right_y},d->l2,d->r2};
    if (s.buttons==record_last.buttons && !memcmp(s.axes,record_last.axes,4) &&
        s.l2==record_last.l2 && s.r2==record_last.r2) return;
    record_last=s;
    fprintf(record_file,"%u %u %u %u %u %u %u %u\n",s.ms,s.buttons,s.axes[0],s.axes[1],s.axes[2],
            s.axes[3],s.l2,s.r2);
    fflush(record_file);
}
static PadSample *replay; static size_t replay_count, replay_next;
static void replay_sample(PadData *d) {
    if (!replay_armed) return;
    if (!replay_start) {
        static int loaded;
        if (!loaded) {
            loaded=1;
            const char *path=getenv("BB_PAD_REPLAY");
            FILE *f=path ? fopen(path,"r") : NULL;
            PadSample s; unsigned v[8]; size_t cap=0;
            while (f && fscanf(f,"%u %u %u %u %u %u %u %u",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7])==8) {
                s=(PadSample){v[0],v[1],{(uint8_t)v[2],(uint8_t)v[3],(uint8_t)v[4],(uint8_t)v[5]},(uint8_t)v[6],(uint8_t)v[7]};
                if (replay_count==cap && !(replay=realloc(replay,(cap=cap ? cap*2 : 1024)*sizeof(*replay)))) break;
                replay[replay_count++]=s;
            }
            if (f) fclose(f);
            printf("Runtime: pad replay of %zu samples from %s\n",replay_count,path ? path : "(unset)");
        }
        replay_start=now_us();
        replay_next=0;
    }
    const uint32_t ms=(uint32_t)((now_us()-replay_start)/1000);
    while (replay_next<replay_count && replay[replay_next].ms<=ms) ++replay_next;
    if (!replay_next) return;
    if (replay_next==replay_count && ms>replay[replay_count-1].ms+500) {
        if (replay_armed==1) { puts("Runtime: pad replay finished"); replay_armed=2; }
        return;
    }
    const PadSample *s=&replay[replay_next-1];
    d->buttons=s->buttons;
    d->left_x=s->axes[0]; d->left_y=s->axes[1]; d->right_x=s->axes[2]; d->right_y=s->axes[3];
    d->l2=s->l2; d->r2=s->r2;
}
/* After the menu, the map or the text dialog closes, buttons, sticks and triggers still held
 * (the Cross that accepted a name) stay hidden until released: the game would take them as a
 * new press. (Hunter's Atlas, runtime_atlas_release.h.) */
static BbAtlasRelease atlas_release;
/* The buttons the game was last given (runtime_pad_buttons: the in-game PC menu's paging). */
static volatile uint32_t delivered_buttons;
uint32_t runtime_pad_buttons(void) { return delivered_buttons; }
static void sample_game(PadData *d);
static void sample(PadData *d) {
    sample_game(d);
    finish_touch(d);
    delivered_buttons=d->buttons;
}
static void sample_game(PadData *d) {
    sample_host(d);
    record_sample(d);
    read_inject();
    replay_sample(d);
    d->buttons|=injected.buttons;
    if (injected.touch_side>=0) touch_click(d,injected.touch_side);
    else if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0);
    if (injected.buttons & BTN_L2) d->l2=255;
    if (injected.buttons & BTN_R2) d->r2=255;
    uint8_t *axes[4]={&d->left_x,&d->left_y,&d->right_x,&d->right_y};
    for (int i=0;i<4;++i) if (injected.stick[i]>=0) *axes[i]=(uint8_t)injected.stick[i];
    const uint32_t raw_buttons=d->buttons;
    uint8_t sampled_axes[4]={d->left_x,d->left_y,d->right_x,d->right_y},sampled_triggers[2]={d->l2,d->r2};
    const int capture_before=bbgpu_overlay_captures_input();
    /* Hunter's Atlas: the quick menu's Map tile and the open map take the buttons they use. The
     * open map handles the mouse itself (drag to pan, click a lamp to travel), so the buttons
     * the mouse's bindings hold stay out of its pad input: a left click is R1, which the map
     * reads as the next lamp, and every drag or travel click also moved the selection (#41). */
    const uint32_t routed_in=capture_before ? d->buttons & ~host_mouse_buttons : d->buttons;
    d->buttons=bbgpu_atlas_route_pad(routed_in,d->left_x,d->left_y,d->right_x,d->right_y);
    if (capture_before || bbgpu_overlay_captures_input()) {
        bb_atlas_remember(&atlas_release,raw_buttons,sampled_axes,sampled_triggers);
        d->buttons=0; d->left_x=d->left_y=d->right_x=d->right_y=128; d->l2=d->r2=0; d->touch_count=0;
        return;
    }
    const uint32_t routed=d->buttons;
    bb_atlas_release_filter(&atlas_release,raw_buttons,&d->buttons,sampled_axes,sampled_triggers);
    if (d->buttons!=routed) d->touch_count=0;
    d->left_x=sampled_axes[0]; d->left_y=sampled_axes[1]; d->right_x=sampled_axes[2]; d->right_y=sampled_axes[3];
    d->l2=sampled_triggers[0]; d->r2=sampled_triggers[1];
    runtime_stamina_tick(d->buttons);
}

static ABI int32_t pad_init(void) { pthread_mutex_lock(&lock); initialized=1; pthread_mutex_unlock(&lock); return 0; }
static ABI int32_t pad_open(int32_t user, int32_t type, int32_t index, const void *param) {
    (void)param;
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (user!=1) return ERR_INVALID_ARG;
    if (type!=0 && type!=2) return ERR_INVALID_ARG; /* standard / special port */
    if (index) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    int already=opened; opened=1;
    pthread_mutex_unlock(&lock);
    if (already) return ERR_ALREADY_OPENED;
    puts("Runtime: pad opened for user 1 (SDL gamepad or keyboard)");
    return PAD_HANDLE;
}
static ABI int32_t pad_close(int32_t handle) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    opened=0; return 0;
}
static ABI int32_t pad_read_state(int32_t handle, PadData *data) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!data) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    sample(data); ++reads;
    pthread_mutex_unlock(&lock);
    return 0;
}
/* Buffered read: the port samples once per call, so one entry is returned. */
static ABI int32_t pad_read(int32_t handle, PadData *data, int32_t count) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!data || count<1 || count>64) return ERR_INVALID_ARG;
    pad_read_state(handle,data);
    return 1;
}
static ABI int32_t pad_info(int32_t handle, ControllerInfo *info) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!info) return ERR_INVALID_ARG;
    memset(info,0,sizeof(*info));
    info->pixel_density=44.86f; info->resolution_x=1920; info->resolution_y=943;
    info->dead_zone_left=info->dead_zone_right=2;
    info->connection_type=0; info->connected=1; info->device_class=0;
    pthread_mutex_lock(&lock);
    current_gamepad();
    info->connected_count=connected_count ? connected_count : 1;
    pthread_mutex_unlock(&lock);
    return 0;
}
/* Short effects on Xbox pads (#213): a hit rumbles both motors for a while, but a visceral attack
 * is a brief jolt. A DualShock's motors answer within a frame; an XInput pad's need tens of
 * milliseconds to spin up, so a pulse the game stops a frame or two later was never felt, and
 * its right (high-frequency) motor is much weaker than the DualShock's small motor. A pulse is
 * therefore kept for at least PAD_RUMBLE_MIN_US (a stop that comes sooner keeps the last values
 * and lets SDL's duration end them), and on an Xbox pad an effect on the small motor alone also
 * drives the large one at half its strength. */
#define PAD_RUMBLE_MIN_US 150000u
static uint16_t rumble_low, rumble_high;   /* the effect running, after the mapping above */
static uint64_t rumble_since;              /* when it started */
static ABI int32_t pad_vibration(int32_t handle, const uint8_t *param) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!param) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    SDL_Gamepad *g=current_gamepad();
    uint16_t low=(uint16_t)(param[0]*257), high=(uint16_t)(param[1]*257);
    if (pad_xbox && !low && high) low=high/2;
    uint32_t duration_ms=1000;
    const uint64_t now=now_us();
    if (low || high) {
        if (!rumble_low && !rumble_high) rumble_since=now;
        rumble_low=low; rumble_high=high;
    } else if ((rumble_low || rumble_high) && now-rumble_since<PAD_RUMBLE_MIN_US) {
        low=rumble_low; high=rumble_high;
        duration_ms=(uint32_t)((rumble_since+PAD_RUMBLE_MIN_US-now+999)/1000);
    } else rumble_low=rumble_high=0;
    const bool ok=g && SDL_RumbleGamepad(g,low,high,duration_ms);
    /* BB_PAD_VIBRATION_LOG=1: what the game asks for (large, small motor) and whether SDL took
     * it, to compare pads where some effects do not rumble (#213). */
    static int log_vibration=-1;
    if (log_vibration<0) { const char *e=getenv("BB_PAD_VIBRATION_LOG"); log_vibration=e && *e=='1'; }
    if (log_vibration)
        printf("Runtime: vibration large=%u small=%u -> %s (%s)\n",param[0],param[1],
               ok ? "ok" : g ? SDL_GetError() : "no gamepad",g ? SDL_GetGamepadName(g) : "-");
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t pad_ok_handle(int32_t handle) { return handle==PAD_HANDLE && opened ? 0 : ERR_INVALID_HANDLE; }
static ABI int32_t pad_ok_handle_flag(int32_t handle, uint8_t flag) { (void)flag; return pad_ok_handle(handle); }

static const RuntimeExport exports[]={
    {"scePadInit",pad_init}, {"scePadOpen",pad_open}, {"scePadClose",pad_close},
    {"scePadReadState",pad_read_state}, {"scePadRead",pad_read},
    {"scePadGetControllerInformation",pad_info}, {"scePadSetVibration",pad_vibration},
    {"scePadResetOrientation",pad_ok_handle},
    {"scePadSetAngularVelocityDeadbandState",pad_ok_handle_flag}, {"scePadSetTiltCorrectionState",pad_ok_handle_flag},
    {"scePadSetMotionSensorState",pad_ok_handle_flag},
};
uintptr_t runtime_pad_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_pad_report(void) { printf("Runtime: pad reads=%zu, gamepad=%s\n",reads,gamepad ? SDL_GetGamepadName(gamepad) : "none"); }
