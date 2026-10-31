// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_settings.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace BbSettings {

namespace {

const char* Path() {
    const char* env = std::getenv("BB_CONFIG");
    return env && env[0] ? env : "bbport.ini";
}

// Sets `key` in the file to `value` and removes `drop`, keeping every other line as it is.
void RewriteKey(const char* key, const char* value, const char* drop) {
    std::vector<std::string> lines;
    bool found = false;
    if (FILE* file = std::fopen(Path(), "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), file)) {
            std::string text{line};
            text.erase(text.find_last_not_of("\r\n") + 1);
            const std::string name = text.substr(0, text.find('='));
            if (name == drop) continue;
            if (name == key) {
                text = std::string(key) + "=" + value;
                found = true;
            }
            lines.push_back(text);
        }
        std::fclose(file);
    }
    if (!found) lines.push_back(std::string(key) + "=" + value);
    if (FILE* file = std::fopen(Path(), "w")) {
        for (const std::string& line : lines) std::fprintf(file, "%s\n", line.c_str());
        std::fclose(file);
    }
}

float Clamp(float v, float lo, float hi) {
    return std::clamp(v, lo, hi);
}

void Set(Values& v, const std::string& key, const std::string& value) {
    const float f = float(std::atof(value.c_str()));
    const int i = std::atoi(value.c_str());
    if (key == "upscaler") {
        for (int u = 0; u < UpscalerCount; ++u) {
            if (value == UpscalerName(u)) {
                v.upscaler = u;
            }
        }
    } else if (key == "frame_gen") {
        for (int g = 0; g < FrameGenCount; ++g) {
            if (value == FrameGenName(g)) {
                v.frame_gen = g;
            }
        }
    } else if (key == "frame_gen_multiplier") {
        v.frame_gen_multiplier = std::clamp(i, 2, 4);
    } else if (key == "frame_gen_fit_display") {
        v.frame_gen_fit_display = i != 0;
    } else if (key == "preset") {
        v.preset = std::clamp(i, 0, PresetCount - 1);
    } else if (key == "sharpen") {
        v.sharpen = i != 0;
    } else if (key == "sharpness") {
        v.sharpness = Clamp(f, 0.0f, 2.0f);
    } else if (key == "jitter") {
        v.jitter = i != 0;
    } else if (key == "reactive") {
        v.reactive = i != 0;
    } else if (key == "object_motion") {
        v.object_motion = i != 0;
    } else if (key == "reactive_scale") {
        v.reactive_scale = Clamp(f, 0.0f, 16.0f);
    } else if (key == "reactive_threshold") {
        v.reactive_threshold = Clamp(f, 0.0f, 1.0f);
    } else if (key == "reactive_max") {
        v.reactive_max = Clamp(f, 0.0f, 1.0f);
    } else if (key == "debug_view") {
        v.debug_view = std::clamp(i, 0, DebugViewCount - 1);
    } else if (key == "show_fps") {
        v.show_fps = i != 0;
    } else if (key == "fsr4_auto_exposure") {
        v.fsr4_auto_exposure = i != 0;
    } else if (key == "fsr4_invert_jitter") {
        v.fsr4_invert_jitter = i != 0;
    } else if (key == "fsr4_device_lost") {
        v.fsr4_device_lost = i != 0;
    } else if (key == "mouse_camera") {
        v.mouse_camera = i != 0;
    } else if (key == "mouse_sensitivity") {
        v.mouse_sensitivity = std::clamp(i, 1, 50);
    } else if (key == "mouse_invert_y") {
        v.mouse_invert_y = i != 0;
    } else if (key == "mouse_flick_threshold") {
        v.mouse_flick_threshold = std::clamp(i, 0, 10000);
    } else if (key == "mouse_menu") {
        v.mouse_menu = i != 0;
    } else if (key == "mouse_hide_cursor") {
        v.mouse_hide_cursor = i != 0;
    } else if (key == "overlay_pad_shortcut") {
        v.overlay_pad_shortcut = i != 0;
    } else if (key == "model_lod") {
        v.model_lod = std::clamp(i, -2, 2);
    } else if (key == "live_resolution") {
        v.live_resolution = value == "auto" ? -1 : std::clamp(i, 0, 1);
    } else if (key == "output_res") {
        for (int r = 0; r < OutputCustom; ++r) {
            if (value == std::to_string(OutputWidths[r]) + "x" + std::to_string(OutputHeights[r])) {
                v.output_res = r;
                return;
            }
        }
        const auto separator = value.find('x');
        if (separator == std::string::npos) return;
        int w = 0, h = 0;
        const char* begin = value.data();
        const char* split = begin + separator;
        const char* end = begin + value.size();
        const auto width = std::from_chars(begin, split, w);
        const auto height = std::from_chars(split + 1, end, h);
        if (width.ec == std::errc{} && width.ptr == split &&
            height.ec == std::errc{} && height.ptr == end &&
            w >= 256 && w <= 7680 && h >= 144 && h <= 4320 && !(w & 1) && !(h & 1)) {
            OutputWidths[OutputCustom] = w;
            OutputHeights[OutputCustom] = h;
            v.output_res = OutputCustom;
        }
    } else {
        for (int e = 0; e < EffectCount; ++e) {
            if (key == Effects[e].key) {
                v.effects[e] = i != 0;
            }
        }
    }
}

} // namespace

Values& Get() {
    static Values values;
    return values;
}

void Load() {
    auto& v = Get();
    for (int e = 0; e < EffectCount; ++e) {
        v.effects[e] = Effects[e].default_on;
    }
    if (FILE* file = std::fopen(Path(), "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), file)) {
            std::string text{line};
            text.erase(text.find_last_not_of(" \t\r\n") + 1);
            const auto eq = text.find('=');
            if (text.empty() || text[0] == '#' || eq == std::string::npos) {
                continue;
            }
            Set(v, text.substr(0, eq), text.substr(eq + 1));
        }
        std::fclose(file);
        std::printf("Settings: %s\n", Path());
    }
    // Environment overrides (scripts, A/B tests).
    if (const char* env = std::getenv("BB_UPSCALER")) {
        v.upscaler = UpscalerOff;
        for (int u = 0; u < UpscalerCount; ++u) {
            if (std::strcmp(env, UpscalerName(u)) == 0) v.upscaler = u;
        }
    }
    const std::pair<const char*, const char*> env_keys[] = {
        {"BB_FSR_SHARPNESS", "sharpness"},        {"BB_JITTER", "jitter"},
        {"BB_REACTIVE", "reactive"},              {"BB_REACTIVE_SCALE", "reactive_scale"},
        {"BB_REACTIVE_THRESHOLD", "reactive_threshold"}, {"BB_REACTIVE_MAX", "reactive_max"},
        {"BB_UPSCALE_PRESET", "preset"},            {"BB_OBJECT_MOTION", "object_motion"},
        {"BB_PC_MIRROR", "pc_mirror_appearance"},   {"BB_PC_TWO_INVADERS", "pc_two_invaders"},
        {"BB_PC_REBIRTH", "pc_rebirth"},           {"BB_MOUSE_CAMERA", "mouse_camera"},
        {"BB_PC_LAMPS", "pc_enhanced_lamps"},
        {"BB_OUTPUT_RES", "output_res"},
        {"BB_MOUSE_MENU", "mouse_menu"},
        {"BB_FRAME_GEN", "frame_gen"},              {"BB_FRAME_GEN_MULTIPLIER", "frame_gen_multiplier"},
        {"BB_FRAME_GEN_FIT_DISPLAY", "frame_gen_fit_display"},
    };
    for (const auto& [env, key] : env_keys) {
        if (const char* value = std::getenv(env)) {
            Set(v, key, value);
        }
    }
    // The window mode is the launcher's setting, passed at start.
    const char* fullscreen = std::getenv("BB_FULLSCREEN");
    v.fullscreen = fullscreen && fullscreen[0] == '1';
    v.startup_preset = v.preset;
    v.startup_upscaler = v.upscaler;
    v.startup_object_motion = v.object_motion;
    for (int e = 0; e < EffectCount; ++e) {
        v.startup_effects[e] = v.effects[e];
    }
    v.startup_model_lod = v.model_lod;
    v.startup_output_res = v.output_res;
    v.startup_live_resolution = v.live_resolution;
}

void ConfigureUpscalerSupport(bool fsr4, bool fsr411) {
    auto& v = Get();
    v.fsr4_supported = fsr4;
    v.fsr411_supported = fsr4 && fsr411;
    const int requested = v.upscaler;
    if ((requested == UpscalerFsr4 && !v.fsr4_supported) ||
        (requested == UpscalerFsr411 && !v.fsr411_supported)) {
        v.fsr4_problem = "GPU does not support the selected FSR 4 shaders; using FSR 3.1";
        std::printf("Upscaler: %s unsupported on this GPU; falling back to FSR 3.1 before the first frame\n",
                    UpscalerName(requested));
        v.upscaler = UpscalerFsr3;
    } else if (IsFsr4(requested) && v.fsr4_device_lost) {
        v.fsr4_problem = "The GPU was lost with FSR 4 on last time; using FSR 3.1. Choose FSR 4 again to retry";
        std::printf("Upscaler: the GPU was lost while %s ran last time; using FSR 3.1 "
                    "(choose it again in the menu to retry)\n", UpscalerName(requested));
        v.upscaler = UpscalerFsr3;
        // Saved as FSR 3.1, so choosing FSR 4 again (here or in the launcher) is the retry.
        v.fsr4_device_lost = false;
        RewriteKey("upscaler", UpscalerName(UpscalerFsr3), "fsr4_device_lost");
    }
}

void ConfigureDlssSupport(bool available, const char* problem) {
    auto& v = Get();
    v.dlss_supported = available;
    static std::string kept;
    kept = problem ? problem : "";
    v.dlss_problem = available || kept.empty() ? nullptr : kept.c_str();
    if (v.upscaler == UpscalerDlss && !available) {
        std::printf("Upscaler: DLSS unavailable (%s); falling back to FSR 3.1\n",
                    kept.empty() ? "bbport_dlss.dll or nvngx_dlss.dll missing" : kept.c_str());
        v.upscaler = UpscalerFsr3;
    }
}

bool FixedRenderSession() {
    const char* size = std::getenv("BB_RENDER_RES");
    return size && size[0];
}

int RenderPreset() {
    const auto& v = Get();
    return FixedRenderSession() ? v.startup_preset :
        v.upscaler == UpscalerTaa ? NativeAA : v.preset.load();
}

// 16:9 to within a pixel of width: 1366x768 plays as 16:9 (patches.py is_16_9).
static bool Is16By9(int output) {
    return std::llabs(int64_t(OutputWidths[output]) * 9 - int64_t(OutputHeights[output]) * 16) <= 9;
}

bool AspectNeedsRestart() {
    const auto& v = Get();
    const int a = v.output_res, b = v.startup_output_res;
    if (Is16By9(a) && Is16By9(b)) return false;
    return int64_t(OutputWidths[a]) * OutputHeights[b] !=
           int64_t(OutputWidths[b]) * OutputHeights[a];
}

bool ResolutionNeedsRestart() {
    const auto& v = Get();
    // TAA needs the live path (native guest targets): run.sh selects it on restart.
    return AspectNeedsRestart() || (FixedRenderSession() &&
        (v.preset != v.startup_preset || v.output_res != v.startup_output_res ||
         (v.upscaler == UpscalerOff) != (v.startup_upscaler == UpscalerOff) ||
         (v.upscaler == UpscalerTaa) != (v.startup_upscaler == UpscalerTaa)));
}

namespace {
bool device_lost_now = false; ///< Save() from NoteDeviceLost: FSR 4 is the upscaler that failed
} // namespace

void Save() {
    auto& v = Get();
    // Choosing FSR 4 again in the menu is the retry: the note goes.
    if (IsFsr4(v.upscaler) && !device_lost_now) v.fsr4_device_lost = false;
    // Lines this function does not write (keys other tools own, such as pc_menu) are kept, once
    // each: a key repeated in the file keeps its last line, the one Load() applies.
    std::vector<std::string> kept;
    if (FILE* old = std::fopen(Path(), "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), old)) {
            std::string text{line};
            text.erase(text.find_last_not_of(" \t\r\n") + 1);
            const auto eq = text.find('=');
            if (text.empty() || text[0] == '#' || eq == std::string::npos) continue;
            const std::string key = text.substr(0, eq);
            bool ours = false;
            for (const char* name : {"upscaler", "preset", "sharpen", "sharpness", "jitter", "reactive",
                                     "object_motion", "reactive_scale", "reactive_threshold", "reactive_max",
                                     "debug_view", "show_fps", "fsr4_auto_exposure", "fsr4_invert_jitter", "fsr4_device_lost",
                                     "model_lod", "output_res", "live_resolution",
                                     "frame_gen", "frame_gen_multiplier", "frame_gen_fit_display",
                                     "mouse_camera", "mouse_sensitivity", "mouse_invert_y", "mouse_menu",
                                     "mouse_hide_cursor", "mouse_flick_threshold", "overlay_pad_shortcut"}) {
                ours |= key == name;
            }
            for (int e = 0; e < EffectCount; ++e) ours |= key == Effects[e].key;
            if (ours) continue;
            std::erase_if(kept, [&](const std::string& old) {
                return old.compare(0, eq + 1, text, 0, eq + 1) == 0;
            });
            kept.push_back(text);
        }
        std::fclose(old);
    }
    FILE* file = std::fopen(Path(), "w");
    if (!file) {
        std::printf("Settings: cannot write %s\n", Path());
        return;
    }
    std::fprintf(file,
                 "# bbport settings (in-game menu: Insert / L3+R3)\n"
                 "upscaler=%s\npreset=%d\nsharpen=%d\nsharpness=%.2f\njitter=%d\n"
                 "reactive=%d\nobject_motion=%d\nreactive_scale=%.2f\nreactive_threshold=%.2f\nreactive_max=%.2f\n"
                 "debug_view=%d\nshow_fps=%d\nfsr4_auto_exposure=%d\nfsr4_invert_jitter=%d\n",
                 UpscalerName(v.upscaler), v.preset.load(), int(v.sharpen.load()),
                 v.sharpness.load(), int(v.jitter.load()), int(v.reactive.load()),
                 int(v.object_motion.load()),
                 v.reactive_scale.load(), v.reactive_threshold.load(), v.reactive_max.load(),
                 v.debug_view.load(), int(v.show_fps.load()),
                 int(v.fsr4_auto_exposure.load()), int(v.fsr4_invert_jitter.load()));
    if (v.fsr4_device_lost) std::fprintf(file, "fsr4_device_lost=1\n");
    std::fprintf(file, "mouse_camera=%d\nmouse_sensitivity=%d\nmouse_invert_y=%d\nmouse_menu=%d\n"
                       "mouse_hide_cursor=%d\nmouse_flick_threshold=%d\n",
                 int(v.mouse_camera.load()), v.mouse_sensitivity.load(), int(v.mouse_invert_y.load()),
                 int(v.mouse_menu.load()), int(v.mouse_hide_cursor.load()), v.mouse_flick_threshold.load());
    std::fprintf(file, "overlay_pad_shortcut=%d\n", int(v.overlay_pad_shortcut.load()));
    // Read by patches.py at start.
    for (int e = 0; e < EffectCount; ++e) {
        std::fprintf(file, "%s=%d\n", Effects[e].key, int(v.effects[e].load()));
    }
    std::fprintf(file, "frame_gen=%s\nframe_gen_multiplier=%d\nframe_gen_fit_display=%d\n",
                 FrameGenName(v.frame_gen), v.frame_gen_multiplier.load(),
                 int(v.frame_gen_fit_display.load()));
    std::fprintf(file, "model_lod=%d\noutput_res=%dx%d\n", v.model_lod.load(),
                 OutputWidths[v.output_res], OutputHeights[v.output_res]);
    // Read by run.sh at start.
    std::fprintf(file, "live_resolution=%s\n", v.live_resolution < 0 ? "auto"
                                                  : v.live_resolution ? "1" : "0");
    for (const std::string& line : kept) {
        std::fprintf(file, "%s\n", line.c_str());
    }
    std::fclose(file);
}

void RewriteKeys(const std::vector<std::pair<std::string, std::string>>& keys, const char* drop_prefix) {
    std::vector<std::string> lines;
    std::vector<bool> written(keys.size());
    if (FILE* file = std::fopen(Path(), "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), file)) {
            std::string text{line};
            text.erase(text.find_last_not_of("\r\n") + 1);
            std::string name = text.substr(0, text.find('='));
            name.erase(name.find_last_not_of(" \t") + 1);
            size_t k = 0;
            while (k < keys.size() && keys[k].first != name) ++k;
            if (k < keys.size()) {
                if (written[k]) continue;
                text = keys[k].first + "=" + keys[k].second;
                written[k] = true;
            } else if (drop_prefix && name.rfind(drop_prefix, 0) == 0) {
                continue;
            }
            lines.push_back(text);
        }
        std::fclose(file);
    }
    for (size_t k = 0; k < keys.size(); ++k)
        if (!written[k]) lines.push_back(keys[k].first + "=" + keys[k].second);
    if (FILE* file = std::fopen(Path(), "w")) {
        for (const std::string& line : lines) std::fprintf(file, "%s\n", line.c_str());
        std::fclose(file);
    }
}

void NoteDeviceLost() {
    auto& v = Get();
    if (!v.fsr4_running || !IsFsr4(v.upscaler)) return;
    std::printf("Upscaler: the GPU was lost while %s ran; the next start uses FSR 3.1\n",
                UpscalerName(v.upscaler));
    std::fflush(stdout);
    v.fsr4_device_lost = true;
    device_lost_now = true;
    Save();
    device_lost_now = false;
}

// The in-game PC menu (src/pc_menu.c) reads and writes the same settings through these.
namespace {
// A setting as the in-game PC menu sees it: on/off as 0/1, the upscaler as its index, the
// output resolution as an index into OutputWidths, live resolution as -1 (auto), 0 or 1, the
// model detail as -2..2. False for a key the menu does not handle.
bool Read(const Values& v, const std::string& k, int& out) {
    if (k == "show_fps") out = v.show_fps.load();
    else if (k == "fullscreen") out = v.fullscreen.load();
    else if (k == "sharpen") out = v.sharpen.load();
    else if (k == "jitter") out = v.jitter.load();
    else if (k == "object_motion") out = v.object_motion.load();
    else if (k == "upscaler") out = v.upscaler.load();
    else if (k == "preset") out = v.preset.load();
    else if (k == "output_res") out = v.output_res.load();
    else if (k == "live_resolution") out = v.live_resolution.load();
    else if (k == "model_lod") out = v.model_lod.load();
    else if (k == "mouse_camera") out = v.mouse_camera.load();
    else if (k == "mouse_sensitivity") out = v.mouse_sensitivity.load();
    else if (k == "mouse_invert_y") out = v.mouse_invert_y.load();
    else if (k == "mouse_flick_threshold") out = v.mouse_flick_threshold.load();
    else if (k == "mouse_menu") out = v.mouse_menu.load();
    else if (k == "mouse_hide_cursor") out = v.mouse_hide_cursor.load();
    else {
        for (int e = 0; e < EffectCount; ++e) {
            if (k == Effects[e].key) {
                out = v.effects[e].load();
                return true;
            }
        }
        return false;
    }
    return true;
}
} // namespace

bool Online() {
    static const bool online = [] {
        const char* net = std::getenv("BB_ONLINE");
        const char* coop = std::getenv("BB_COOP");
        return net && !std::strcmp(net, "1") && !(coop && !std::strcmp(coop, "1"));
    }();
    return online;
}

bool OnlineBlocked(int effect) {
    const char* key = Effects[effect].key;
    return Online() && (!std::strncmp(key, "cheat_", 6) || !std::strncmp(key, "debug_", 6));
}

// The in-game PC menu (src/pc_menu.c) reads and writes the same settings through these. Online,
// the blocked switches read as off (the live stamina cheats among them); bbport.ini keeps them.
extern "C" int bbgpu_setting_get(const char* key, int* value) {
    for (int e = 0; e < EffectCount; ++e) {
        if (OnlineBlocked(e) && !std::strcmp(key, Effects[e].key)) {
            *value = 0;
            return 1;
        }
    }
    return Read(Get(), key, *value) ? 1 : 0;
}

extern "C" int bbgpu_setting_default(const char* key, int* value) {
    const std::string k = key;
    for (int e = 0; e < EffectCount; ++e) {
        if (k == Effects[e].key) {
            *value = Effects[e].default_on;
            return 1;
        }
    }
    static const Values defaults;
    return Read(defaults, k, *value) ? 1 : 0;
}

namespace {
// Fullscreen belongs to the launcher (settings.json, passed as BB_FULLSCREEN): the window
// follows the setting at once (window.cpp), a restart inherits the environment, and the
// launcher's file keeps it for the next start.
void SetFullscreen(bool on) {
    Get().fullscreen = on;
#ifdef _WIN32
    _putenv_s("BB_FULLSCREEN", on ? "1" : "0");
#else
    setenv("BB_FULLSCREEN", on ? "1" : "0", 1);
#endif
    const char* appdata = std::getenv("APPDATA");
    if (!appdata) return;
    const std::filesystem::path file = std::filesystem::path(appdata) / "bbport-launcher" / "settings.json";
    std::ifstream in(file);
    if (!in) return;
    nlohmann::json root = nlohmann::json::parse(in, nullptr, false);
    in.close();
    if (!root.is_object()) return;
    root["fullscreen"] = on;
    const auto part = file.string() + ".part";
    {
        std::ofstream out(part, std::ios::trunc);
        out << root.dump(2);
        if (!out) return;
    }
    std::error_code error;
    std::filesystem::rename(part, file, error);
}
} // namespace

extern "C" void bbgpu_setting_set(const char* key, int value) {
    const std::string k = key;
    for (int e = 0; e < EffectCount; ++e)
        if (OnlineBlocked(e) && k == Effects[e].key) return; // stays as bbport.ini has it
    if (k == "fullscreen") {
        SetFullscreen(value != 0);
        return;
    }
    std::string text = std::to_string(value);
    if (k == "upscaler") {
        text = UpscalerName(value);
    } else if (k == "output_res") {
        const int r = std::clamp(value, 0, OutputCount - 1);
        text = std::to_string(OutputWidths[r]) + "x" + std::to_string(OutputHeights[r]);
    } else if (k == "live_resolution" && value < 0) {
        text = "auto";
    }
    Set(Get(), k, text);
    Save();
}

// Whether this GPU runs an upscaler (the menu offers only those).
extern "C" int bbgpu_upscaler_supported(int upscaler) {
    const auto& v = Get();
    switch (upscaler) {
    case UpscalerFsr4: return v.fsr4_supported.load();
    case UpscalerFsr411: return v.fsr411_supported.load();
    case UpscalerDlss: return v.dlss_supported.load();
    default: return upscaler >= 0 && upscaler < UpscalerCount;
    }
}

float PresetScale(int preset) {
    static constexpr float scales[PresetCount] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
    return scales[std::clamp(preset, 0, PresetCount - 1)];
}

const char* PresetName(int preset) {
    static constexpr const char* names[PresetCount] = {"Native AA", "Quality", "Balanced",
                                                       "Performance", "Ultra Performance"};
    return names[std::clamp(preset, 0, PresetCount - 1)];
}

const char* FrameGenName(int frame_gen) {
    static constexpr const char* names[FrameGenCount] = {"off", "fsr3", "dlss"};
    return names[std::clamp(frame_gen, 0, FrameGenCount - 1)];
}

const char* UpscalerName(int upscaler) {
    static constexpr const char* names[UpscalerCount] = {"off", "fsr3", "fsr4", "fsr411", "taa", "dlss"};
    return names[std::clamp(upscaler, 0, UpscalerCount - 1)];
}

} // namespace BbSettings
