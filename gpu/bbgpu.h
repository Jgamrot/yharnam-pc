/* C interface of the GPU library (gpu/): shadPS4's Liverpool/Vulkan video core,
 * GnmDriver, VideoOut and kernel event queues, adapted to the native loader. */
#ifndef BBGPU_H
#define BBGPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;          /* window title */
    const char *serial;         /* CUSA id, names the pipeline cache */
    const char *user_dir;       /* pipeline cache/logs directory */
    uint32_t sdk_version;       /* from the eboot's procparam */
    uint32_t psf_attributes;    /* param.sfo ATTRIBUTE */
    int32_t width, height;      /* initial window size */
} BbGpuConfig;
/* Registers kernel event queues (needed with or without graphics). */
void bbgpu_register_kernel(void);
/* Co-op: the game folder and param.sfo identity for the network and PSN libraries. */
void bbnet_configure(const char *app0, const char *serial, const char *title, const char *app_ver);
/* The game's code before the patches (The Hunter's Dream backend), then the online session's
 * start (bbnet/bbnet_glue.cpp). */
void bbnet_set_image(unsigned char *image, uint64_t size);
void bbnet_initialize(void);
/* Creates window, Vulkan device, presenter and GPU command processor. */
int bbgpu_init(const BbGpuConfig *config);
/* Function for an imported NID ("NID#lib#mod"), or 0 when the GPU library does not provide it. */
uintptr_t bbgpu_resolve(const char *scoped_nid);
/* Called first by the loader's SIGSEGV handler: 1 when a GPU page-tracking fault was handled. */
int bbgpu_handle_fault(void *ucontext, void *address);
/* BB_WRITE_LOG=1: prints the logged GPU-side writes to guest memory near the fault. */
void bbgpu_dump_guest_writes(void *ucontext);
/* Keyboard text entry through the game window (IME dialog). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
int bbgpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int bbgpu_text_input_poll(char *out_utf8, uint64_t size);
/* 1 while the in-game settings menu is open: the game's pad input is held neutral. */
int bbgpu_overlay_captures_input(void);
/* Hunter's Atlas: the quick menu's Map tile and the open map take the pad buttons they use;
 * returns the buttons left for the game. */
uint32_t bbgpu_atlas_route_pad(uint32_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry);
/* Local trophy notifications. IDs are supplied by the game's trophy API, not PSN. */
void bbgpu_trophy_configure(const char *game_dir, const char *user_dir, int32_t language);
void bbgpu_trophy_load(const int64_t *timestamps, uint32_t count);
void bbgpu_trophy_unlocked(int32_t id, int64_t timestamp);
/* Called at the start of every frame submission (sceVideoOutSubmitFlip / SubmitEopFlip), on the
 * game's thread that submits it: the loader's per-frame work for the engine features. */
void bbgpu_set_frame_callback(void (*callback)(void));
/* bbport.ini settings by key, for the in-game PC menu: on/off as 0/1, upscaler as its index,
 * output_res as an index, live_resolution -1 (auto)/0/1, model_lod -2..2. get and default
 * return 0 for a key they do not handle; set also saves the file. */
int bbgpu_setting_get(const char *key, int *value);
int bbgpu_setting_default(const char *key, int *value);
void bbgpu_setting_set(const char *key, int value);
/* Whether this GPU runs the upscaler (BbSettings::Upscaler index). */
int bbgpu_upscaler_supported(int upscaler);
/* Local achievements (BbTrophies: the game's own trophy pack and the unlock history), read
 * only, for the in-game PC menu. Names are empty without the player's trophy key. */
typedef struct {
    int32_t id;
    int32_t hidden;       /* a secret one: shown once earned */
    int32_t score;        /* Gamerscore, -1 when the grade is not known */
    int64_t unlocked_at;  /* Unix seconds, 0 while locked */
    char name[128];       /* UTF-8 */
    char description[256];
} BbAchievement;
int bbgpu_achievement_count(void);
int bbgpu_achievement_get(int index, BbAchievement *out);
/* Earned and total Gamerscore; returns 0 while the total is not the full 1000. */
int bbgpu_achievement_score(int *earned, int *total);
/* The Key Bindings page of the port's menu (Insert): the runtime's actions (src/input_bindings.c).
 * text writes an action's bindings as bbport.ini keeps them; set applies a key_<action> value
 * at once, reset every default. The menu saves the key_ lines itself. */
typedef struct {
    int count;
    const char *(*key)(int action);   /* the bbport.ini name after "key_" */
    const char *(*label)(int action);
    void (*text)(int action, char *out, int size);
    void (*set)(int action, const char *value);
    void (*reset)(void);
} BbBindingsApi;
void bbgpu_set_bindings(const BbBindingsApi *api);
/* Mouse wheel notches for the pad read, one a call: bit 0 up, bit 1 down (input_bindings.h). */
uint32_t bbgpu_mouse_wheel_take(void);
/* The mouse camera: whether the window holds the mouse (relative mode), and the motion since
 * the last call in mouse counts (right and down positive); 1 when there was any. */
int bbgpu_mouse_captured(void);
int bbgpu_mouse_motion_take(float *dx, float *dy);
/* The menu pointer (src/menu_pointer.c): a menu owns the mouse, so the window shows the cursor
 * instead of holding it for the camera, and takes it back for the camera when the menu closes
 * if it had it. */
void bbgpu_set_menu_pointer(int active);
/* The mouse on the menus' 1920x1080 stage (the movie's pixels: the presented frame letterboxed
 * in the window, the stage centred in the frame); 0 when it is outside or the window has no focus. */
int bbgpu_mouse_stage_position(float *x, float *y);
/* Ends the game as closing its window does (the title menu's Quit Game). */
void bbgpu_request_quit(void);
/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned bbgpu_symbol_count(void);
#ifdef __cplusplus
}
#endif
#endif
