// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: user settings changed at run time from the in-game menu (bbport_overlay.h) and kept
// in bbport.ini (BB_CONFIG overrides the path). Environment variables override the file at
// start. Readers load the atomics every frame; writers are the menu and Load().

#pragma once

#include <atomic>
#include <string>
#include <utility>
#include <vector>

namespace BbSettings {

enum Upscaler : int { UpscalerOff = 0, UpscalerFsr3 = 1, UpscalerFsr4 = 2, UpscalerFsr411 = 3,
                      UpscalerTaa = 4, UpscalerDlss = 5, UpscalerCount };
/// FSR 4 v07 or FSR 4.1.1: the same inputs, settings and placement in the frame.
inline bool IsFsr4(int upscaler) {
    return upscaler == UpscalerFsr4 || upscaler == UpscalerFsr411;
}
enum Preset : int { NativeAA = 0, Quality, Balanced, Performance, UltraPerformance, PresetCount };
enum DebugView : int { DebugNone = 0, DebugReactive = 1, DebugMotion = 2, DebugViewCount };
/// Frame generation (vk_frame_generation.h): frames generated between two of the game's.
enum FrameGen : int { FrameGenOff = 0, FrameGenFsr3 = 1, FrameGenDlss = 2, FrameGenCount };

/// Game effects switched by the community patches at start (patches.py EFFECTS): ini key,
/// menu label, default (the game's own behaviour).
struct Effect {
    const char* key;
    const char* label;
    bool default_on;
};
inline constexpr Effect Effects[] = {
    {"effect_chromatic_aberration", "Chromatic aberration", true},
    {"effect_dof", "Depth of field (DoF)", true},
    {"effect_motion_blur", "Motion blur", true},
    {"effect_ssao", "Ambient occlusion (SSAO)", true},
    {"effect_game_aa", "Game anti-aliasing", true},
    {"effect_dynamic_shadows", "Dynamic shadows", true},
    {"effect_ssr", "Screen-space reflections (SSR)", false},
    {"skip_intro", "Skip intro logos and movie", false},
    {"debug_camera", "Free camera (Cross + L3)", false},
    {"debug_menu", "Debug menu (requires font files)", false},
    {"cheat_no_death", "Cheat: never die (minimum 1 HP)", false},
    {"cheat_stealth", "Cheat: enemies cannot see you", false},
    {"cheat_silent", "Cheat: enemies cannot hear you", false},
    {"cheat_rally_no_decay", "Cheat: Rally never decays", false},
    {"cheat_enemy_control", "Cheat: control targeted enemy (R3 / L3)", false},
    {"cheat_no_stamina", "Cheat: no stamina use", false},
    {"cheat_stamina_out_of_combat", "Cheat: no stamina use out of combat", false},
    {"tweak_no_rally", "Disable Rally (health recovery)", false},
    {"tweak_camera_distance", "Increase camera distance", false},
    {"tweak_no_camera_rotation", "Disable automatic camera rotation", false},
    {"tweak_easy_run", "Run with less stick tilt", false},
    {"tweak_ragdoll", "Dark Souls-style ragdoll physics", false},
    // PC enhancements (scripts/enhancements.py, after bbhost): game files made at start. Off
    // keeps the game as it shipped. Kept last: the menu lists them in their own section.
    {"pc_mirror_appearance", "Hunter's Dream mirror: change appearance", false},
    {"pc_two_invaders", "Two invaders in the late areas (every player needs it)", false},
    {"pc_rebirth", "Rebirth in the Nightmare at the Altar of Despair", false},
    {"pc_enhanced_lamps", "Lamps: Level Up and Storage at every lit lamp", false},
};
inline constexpr int EffectCount = int(sizeof(Effects) / sizeof(Effects[0]));
inline constexpr int EnhancementCount = 4;
inline constexpr int FirstEnhancement = EffectCount - EnhancementCount;
/// Off while playing online (BB_ONLINE=1, but not seamless co-op's BB_COOP=1), whatever
/// bbport.ini says: the cheats, the free camera and the debug menu (patches.py ONLINE_BLOCKED).
bool Online();
bool OnlineBlocked(int effect);
/// Live output resolutions: the upscaler's output and the UI host targets.
// Last slot retains a validated custom resolution loaded from the launcher/ini.
// Wider than 16:9 (2560x1080 .. 5120x1440, 3840x1600, 3840x1080): camera and HUD patched at start
// (patches.py ultrawide_writes); src/pc_menu.c names them by index.
inline int OutputWidths[] = {1280,1920,2560,3840,1280,2560,3440,5120,5120,1080,3840,3840,1920,1366,1920};
inline int OutputHeights[] = {720,1080,1440,2160,800,1080,1440,1440,2160,1920,1600,1080,1200,768,1080};
inline constexpr int OutputCount = sizeof(OutputWidths) / sizeof(OutputWidths[0]);
inline constexpr int OutputCustom = OutputCount - 1;
inline constexpr int OutputDefault = 1; ///< 1920x1080, the game's own size

struct Values {
    std::atomic<int> upscaler{UpscalerFsr3};
    std::atomic<int> preset{NativeAA};
    std::atomic<bool> sharpen{true};
    std::atomic<float> sharpness{0.3f};
    std::atomic<bool> jitter{true};
    std::atomic<bool> reactive{false};
    std::atomic<bool> object_motion{true};
    std::atomic<float> reactive_scale{1.0f};
    std::atomic<float> reactive_threshold{0.2f};
    std::atomic<float> reactive_max{0.9f};
    std::atomic<int> debug_view{DebugNone};
    std::atomic<bool> show_fps{false};
    /// The window (the launcher's setting, BB_FULLSCREEN): the in-game PC menu changes it live
    /// and writes it back to the launcher's settings.json, not bbport.ini.
    std::atomic<bool> fullscreen{false};
    // FSR 4 checks (menu): the provider's auto exposure, the jitter sign it is given.
    std::atomic<bool> fsr4_auto_exposure{true};
    std::atomic<bool> fsr4_invert_jitter{false};
    /// Mouse camera (src/mouse_camera.c): the mouse turns the camera directly, 1:1, while the
    /// game window holds it (a click captures it). Sensitivity in tenths: 10 = 900 counts per
    /// radian. BB_MOUSE_CAMERA=0 turns it off.
    std::atomic<bool> mouse_camera{true};
    std::atomic<int> mouse_sensitivity{10};
    std::atomic<bool> mouse_invert_y{false};
    /// Locked on, a sideways flick of this many mouse counts (decaying over ~0.1 s) switches
    /// targets; 0 turns flicks off.
    std::atomic<int> mouse_flick_threshold{300};
    /// Menu pointer (src/menu_pointer.c): the mouse points and clicks in the game's menus.
    /// BB_MOUSE_MENU=0 turns it off.
    std::atomic<bool> mouse_menu{true};
    /// The cursor stays hidden over the game window, menus included (#88: for mouse-to-pad
    /// tools); the guide menu, the map and text entry still show it.
    std::atomic<bool> mouse_hide_cursor{false};
    /// L3 + R3 on a controller opens this menu (Insert always does). Off for mods that put a
    /// move on L3, such as jumping, where L3 + R3 (lock on) opened it mid-fight (#184).
    std::atomic<bool> overlay_pad_shortcut{true};
    std::atomic<int> active_render_width{1920}, active_render_height{1080};
    /// Applied at start (patches.py); the menu shows when a restart is needed.
    std::atomic<bool> effects[EffectCount]{};
    std::atomic<int> model_lod{0}; ///< -2 highest .. 2 lowest, 0 the game's
    std::atomic<int> output_res{OutputDefault}; ///< index into OutputWidths
    /// Live resolution and preset changes (run.sh): 0 off by default (startup patch, fastest
    /// on the Steam Deck and older GPUs), -1 auto (strong discrete GPUs), 1 on. On restart.
    std::atomic<int> live_resolution{0};
    /// Why FSR 4 cannot run (assets, device features), or null. Set by the renderer.
    std::atomic<const char*> fsr4_problem{nullptr};
    std::atomic<bool> fsr4_supported{false}, fsr411_supported{false};
    /// FSR 4 recorded its last frame (the renderer); a device loss then is noted below.
    std::atomic<bool> fsr4_running{false};
    /// The GPU was lost while FSR 4 ran (#151: RX 6700 XT, AMD 26.10.1): bbport.ini
    /// fsr4_device_lost=1, and the next start saves FSR 3.1 instead and says why.
    std::atomic<bool> fsr4_device_lost{false};
    /// DLSS (bbport_dlss.dll, NVIDIA RTX) is ready, or why not (null before the device exists).
    std::atomic<bool> dlss_supported{false};
    std::atomic<const char*> dlss_problem{nullptr};
    /// Frame generation and the frames per game frame (2x with FSR 3.1, up to 4x with DLSS on
    /// RTX 50). dlssg_max_multiplier: 1 without DLSS Frame Generation.
    std::atomic<int> frame_gen{FrameGenOff};
    std::atomic<int> frame_gen_multiplier{2};
    /// At most the frames the display shows between two of the game's (an uncapped game then
    /// gets fewer generated frames); off: the multiplier as chosen.
    std::atomic<bool> frame_gen_fit_display{false};
    std::atomic<int> dlssg_max_multiplier{1};
    std::atomic<const char*> frame_gen_problem{nullptr};
    /// Generated frames per game frame the display has room for now (0: paused), and its
    /// refresh rate (the presenter's measure, for the menu).
    std::atomic<int> frame_gen_now{0};
    std::atomic<int> frame_gen_display_hz{0};
    /// The game's own frame rate (presenter, game frames only), shown beside the total when
    /// frame generation adds frames.
    std::atomic<float> game_fps{0.0f};
    /// The game's frame rate cap frame generation needs (VideoOut applies it), 0 for none: with
    /// FIFO presentation (V-Sync; AMD has no Mailbox) at most refresh / (generated + 1), or each
    /// extra frame waits for the display and generation pauses.
    std::atomic<int> frame_gen_flip_limit{0};

    /// Startup settings for the explicit BB_RENDER_RES compatibility patch only.
    int startup_preset = NativeAA;
    int startup_upscaler = UpscalerFsr3;
    bool startup_object_motion = true;
    bool startup_effects[EffectCount]{};
    int startup_model_lod = 0;
    int startup_output_res = OutputDefault;
    int startup_live_resolution = 0;
};

Values& Get();

/// Reads the file, then the environment overrides. Called once at start.
void Load();
/// Checks the loaded choice before the first frame; unsupported FSR 4 uses FSR 3.1.
void ConfigureUpscalerSupport(bool fsr4, bool fsr411);
/// After device creation: DLSS availability; a DLSS setting falls back to FSR 3.1 without it.
void ConfigureDlssSupport(bool available, const char* problem);
/// Startup-patched scene dimensions cannot change until run.sh prepares a new image.
bool FixedRenderSession();
int RenderPreset();
bool ResolutionNeedsRestart();
/// Camera and HUD aspect changes require new guest patches on restart.
bool AspectNeedsRestart();
/// Writes the file (menu changes).
void Save();
/// Sets these keys in the file (each in its place, a new one at the end), and with drop_prefix
/// removes every other line whose key starts with it; the rest stays as it is (key_ bindings).
void RewriteKeys(const std::vector<std::pair<std::string, std::string>>& keys, const char* drop_prefix);
/// The GPU was lost: with FSR 4 running, writes fsr4_device_lost=1 for the next start.
void NoteDeviceLost();

/// Render resolution divisor of a preset (1.0 native, 1.5 quality, ...).
float PresetScale(int preset);
const char* PresetName(int preset);
const char* UpscalerName(int upscaler);
const char* FrameGenName(int frame_gen);

} // namespace BbSettings
