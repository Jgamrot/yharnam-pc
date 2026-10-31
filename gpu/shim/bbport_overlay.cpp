// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_overlay.h"
#include "bbport_map.h"
#include "bbport_map_art.h"
#include "bbport_atlas_menu.h"
#include "bbport_atlas_paths.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <cfloat>
#include <chrono>
#include <cctype>
#include <string>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>

#include <SDL3/SDL.h>
#include "bbport_settings.h"
#include "bbport_trophies.h"
#include "bbport_trophy_toast.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

// DejaVu Sans (Cyrillic), embedded (third_party/fonts, Bitstream Vera license). The offscreen
// preview (tools/overlay_preview.cpp) includes this file and uses the library's copy.
#ifdef BB_OVERLAY_PREVIEW
#elif defined(_WIN32)
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden bb_font_ttf\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".hidden bb_font_ttf_end\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char bb_font_ttf[];
extern "C" const unsigned char bb_font_ttf_end[];

extern "C" void runtime_restart(void); // bb-probe (probe.c)

namespace BbOverlay {

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
std::atomic<bool> initialized{false};
std::atomic<bool> menu_open{false};
std::atomic<std::int64_t> menu_opened_ms{0};
bool l3_down = false, r3_down = false;
int active_tab = 0; // Hunt, Trophy gallery, Image tools, Gameplay extras
// Where the controller's focus was last frame: on a rail icon, or on the page beside it.
bool rail_focused_last = false, page_focused_last = false;
bool dirty = false; // settings changed while open: saved on close
bool mod_selection_changed = false;
std::string mod_selection_error;
// The game's text dialog (SetTextEntry), guarded by imgui_mutex. The flag is atomic:
// RoutePad reads it on the game's pad thread without that lock.
std::atomic<bool> text_entry_active{false};
std::string text_entry_prompt, text_entry_text;
float base_scale = 1.0f;
ImFont* guide_serif_font = nullptr; // Georgia from Windows when present; embedded font remains the fallback.

// Present rate for the FPS counter.
std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f, last_present_ms = 0.0f;

// Trophy events can arrive on the game thread while the present thread draws the overlay.
std::mutex trophy_mutex;
std::array<unsigned char, 128> trophies{};
std::unordered_map<int, std::unique_ptr<ImTextureData>> trophy_icons; // present thread only
// Icons decoded ahead of their toast (the notifications' worker thread), taken by IconRef. Never
// destroyed: the worker may still be decoding while the process exits.
struct DecodedIcon {
    std::vector<unsigned char> pixels;
    int width = 0, height = 0;
};
std::mutex& decoded_mutex = *new std::mutex;
auto& decoded_icons = *new std::unordered_map<int, DecodedIcon>;

std::vector<unsigned char> DecodePng(const std::filesystem::path& path, int& width, int& height) {
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(path, ec);
    if (ec || bytes == 0 || bytes > 2 * 1024 * 1024) return {};
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::vector<unsigned char> input(bytes);
    if (!file.read(reinterpret_cast<char*>(input.data()), std::streamsize(input.size()))) return {};
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_PNG);
    if (!codec) return {};
    AVCodecContext* context = avcodec_alloc_context3(codec);
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    std::vector<unsigned char> pixels;
    if (context && packet && frame && avcodec_open2(context, codec, nullptr) >= 0 &&
        av_new_packet(packet, int(input.size())) >= 0) {
        std::memcpy(packet->data, input.data(), input.size());
        if (avcodec_send_packet(context, packet) >= 0 &&
            avcodec_receive_frame(context, frame) >= 0 &&
            frame->width > 0 && frame->height > 0 && frame->width <= 1024 &&
            frame->height <= 1024) {
            width = frame->width;
            height = frame->height;
            pixels.resize(std::size_t(width) * height * 4);
            SwsContext* converter = sws_getContext(width, height,
                static_cast<AVPixelFormat>(frame->format), width, height, AV_PIX_FMT_RGBA,
                SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (converter) {
                std::uint8_t* out[] = {pixels.data()};
                const int stride[] = {width * 4};
                if (sws_scale(converter, frame->data, frame->linesize, 0, height, out, stride) != height)
                    pixels.clear();
                sws_freeContext(converter);
            } else pixels.clear();
        }
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&context);
    return pixels;
}

// Any thread (the notifications' worker, before the toast): the PNG decoded for IconRef, so the
// present thread only uploads it.
void PrefetchIcon(int id) {
    {
        std::scoped_lock lock{decoded_mutex};
        if (decoded_icons.count(id)) return;
    }
    const auto entry = BbTrophies::Find(id);
    DecodedIcon icon;
    if (!entry.icon.empty()) icon.pixels = DecodePng(entry.icon, icon.width, icon.height);
    std::scoped_lock lock{decoded_mutex};
    decoded_icons.emplace(id, std::move(icon));
}

ImTextureRef IconRef(int id) {
    auto it = trophy_icons.find(id);
    if (it != trophy_icons.end()) {
        std::scoped_lock lock{decoded_mutex}; // decoded again for a toast: not needed after all
        decoded_icons.erase(id);
        return it->second ? it->second->GetTexRef() : ImTextureRef{};
    }
    int width = 0, height = 0;
    std::vector<unsigned char> pixels;
    bool prefetched = false;
    {
        std::scoped_lock lock{decoded_mutex};
        if (auto found = decoded_icons.find(id); found != decoded_icons.end()) {
            pixels = std::move(found->second.pixels);
            width = found->second.width;
            height = found->second.height;
            decoded_icons.erase(found);
            prefetched = true;
        }
    }
    if (!prefetched) {
        const auto entry = BbTrophies::Find(id);
        if (!entry.icon.empty()) pixels = DecodePng(entry.icon, width, height);
    }
    if (pixels.empty()) {
        trophy_icons.emplace(id, nullptr);
        return {};
    }
    auto texture = std::make_unique<ImTextureData>();
    texture->Create(ImTextureFormat_RGBA32, width, height);
    std::memcpy(texture->Pixels, pixels.data(), pixels.size());
    texture->UseColors = true;
    ImGui::RegisterUserTexture(texture.get());
    const auto ref = texture->GetTexRef();
    trophy_icons.emplace(id, std::move(texture));
    return ref;
}

ImU32 GradeColor(BbTrophies::Grade grade, float alpha = 1.0f) {
    const ImVec4 color = grade == BbTrophies::Grade::Bronze ? ImVec4(.77f, .45f, .24f, alpha) :
                         grade == BbTrophies::Grade::Silver ? ImVec4(.74f, .79f, .84f, alpha) :
                         grade == BbTrophies::Grade::Gold ? ImVec4(1.0f, .78f, .27f, alpha) :
                         grade == BbTrophies::Grade::Platinum ? ImVec4(.66f, .83f, 1.0f, alpha) :
                         ImVec4(.35f, .78f, .42f, alpha);
    return ImGui::ColorConvertFloat4ToU32(color);
}

void TrophyGlyph(ImDrawList* draw, ImVec2 center, float size, float alpha) {
    const ImU32 white = IM_COL32(255, 255, 255, int(255 * alpha));
    const float x = center.x, y = center.y, s = size;
    const ImVec2 bowl[] = {{x - .30f*s, y - .24f*s}, {x + .30f*s, y - .24f*s},
                           {x + .22f*s, y + .13f*s}, {x, y + .25f*s},
                           {x - .22f*s, y + .13f*s}};
    draw->AddConvexPolyFilled(bowl, 5, white);
    draw->AddLine({x - .29f*s, y - .15f*s}, {x - .43f*s, y - .15f*s}, white, .065f*s);
    draw->AddLine({x - .43f*s, y - .15f*s}, {x - .35f*s, y + .09f*s}, white, .065f*s);
    draw->AddLine({x + .29f*s, y - .15f*s}, {x + .43f*s, y - .15f*s}, white, .065f*s);
    draw->AddLine({x + .43f*s, y - .15f*s}, {x + .35f*s, y + .09f*s}, white, .065f*s);
    draw->AddRectFilled({x - .055f*s, y + .23f*s}, {x + .055f*s, y + .42f*s}, white);
    draw->AddRectFilled({x - .25f*s, y + .39f*s}, {x + .25f*s, y + .46f*s}, white,
                        .035f*s);
}

int64_t ClockMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

void SetOpen(bool value) {
    if (menu_open.exchange(value) == value) {
        return;
    }
    ImGui::GetIO().MouseDrawCursor = value;
    if (value) {
        active_tab = 0;
        menu_opened_ms = ClockMs();
        // The pointer shows at once where the mouse is, not only after it next moves.
        if (SDL_Window* window = SDL_GetMouseFocus()) {
            float x = 0.0f, y = 0.0f;
            SDL_GetMouseState(&x, &y);
            const float density = SDL_GetWindowPixelDensity(window);
            const float d = density > 0.0f ? density : 1.0f;
            ImGui::GetIO().AddMousePosEvent(x * d, y * d);
        }
    }
    else for (const auto key : {ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight,
                                ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown})
        ImGui::GetIO().AddKeyAnalogEvent(key, false, 0.0f);
    if (!value && dirty) {
        dirty = false;
        BbSettings::Save();
    }
}

ImGuiKey KeyFromSdl(SDL_Keycode key) {
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

// Marks the settings dirty when a widget changed them.
template <typename T>
void Store(std::atomic<T>& target, T value, bool changed) {
    if (changed) {
        target = value;
        dirty = true;
    }
}

// The guide's look, shared with the launcher (launcher/qml/Theme.qml): near-black panels, warm
// off-white text, one crimson accent. Colours go through GetColorU32 so disabled rows fade.
namespace Ink {
constexpr ImU32 Text = IM_COL32(236, 231, 223, 255);
constexpr ImU32 Dim = IM_COL32(163, 158, 151, 255);
constexpr ImU32 Faint = IM_COL32(111, 107, 102, 255);
constexpr ImU32 Line = IM_COL32(38, 43, 49, 255);
constexpr ImU32 LineStrong = IM_COL32(54, 60, 67, 255);
constexpr ImU32 Panel = IM_COL32(11, 13, 16, 255);
constexpr ImU32 Rail = IM_COL32(9, 11, 14, 255);
constexpr ImU32 Surface = IM_COL32(20, 24, 29, 255);
constexpr ImU32 SurfaceHover = IM_COL32(26, 31, 37, 255);
constexpr ImU32 Accent = IM_COL32(200, 50, 63, 255);
constexpr ImU32 AccentSoft = IM_COL32(42, 18, 22, 255);
constexpr ImU32 Good = IM_COL32(143, 197, 156, 255);
constexpr ImU32 Warn = IM_COL32(227, 168, 120, 255);
} // namespace Ink
ImVec4 Vec(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }
float Px(float v) { return v * base_scale; }
// A one-pixel rule (filled, so it stays solid at any scale and on any renderer).
void Rule(ImDrawList* draw, float x0, float x1, float y, ImU32 color = Ink::Line) {
    draw->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + std::max(1.0f, std::floor(base_scale))), color);
}

// Settings are rows of a two-column table: label left, control right, a hairline between rows. A
// section header or a note closes the table; the next row opens a new one.
bool rows_open = false;
int rows_id = 0;
ImVec2 last_label_min{}, last_label_max{};
float RowHeight() { return Px(54.0f); }
float ControlWidth() { return std::min(Px(300.0f), ImGui::GetContentRegionAvail().x * 0.46f); }
void CloseRows() {
    if (!rows_open) return;
    ImGui::EndTable();
    rows_open = false;
}
void OpenRows() {
    if (rows_open) return;
    // A name per table, not PushID: BeginTable keeps its own ID pushed until EndTable.
    char id[24];
    std::snprintf(id, sizeof(id), "##rows%d", ++rows_id);
    const bool open = ImGui::BeginTable(id, 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_NoSavedSettings);
    if (!open) return;
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthFixed, ControlWidth());
    rows_open = true;
}
// Starts a row: its label, vertically centred, then the control column, ready for a control of
// the given height.
void RowLabel(const char* label, float control_height) {
    OpenRows();
    const float h = RowHeight();
    ImGui::TableNextRow(ImGuiTableRowFlags_None, h);
    ImGui::TableSetColumnIndex(0);
    const float y = ImGui::GetCursorPosY();
    ImGui::SetCursorPosY(y + (h - ImGui::GetTextLineHeight()) * 0.5f - ImGui::GetStyle().CellPadding.y);
    ImGui::PushStyleColor(ImGuiCol_Text, Vec(Ink::Text));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    last_label_min = ImGui::GetItemRectMin();
    last_label_max = ImGui::GetItemRectMax();
    ImGui::TableSetColumnIndex(1);
    ImGui::SetCursorPosY(y + (h - control_height) * 0.5f - ImGui::GetStyle().CellPadding.y);
}
// The section's small-caps title, with space above it.
void Section(const char* title) {
    CloseRows();
    ImGui::Dummy(ImVec2(0, Px(18.0f)));
    ImGui::PushStyleColor(ImGuiCol_Text, Vec(Ink::Dim));
    std::string upper(title);
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    ImGui::TextUnformatted(upper.c_str());
    ImGui::PopStyleColor();
    const ImVec2 min = ImGui::GetItemRectMin();
    Rule(ImGui::GetWindowDrawList(), min.x, min.x + ImGui::GetContentRegionAvail().x, ImGui::GetItemRectMax().y + Px(8.0f));
    ImGui::Dummy(ImVec2(0, Px(6.0f)));
}
// A page's serif title and its rule.
void PageTitle(const char* title) {
    CloseRows();
    ImGui::PushFont(guide_serif_font, 39.0f);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, Px(6.0f)));
    const ImVec2 at = ImGui::GetCursorScreenPos();
    Rule(ImGui::GetWindowDrawList(), at.x, at.x + ImGui::GetContentRegionAvail().x, at.y);
    ImGui::Dummy(ImVec2(0, Px(10.0f)));
}
// Explanatory text outside the rows (dim), or a warning (amber).
void Note(const char* text, ImU32 color = Ink::Dim) {
    CloseRows();
    ImGui::PushStyleColor(ImGuiCol_Text, Vec(color));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}
void NoteF(ImU32 color, const char* format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    Note(text, color);
}
// A row that only reports something: label and value.
void InfoRow(const char* label, const char* format, ...) {
    char value[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(value, sizeof(value), format, args);
    va_end(args);
    RowLabel(label, ImGui::GetTextLineHeight());
    ImGui::PushStyleColor(ImGuiCol_Text, Vec(Ink::Dim));
    ImGui::TextUnformatted(value);
    ImGui::PopStyleColor();
}

// A switch: crimson when on. The whole switch is the (navigable) button.
bool Switch(const char* id, bool* value) {
    const float w = Px(42.0f), h = Px(24.0f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - w);
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    if (pressed) *value = !*value;
    const bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, max, ImGui::GetColorU32(*value ? Ink::Accent : hot ? IM_COL32(42, 48, 55, 255) : IM_COL32(34, 39, 45, 255)), h * 0.5f);
    if (!*value) draw->AddRect(min, max, ImGui::GetColorU32(hot ? Ink::Dim : Ink::LineStrong), h * 0.5f, 0, Px(1.0f));
    const float r = h * 0.5f - Px(4.0f);
    draw->AddCircleFilled(ImVec2(*value ? max.x - h * 0.5f : min.x + h * 0.5f, (min.y + max.y) * 0.5f), r,
                          ImGui::GetColorU32(*value ? IM_COL32(255, 246, 242, 255) : Ink::Dim));
    return pressed;
}

void Checkbox(const char* label, std::atomic<bool>& value) {
    RowLabel(label, Px(24.0f));
    bool v = value;
    ImGui::PushID(label);
    const bool changed = Switch("##switch", &v);
    ImGui::PopID();
    Store(value, v, changed);
}
bool Toggle(const char* label, bool* value) {
    RowLabel(label, Px(24.0f));
    ImGui::PushID(label);
    const bool changed = Switch("##switch", value);
    ImGui::PopID();
    return changed;
}

void Slider(const char* label, std::atomic<float>& value, float lo, float hi) {
    RowLabel(label, ImGui::GetFrameHeight());
    float v = value;
    ImGui::SetNextItemWidth(-FLT_MIN);
    const std::string id = std::string("##slider_") + label;
    const bool changed = ImGui::SliderFloat(id.c_str(), &v, lo, hi, "%.2f");
    Store(value, v, changed);
}

// A pick list in a row: as ImGui::BeginCombo; EndCombo when it returns true.
bool ComboRow(const char* label, const char* preview) {
    RowLabel(label, ImGui::GetFrameHeight());
    ImGui::SetNextItemWidth(-FLT_MIN);
    // An ID string, not PushID: an open combo continues in its popup window. The arrow is the
    // launcher's thin chevron, drawn on the row's window after the frame.
    const std::string id = std::string("##combo_") + label;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetFrameHeight();
    const bool open = ImGui::BeginCombo(id.c_str(), preview, ImGuiComboFlags_NoArrowButton);
    const ImVec2 c(at.x + w - Px(18.0f), at.y + h * 0.5f);
    const float k = Px(4.5f);
    draw->PathLineTo(ImVec2(c.x - k, c.y - k * 0.5f));
    draw->PathLineTo(ImVec2(c.x, c.y + k * 0.5f));
    draw->PathLineTo(ImVec2(c.x + k, c.y - k * 0.5f));
    draw->PathStroke(ImGui::GetColorU32(open ? Ink::Text : Ink::Dim), ImDrawFlags_None, Px(1.6f));
    return open;
}

// One of several choices: a row with a radio mark on the right.
bool ChoiceRow(const char* label, bool selected) {
    RowLabel(label, Px(22.0f));
    const float d = Px(22.0f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - d);
    ImGui::PushID(label);
    const bool pressed = ImGui::InvisibleButton("##choice", ImVec2(d, d), ImGuiButtonFlags_EnableNav);
    ImGui::PopID();
    const bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 c(min.x + d * 0.5f, min.y + d * 0.5f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddCircle(c, d * 0.5f - Px(1.0f), ImGui::GetColorU32(selected ? Ink::Accent : hot ? Ink::Dim : Ink::LineStrong), 0, Px(1.6f));
    if (selected) draw->AddCircleFilled(c, d * 0.25f, ImGui::GetColorU32(Ink::Accent));
    return pressed;
}

// The one primary action of a page (crimson, full width).
bool PrimaryButton(const char* label) {
    CloseRows();
    ImGui::Dummy(ImVec2(0, Px(6.0f)));
    ImGui::PushStyleColor(ImGuiCol_Button, Vec(Ink::Accent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Vec(IM_COL32(217, 65, 78, 255)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Vec(IM_COL32(169, 42, 53, 255)));
    const bool pressed = ImGui::Button(label, ImVec2(-FLT_MIN, Px(48.0f)));
    ImGui::PopStyleColor(3);
    return pressed;
}

// What the last row does: a small marker after its label, the text on hover or focus.
void Hint(const char* text) {
    const bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    const ImVec2 c(last_label_max.x + Px(13.0f), (last_label_min.y + last_label_max.y) * 0.5f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 ink = ImGui::GetColorU32(hot ? Ink::Text : Ink::Dim);
    draw->AddCircle(c, Px(7.5f), ink, 0, Px(1.2f));
    draw->AddRectFilled(ImVec2(c.x - Px(0.75f), c.y - Px(1.0f)), ImVec2(c.x + Px(0.75f), c.y + Px(4.0f)), ink); // an "i"
    draw->AddCircleFilled(ImVec2(c.x, c.y - Px(3.4f)), Px(1.0f), ink);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool over_marker = std::abs(mouse.x - c.x) < Px(10.0f) && std::abs(mouse.y - c.y) < Px(10.0f);
    if (hot || over_marker) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

bool IsCheat(int e) { return !std::strncmp(BbSettings::Effects[e].key, "cheat_", 6); }
// The stamina cheats are set each frame (src/runtime_stamina.c), not patched at start.
bool LiveSetting(int e) { return std::strstr(BbSettings::Effects[e].key, "stamina") != nullptr; }

bool RestartNeeded() {
    const auto& s = BbSettings::Get();
    bool restart = s.object_motion != s.startup_object_motion ||
                   s.model_lod != s.startup_model_lod ||
                   s.live_resolution != s.startup_live_resolution ||
                   BbSettings::ResolutionNeedsRestart();
    for (int e = 0; e < BbSettings::EffectCount; ++e)
        restart |= !LiveSetting(e) && s.effects[e] != s.startup_effects[e];
    return restart;
}

/// The presented frame rate; with frame generation adding frames also the game's own rate,
/// which is what the GPU's load (resolution, upscaler) changes. The total follows the game's
/// FPS cap times the multiplier while the GPU keeps up.
std::string FpsText() {
    const auto& s = BbSettings::Get();
    const float shown = frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f;
    char text[96];
    const int generated = s.frame_gen != BbSettings::FrameGenOff ? s.frame_gen_now.load() : 0;
    if (generated > 0 && s.game_fps > 0.0f) {
        std::snprintf(text, sizeof(text), "%.0f FPS (game %.0f, frame gen %dx)", shown,
                      s.game_fps.load(), int(shown / s.game_fps + 0.5f));
    } else {
        std::snprintf(text, sizeof(text), "%.0f FPS  %.1f ms", shown, frame_ms_avg);
    }
    return text;
}


bool NativePcMenuAvailable() {
    const char* assets = std::getenv("BB_PC_MENU_ASSETS");
    const char* enabled = std::getenv("BB_PC_MENU");
    return assets && std::strcmp(assets, "1") == 0 &&
           (!enabled || std::strcmp(enabled, "0") != 0);
}

bool ExtrasRestartNeeded() {
    const auto& s = BbSettings::Get();
    for (int e = 0; e < BbSettings::EffectCount; ++e) // Cheats: native PC menu has no room for this section.
        if (IsCheat(e) && !LiveSetting(e) && s.effects[e] != s.startup_effects[e]) return true;
    for (int e = BbSettings::FirstEnhancement; e < BbSettings::EffectCount; ++e)
        if (s.effects[e] != s.startup_effects[e]) return true;
    return false;
}

void RestartPrompt() {
    if (!RestartNeeded()) return;
    Note("Changes apply after restarting the game.", Ink::Warn);
    if (PrimaryButton("Apply and restart")) {
        BbSettings::Save();
        runtime_restart();
    }
}

void SettingsTab() {
    auto& s = BbSettings::Get();
    PageTitle("Visual settings");
    Section("Temporal upscaling");
    static const char* upscalers[] = {"Off", "FSR 3.1", "FSR 4 (INT8)", "FSR 4.1.1 (INT8)",
                                     "TAA (native anti-aliasing)", "DLSS (NVIDIA RTX)"};
    static const char* later[] = {"XeSS"};
    int upscaler = s.upscaler;
    if (ComboRow("Upscaler", upscalers[upscaler])) {
        for (int i = 0; i < BbSettings::UpscalerCount; ++i) {
            const bool supported = i == BbSettings::UpscalerFsr4 ? s.fsr4_supported.load()
                : i == BbSettings::UpscalerFsr411 ? s.fsr411_supported.load()
                : i == BbSettings::UpscalerDlss   ? s.dlss_supported.load() : true;
            ImGui::BeginDisabled(!supported);
            if (ImGui::Selectable(upscalers[i], i == upscaler)) {
                Store(s.upscaler, i, true);
            }
            ImGui::EndDisabled();
            if (!supported) {
                ImGui::SameLine();
                ImGui::TextDisabled("- unsupported by this GPU");
            }
        }
        for (const char* name : later) {
            ImGui::BeginDisabled();
            ImGui::Selectable(name, false);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("- in development");
        }
        ImGui::EndCombo();
    }
    if (const char* problem = s.dlss_problem.load(); problem && upscaler == BbSettings::UpscalerDlss) {
        NoteF(Ink::Warn, "DLSS: %s", problem);
    }
    if (const char* problem = s.fsr4_problem.load()) {
        NoteF(Ink::Warn, "FSR 4 unavailable: %s", problem);
        if (!BbSettings::IsFsr4(s.upscaler))
            Note("The selected mode is active. You can select FSR 4 again.");
    }
    if (BbSettings::IsFsr4(s.upscaler)) {
        if (s.upscaler == BbSettings::UpscalerFsr411) {
            Hint("FSR 4.1.1 INT8: AMD 4.1.1 DLL model reproduced in Vulkan. One model covers Native through Performance, with another for Ultra Performance. Assets: tools/fsr4cap/build_assets.sh (requires the DLL and Proton).");
        } else {
            Hint("FSR 4 INT8 uses the v07 model from the AMD FidelityFX SDK. Higher quality than FSR 3.1, with more GPU work. Changing presets rebuilds the model and may pause briefly. Assets: tools/fetch_fsr4_assets.sh.");
        }
        Checkbox("FSR 4: auto exposure", s.fsr4_auto_exposure);
        Checkbox("FSR 4: invert jitter", s.fsr4_invert_jitter);
        Hint("Ghosting diagnostics: FSR 4 uses exposure to normalize color and decide when to discard history. These options apply immediately.");
    }
    const bool upscaler_on = s.upscaler != BbSettings::UpscalerOff;
    const bool taa = s.upscaler == BbSettings::UpscalerTaa;
    ImGui::BeginDisabled(!upscaler_on);
    ImGui::BeginDisabled(taa);
    int preset = taa ? BbSettings::NativeAA : s.preset.load();
    char preset_label[64];
    std::snprintf(preset_label, sizeof(preset_label), "%s (x%.1f)", BbSettings::PresetName(preset),
                  BbSettings::PresetScale(preset));
    if (ComboRow("Preset", preset_label)) {
        for (int i = 0; i < BbSettings::PresetCount; ++i) {
            char label[64];
            const float scale = BbSettings::PresetScale(i);
            const int output = s.output_res;
            std::snprintf(label, sizeof(label), "%s (x%.1f, render %dx%d)",
                          BbSettings::PresetName(i), scale,
                          int(std::lround(BbSettings::OutputWidths[output] / scale / 2) * 2),
                          int(std::lround(BbSettings::OutputHeights[output] / scale / 2) * 2));
            if (ImGui::Selectable(label, i == preset)) {
                Store(s.preset, i, true);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (taa) {
        Note("TAA smooths the scene at output resolution without FSR upscaling. The saved upscaling preset returns when you select an upscaler.");
    }
    InfoRow("Active scene resolution", "%d x %d", s.active_render_width.load(),
            s.active_render_height.load());
    if (BbSettings::AspectNeedsRestart()) {
        Hint("Changing aspect ratio requires a restart. Ultrawide outputs (21:9, 32:9) widen the camera and HUD (HUD position: in the launcher); other aspect ratios keep a centred 16:9 HUD.");
    } else if (BbSettings::FixedRenderSession()) {
        InfoRow("Startup preset", "%s", BbSettings::PresetName(s.startup_preset));
        if (const char* automatic = std::getenv("BB_AUTO_RENDER_RES");
            automatic && automatic[0] == '1') {
            Hint("At outputs other than 1080p, the scene resolution is patched at startup. Changes to the preset or output require a restart. This saves GPU work on older GPUs. Non-16:9 outputs always use this path to keep camera and HUD geometry synchronized.");
        } else {
            Hint("BB_RENDER_RES fixes the scene resolution at startup. Remove this explicit override to allow live resolution and preset changes. Changes to aspect ratio still require a restart.");
        }
    } else {
        Hint("Native AA uses the upscaler for anti-aliasing. Other presets reduce scene resolution relative to the output. The HUD is drawn at output resolution. Preset changes apply on the next frame in a live session.");
    }
    Checkbox("Sharpening (RCAS)", s.sharpen);
    ImGui::BeginDisabled(!s.sharpen);
    Slider("Sharpness", s.sharpness, 0.0f, 2.0f);
    Hint("Up to 1 uses the upscaler sharpening pass (RCAS). Values above 1 add another RCAS pass. Ctrl+click the slider to enter an exact value.");
    ImGui::EndDisabled();
    Checkbox("Subpixel jitter", s.jitter);
    Hint("The camera shifts by a fraction of a pixel each frame so the upscaler can recover detail across multiple frames. Without jitter, it only smooths using frame history.");

    Section("Reactive mask");
    ImGui::BeginDisabled(taa);
    Checkbox("Enable mask", s.reactive);
    Hint("Marks transparent effects such as particles and fog so the upscaler relies less on history. Reduces trails, but can increase shimmer behind those effects.");
    ImGui::BeginDisabled(!s.reactive);
    Slider("Scale", s.reactive_scale, 0.0f, 4.0f);
    Slider("Threshold", s.reactive_threshold, 0.0f, 1.0f);
    Slider("Maximum", s.reactive_max, 0.0f, 1.0f);
    bool show_mask = s.debug_view == BbSettings::DebugReactive;
    if (Toggle("Show mask (debug)", &show_mask)) {
        s.debug_view = show_mask ? BbSettings::DebugReactive : BbSettings::DebugNone;
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    Checkbox("Object motion vectors", s.object_motion);
    Hint("Accurate vectors for animated objects reduce breakup on clothing and weapons during movement. Static geometry receives no extra pass. Changes require a restart.");
    bool show_motion = s.debug_view == BbSettings::DebugMotion;
    if (Toggle("Show motion vectors (debug)", &show_motion)) {
        s.debug_view = show_motion ? BbSettings::DebugMotion : BbSettings::DebugNone;
    }
    Hint("Red/green: horizontal/vertical motion (8 pixels = full brightness). Blue: the pixel has accurate object motion, beyond camera motion. A moving object with no blue or red/green is treated as stationary and may leave trails.");
    ImGui::EndDisabled(); // upscaler off

    Section("Frame generation");
    static const char* frame_gens[] = {"Off", "FSR 3.1 (any GPU)", "DLSS (NVIDIA RTX 40/50)"};
    const int frame_gen = s.frame_gen;
    if (ComboRow("Frame generation", frame_gens[frame_gen])) {
        for (int i = 0; i < BbSettings::FrameGenCount; ++i) {
            const bool supported = i != BbSettings::FrameGenDlss || s.dlssg_max_multiplier > 1;
            ImGui::BeginDisabled(!supported);
            if (ImGui::Selectable(frame_gens[i], i == frame_gen)) {
                Store(s.frame_gen, i, true);
            }
            ImGui::EndDisabled();
            if (!supported) {
                ImGui::SameLine();
                ImGui::TextDisabled("- unsupported by this GPU");
            }
        }
        ImGui::EndCombo();
    }
    Hint("Generates frames between two of the game's frames: 60 FPS on screen becomes 120. Needs a display with that refresh rate (120 Hz or more at 60 FPS); it pauses when the display cannot show the extra frames, and 3x/4x drop to what it can. Adds a little input latency. Menus and movies are not generated.");
    const int max_multiplier =
        s.frame_gen == BbSettings::FrameGenDlss ? s.dlssg_max_multiplier.load() : 2;
    ImGui::BeginDisabled(s.frame_gen == BbSettings::FrameGenOff || max_multiplier <= 2);
    int multiplier = std::min(s.frame_gen_multiplier.load(), max_multiplier);
    if (ImGui::SliderInt("Multiplier", &multiplier, 2, std::max(max_multiplier, 2), "%dx")) {
        Store(s.frame_gen_multiplier, multiplier, true);
    }
    ImGui::EndDisabled();
    ImGui::BeginDisabled(s.frame_gen == BbSettings::FrameGenOff);
    Checkbox("Fit to display refresh", s.frame_gen_fit_display);
    ImGui::EndDisabled();
    Hint("On: no more frames than the display shows (an uncapped game gets fewer generated frames, and the GPU does less). Off: the multiplier as chosen; past the refresh rate the display shows the newest frames.");
    // What runs now (fitted: at most the frames the display shows between two of the game's).
    if (s.frame_gen != BbSettings::FrameGenOff && s.frame_gen_display_hz > 0) {
        const int now = std::min(s.frame_gen_now.load(), multiplier - 1);
        if (now > 0 && s.frame_gen_flip_limit > 0) {
            ImGui::TextDisabled("Now %dx on a %d Hz display; V-Sync (FIFO) caps the game at %d FPS",
                                now + 1, s.frame_gen_display_hz.load(),
                                s.frame_gen_flip_limit.load());
        } else if (now > 0) {
            ImGui::TextDisabled("Now %dx on a %d Hz display", now + 1,
                                s.frame_gen_display_hz.load());
        } else if (s.frame_gen_fit_display) {
            ImGui::TextDisabled("Paused: no room on a %d Hz display at this frame rate",
                                s.frame_gen_display_hz.load());
        } else {
            ImGui::TextDisabled("Paused: presenting fell behind (V-Sync on a slower display?)");
        }
    }
    if (const char* problem = s.frame_gen_problem.load()) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s", problem);
    }


    Section("Output resolution");
    auto output_label = [](int i) {
        return std::to_string(BbSettings::OutputWidths[i]) + " x " +
               std::to_string(BbSettings::OutputHeights[i]);
    };
    int output = s.output_res;
    if (ComboRow("Output resolution", output_label(output).c_str())) {
        for (int i = 0; i < BbSettings::OutputCount; ++i) {
            if (ImGui::Selectable(output_label(i).c_str(), i == output)) {
                Store(s.output_res, i, true);
            }
        }
        ImGui::EndCombo();
    }
    if (BbSettings::AspectNeedsRestart()) {
        Hint("Changing aspect ratio requires a restart. Ultrawide outputs (21:9, 32:9) widen the camera and HUD (HUD position: in the launcher); other aspect ratios keep a centred 16:9 HUD.");
    } else if (BbSettings::FixedRenderSession()) {
        Hint("Final image and HUD resolution. The preset controls scene resolution relative to the output. 4K Performance is approximately 1920x1080. Changes require a restart.");
    } else {
        Hint("Output and HUD resolution changes at the next frame boundary. The preset controls scene resolution relative to the output. Resizing resets upscaler history and may pause briefly.");
    }
    static const char* live_modes[] = {"Auto (based on GPU)", "Off (faster)", "On"};
    int live = s.live_resolution + 1;
    if (ComboRow("Live resolution changes", live_modes[live])) {
        for (int i = 0; i < 3; ++i) {
            if (ImGui::Selectable(live_modes[i], i == live)) {
                Store(s.live_resolution, i - 1, true);
            }
        }
        ImGui::EndCombo();
    }
    Hint("On: output and presets change without restarting, while game post-processing stays at 1080p. Off: the scene is patched to the preset resolution at startup. Auto enables the live path on capable discrete GPUs, except for outputs above 1080p with depth of field on (the live path blurs those scenes). Changes apply after restarting the game.");
    Section("Game effects (restart required)");
    static const char* lods[] = {"Highest (-2)", "Higher (-1)", "Original", "Lower (1)", "Lowest (2)"};
    static constexpr int lod_values[] = {-2, -1, 0, 1, 2};
    int lod_index = 2;
    for (int i = 0; i < 5; ++i) {
        if (lod_values[i] == s.model_lod) lod_index = i;
    }
    if (ComboRow("Model detail", lods[lod_index])) {
        for (int i = 0; i < 5; ++i) {
            if (ImGui::Selectable(lods[i], i == lod_index)) {
                Store(s.model_lod, lod_values[i], true);
            }
        }
        ImGui::EndCombo();
    }
    for (int e = 0; e < 8; ++e) {
        Checkbox(BbSettings::Effects[e].label, s.effects[e]);
    }
    Hint("Effects are applied through game patches at startup (patches/Bloodborne.xml). Motion blur and dynamic shadows can increase GPU load.");

    RestartPrompt();

    Section("Other");
    Checkbox("Show FPS counter", s.show_fps);
}

void ImageToolsTab() {
    auto& s = BbSettings::Get();
    PageTitle("Image tools");
    Note("Graphics, display, effects and the FPS counter are in Pause > System. These tools are "
         "diagnostics beyond the game menu.");
    // Hunter's Atlas: the map's keys and shortcuts, while the launcher has it on.
    if (BbMap::Enabled()) BbMap::Settings();
    Section("Current image");
    InfoRow("Scene resolution", "%d x %d", s.active_render_width.load(),
            s.active_render_height.load());
    if (BbSettings::FixedRenderSession())
        InfoRow("Startup preset", "%s (changes need a restart)",
                BbSettings::PresetName(s.startup_preset));
    if (const char* problem = s.dlss_problem.load();
        problem && s.upscaler == BbSettings::UpscalerDlss)
        NoteF(Ink::Warn, "DLSS: %s", problem);
    if (const char* problem = s.fsr4_problem.load())
        NoteF(Ink::Warn, "FSR 4 unavailable: %s", problem);

    Section("Fine tuning");
    ImGui::BeginDisabled(!s.sharpen);
    Slider("Sharpness", s.sharpness, 0.0f, 2.0f);
    ImGui::EndDisabled();
    Hint("Enable Sharpening in System > PC Graphics first. Values above 1 add another RCAS pass.");
    if (BbSettings::IsFsr4(s.upscaler)) {
        Checkbox("FSR 4: auto exposure", s.fsr4_auto_exposure);
        Checkbox("FSR 4: invert jitter", s.fsr4_invert_jitter);
        Hint("Provider checks for FSR 4. These changes apply immediately.");
    }
    const bool temporal = s.upscaler != BbSettings::UpscalerOff &&
                          s.upscaler != BbSettings::UpscalerTaa;
    ImGui::BeginDisabled(!temporal);
    Checkbox("Reactive mask", s.reactive);
    ImGui::BeginDisabled(!s.reactive);
    Slider("Mask scale", s.reactive_scale, 0.0f, 4.0f);
    Slider("Mask threshold", s.reactive_threshold, 0.0f, 1.0f);
    Slider("Mask maximum", s.reactive_max, 0.0f, 1.0f);
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    Hint("Marks particles and fog so the upscaler relies less on frame history.");

    Section("Debug views");
    int view = s.debug_view;
    if (ChoiceRow("Normal image", view == BbSettings::DebugNone))
        s.debug_view = BbSettings::DebugNone;
    if (ChoiceRow("Reactive mask view", view == BbSettings::DebugReactive))
        s.debug_view = BbSettings::DebugReactive;
    if (ChoiceRow("Motion vector view", view == BbSettings::DebugMotion))
        s.debug_view = BbSettings::DebugMotion;
    Hint("Debug views are temporary diagnostics; return to Normal image to play.");
}

std::filesystem::path ModConfigPath() {
    if (const char* value = std::getenv("BB_MODS_CONFIG"); value && *value) return value;
    return std::filesystem::current_path() / "mods.json";
}

std::optional<std::string> XboxPromptFolder() {
    const char* configured = std::getenv("BB_MODS_DIR");
    const auto root = configured && *configured ? std::filesystem::path(configured) :
                      std::filesystem::current_path() / "mods";
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return std::nullopt;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (!entry.is_directory(ec)) continue;
        auto name = entry.path().filename().string();
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        if (name.find("xbox") != std::string::npos && name.find("prompt") != std::string::npos)
            return entry.path().filename().string();
    }
    return std::nullopt;
}

nlohmann::json ModProfile() {
    std::ifstream file(ModConfigPath());
    if (!file) return nlohmann::json::object();
    try {
        auto profile = nlohmann::json::parse(file);
        return profile.is_object() ? profile : nlohmann::json::object();
    } catch (const nlohmann::json::exception&) {
        return nlohmann::json::object();
    }
}

bool PromptEnabled(const std::string& folder) {
    const auto profile = ModProfile();
    const auto it = profile.find("disabled");
    if (it == profile.end() || !it->is_array()) return true;
    return std::find(it->begin(), it->end(), folder) == it->end();
}

bool SetPromptEnabled(const std::string& folder, bool enabled) {
    nlohmann::json profile = nlohmann::json::object();
    std::ifstream existing(ModConfigPath());
    if (existing) {
        try {
            profile = nlohmann::json::parse(existing);
            if (!profile.is_object()) return false;
        } catch (const nlohmann::json::exception&) { return false; }
    }
    auto& disabled = profile["disabled"];
    if (!disabled.is_array()) disabled = nlohmann::json::array();
    nlohmann::json next = nlohmann::json::array();
    for (const auto& name : disabled)
        if (!name.is_string() || name.get<std::string>() != folder) next.push_back(name);
    if (!enabled) next.push_back(folder);
    profile["disabled"] = std::move(next);
    std::ofstream file(ModConfigPath(), std::ios::trunc);
    if (!file) return false;
    file << profile.dump(2) << '\n';
    return bool(file);
}

void ModsTab() {
    auto& s = BbSettings::Get();
    const bool native = NativePcMenuAvailable();
    PageTitle(native ? "Gameplay extras" : "Mods & patches");
    Note(native ?
        "Gameplay tweaks and PC settings are in Pause > System. Cheats and enhancements stay here because the game menu has no room for them." :
        "Choose button prompts and gameplay patches, then restart to apply them.");
    if (const char* enabled = std::getenv("BB_MODS_ENABLED"); enabled && std::strcmp(enabled,"0") == 0)
        Note("Mod loading is off in the launcher. Turn it on there first.", Ink::Warn);
    // 0 the game's PlayStation buttons, 1 the Xbox prompt mod, 2 the keyboard key images. The
    // launcher writes both into the player's own menu file the first time they are chosen there;
    // run.py reads the choice again at every start.
    Section("Button prompts");
    static const auto xbox_folder = XboxPromptFolder();
    static const std::optional<std::string> keyboard_folder = [] () -> std::optional<std::string> {
        const char* configured = std::getenv("BB_MODS_DIR");
        const auto root = configured && *configured ? std::filesystem::path(configured) :
                          std::filesystem::current_path() / "mods";
        std::error_code ec;
        for (const std::string name : {"Keyboard Prompts (Kenney)", "Keyboard Prompts (PCBButtons)"})
            if (std::filesystem::is_directory(root / name, ec)) return name;
        return std::nullopt;
    }();
    static int prompt_choice = -1;
    if (prompt_choice < 0)
        prompt_choice = keyboard_folder && PromptEnabled(*keyboard_folder) ? 2
                      : xbox_folder && PromptEnabled(*xbox_folder) ? 1 : 0;
    int choice = prompt_choice;
    if (ChoiceRow("PlayStation buttons", choice == 0)) choice = 0;
    ImGui::BeginDisabled(!xbox_folder);
    if (ChoiceRow("Xbox buttons", choice == 1)) choice = 1;
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!keyboard_folder);
    if (ChoiceRow("Keyboard keys", choice == 2)) choice = 2;
    ImGui::EndDisabled();
    if (choice != prompt_choice) {
        const bool saved = (!xbox_folder || SetPromptEnabled(*xbox_folder, choice == 1)) &&
                           (!keyboard_folder || SetPromptEnabled(*keyboard_folder, choice == 2));
        if (saved) {
            prompt_choice = choice;
            mod_selection_changed = true;
            mod_selection_error.clear();
        } else mod_selection_error = "Could not save mods.json. Check the port folder permissions.";
    }
    if (!xbox_folder)
        Note("Choose Xbox in the launcher's Controls page once to add the Xbox buttons.", Ink::Faint);
    if (!keyboard_folder)
        Note("Choose Keyboard in the launcher's Controls page once to add the keyboard keys.", Ink::Faint);
    if (!mod_selection_error.empty())
        Note(mod_selection_error.c_str(), Ink::Accent);
    if (!native) Section("Gameplay and debug");
    else Section("Cheats (restart required)");
    for (int e = native ? 0 : 8; e < BbSettings::FirstEnhancement; ++e) {
        if (native && !IsCheat(e)) continue;
        const char* label = BbSettings::Effects[e].label;
        if (!std::strncmp(label, "Cheat: ", 7)) label += 7;
        const std::string shown = std::string(1, char(std::toupper((unsigned char)label[0]))) + (label + 1);
        ImGui::BeginDisabled(BbSettings::OnlineBlocked(e));
        Checkbox(shown.c_str(), s.effects[e]);
        ImGui::EndDisabled();
    }
    if (BbSettings::Online())
        Note("Playing online: cheats, the free camera and the debug menu are off.", Ink::Faint);
    if (!native)
        Hint("Free camera: hold Cross/A and press L3 (keyboard: E + Left Ctrl). The debug menu needs DbgFont14h.ccm and DbgFont14h.tpf in dvdroot_ps4/font.");
    Section("PC enhancements (restart required)");
    for (int e = BbSettings::FirstEnhancement; e < BbSettings::EffectCount; ++e)
        Checkbox(BbSettings::Effects[e].label, s.effects[e]);
    Hint("Made from your own game files at start; off is the game as it shipped. The mirror in the Hunter's Dream opens the appearance editor. Two invaders: Mensis, Mergo's Loft, the Nightmare Frontier and the Old Hunters' areas allow two, as the chalice dungeons do. After bbhost by droogie.");
    CloseRows();
    const bool needs_restart = mod_selection_changed ||
        (native ? ExtrasRestartNeeded() : RestartNeeded());
    if (needs_restart)
        Note("Changes are ready. Restart to apply them.", Ink::Warn);
    if (needs_restart && PrimaryButton("Apply and restart")) {
        BbSettings::Save();
        runtime_restart();
    }
    Note("Other mods and their load order are in the launcher.", Ink::Faint);
}

std::string UnlockDate(std::int64_t timestamp) {
    if (timestamp <= 0) return {};
    const std::time_t when = static_cast<std::time_t>(timestamp);
    const std::tm* local = std::localtime(&when);
    if (!local) return {};
    char date[48]{};
    std::strftime(date, sizeof(date), "%d %b %Y", local);
    return date;
}

void AchievementRow(const BbTrophies::Entry& entry) {
    const bool gallery = NativePcMenuAvailable();
    const float h = (gallery ? 72.0f : 98.0f) * base_scale;
    const bool earned = entry.unlocked_at > 0;
    ImGui::PushID(entry.id);
    ImGui::Selectable("##achievement", false, 0, ImVec2(0, h));
    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float icon = (gallery ? 46.0f : 52.0f) * base_scale;
    const ImVec2 icon_min(min.x + 9.0f * base_scale, min.y + (h - icon) * .5f);
    const ImVec2 icon_max(icon_min.x + icon, icon_min.y + icon);
    const auto texture = IconRef(entry.id);
    const ImU32 tint = earned ? IM_COL32_WHITE : IM_COL32(105, 112, 110, 255);
    if (texture._TexData || texture._TexID != ImTextureID_Invalid)
        draw->AddImageRounded(texture, icon_min, icon_max, ImVec2(0, 0), ImVec2(1, 1),
                              tint, 8.0f * base_scale);
    else
        draw->AddRectFilled(icon_min, icon_max, IM_COL32(40, 48, 46, 255), 8.0f * base_scale);
    const ImVec2 center(icon_min.x + icon * .5f, icon_min.y + icon * .5f);
    draw->AddCircle(center, icon * .51f, GradeColor(entry.grade, earned ? 1.0f : .45f),
                    0, 2.0f * base_scale);
    const float text_x = icon_max.x + 14.0f * base_scale;
    const float right_x = max.x - 64.0f * base_scale;
    const bool secret = entry.hidden && !earned;
    const std::string name = secret ? "Secret trophy" : entry.name;
    const std::string detail = secret ? "Unlock to reveal this achievement" : entry.description;
    const std::string score = secret || entry.score < 0 ? "??G" :
                              std::to_string(entry.score) + "G";
    const ImVec4 name_clip(text_x, min.y, right_x - 7.0f * base_scale, max.y);
    const float name_y = gallery ? min.y + (h - ImGui::GetFontSize()) * 0.5f : min.y + 10.0f * base_scale;
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(text_x, name_y),
                  earned ? Ink::Text : Ink::Dim, name.c_str(), nullptr, 0.0f, &name_clip);
    const char* state = gallery ? (earned ? "Earned" : "Locked") : score.c_str();
    draw->AddText(ImVec2(max.x - ImGui::CalcTextSize(state).x - 4.0f * base_scale, name_y),
                  earned ? Ink::Good : Ink::Faint, state);
    if (!gallery && !detail.empty())
        draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * .83f,
                      ImVec2(text_x, min.y + 34.0f * base_scale),
                      earned ? IM_COL32(180, 192, 183, 255) : IM_COL32(112, 122, 115, 255),
                      detail.c_str(), nullptr, right_x - text_x - 7.0f * base_scale);
    if (!gallery && earned) {
        const auto date = UnlockDate(entry.unlocked_at);
        draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * .78f,
                      ImVec2(text_x, min.y + 76.0f * base_scale),
                      IM_COL32(137, 209, 145, 255), date.c_str());
    }
    Rule(draw, min.x, max.x, max.y - 1.0f);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(name.c_str());
        if (!detail.empty()) ImGui::TextWrapped("%s", detail.c_str());
        ImGui::EndTooltip();
    }
    ImGui::PopID();
}

// A section title that opens and closes with a chevron; `hint` shows on the right while closed.
bool FoldSection(const char* title, const char* hint, bool* open) {
    CloseRows();
    ImGui::Dummy(ImVec2(0, Px(18.0f)));
    const float h = ImGui::GetTextLineHeight() + Px(6.0f);
    if (ImGui::InvisibleButton(title, ImVec2(ImGui::GetContentRegionAvail().x, h))) *open = !*open;
    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 ink = ImGui::IsItemHovered() || ImGui::IsItemFocused() ? Ink::Text : Ink::Dim;
    const float s = Px(4.5f);
    const ImVec2 c(min.x + s, (min.y + max.y) * .5f);
    // Pointing down when open, right when closed.
    const ImVec2 down[3] = {ImVec2(c.x - s, c.y - s * .45f), ImVec2(c.x, c.y + s * .55f), ImVec2(c.x + s, c.y - s * .45f)};
    const ImVec2 right[3] = {ImVec2(c.x - s * .45f, c.y - s), ImVec2(c.x + s * .55f, c.y), ImVec2(c.x - s * .45f, c.y + s)};
    draw->AddPolyline(*open ? down : right, 3, ink, 0, Px(1.7f));
    const float text_y = min.y + (h - ImGui::GetTextLineHeight()) * .5f;
    draw->AddText(ImVec2(min.x + s * 3.2f, text_y), ink, title);
    if (!*open && hint)
        draw->AddText(ImVec2(max.x - ImGui::CalcTextSize(hint).x, text_y), Ink::Faint, hint);
    Rule(draw, min.x, max.x, max.y + Px(8.0f));
    ImGui::Dummy(ImVec2(0, Px(6.0f)));
    return *open;
}

// Trophy notifications: theme, sound and timing, and previews that go through the same queue,
// renderer and sounds as a real unlock (nothing is unlocked or saved). The texts, pictures and
// animations come from files in user/custom_trophy (README.md).
void ToastSection(const std::vector<BbTrophies::Entry>& entries) {
    using BbToast::Theme;
    static bool open = false; // closed: the page opens on the trophy list
    if (!FoldSection("TROPHY NOTIFICATIONS", "Theme, sound and previews", &open)) return;
    auto o = BbToast::GetOptions();
    bool changed = false;
    // The four looks, all in sight: one row each.
    for (int i = 0; i < int(Theme::Count); ++i)
        if (ChoiceRow(BbToast::ThemeLabel(Theme(i)), o.theme == Theme(i)) && o.theme != Theme(i)) {
            o.theme = Theme(i);
            changed = true;
        }
    changed |= Toggle("Mute the sound", &o.muted);
    RowLabel("Volume", ImGui::GetFrameHeight());
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::BeginDisabled(o.muted);
    changed |= ImGui::SliderInt("##toast_volume", &o.volume, 0, 100, "%d%%");
    ImGui::EndDisabled();
    RowLabel("Time on screen", ImGui::GetFrameHeight());
    ImGui::SetNextItemWidth(-FLT_MIN);
    changed |= ImGui::SliderFloat("##toast_hold", &o.hold_seconds, 0.0f, 20.0f,
                                  o.hold_seconds < 0.05f ? "Theme default" : "%.1f s");
    if (o.hold_seconds < 0.05f) o.hold_seconds = 0.0f;
    if (o.theme == Theme::CardCinematic) {
        RowLabel("Platinum light", ImGui::GetFrameHeight());
        ImGui::SetNextItemWidth(-FLT_MIN);
        changed |= ImGui::SliderInt("##toast_intensity", &o.cinematic_intensity, 0, 100, "%d%%");
    }
    if (o.theme != Theme::Original) {
        changed |= Toggle("Animate the platinum", &o.platinum_animation);
        Hint("Off: the platinum shows its own picture, like the other trophies.");
        changed |= Toggle("Write the grade", &o.grade_label);
    }
    if (changed) BbToast::SetOptions(o);
    CloseRows();

    ImGui::Dummy(ImVec2(0, Px(6.0f)));
    ImGui::BeginDisabled(entries.empty());
    // A real trophy of the grade (a visible one when there is one), shown as an unlock would be.
    // Without the trophy key no trophy has a grade: the first one is shown as that grade.
    auto preview = [&](BbTrophies::Grade grade) {
        const BbTrophies::Entry* pick = nullptr;
        for (const auto& entry : entries)
            if (entry.grade == grade && (!pick || (pick->hidden && !entry.hidden))) pick = &entry;
        BbToast::Enqueue(pick ? pick->id : entries.front().id, true, int(grade));
    };
    using G = BbTrophies::Grade;
    const std::pair<const char*, G> grades[] = {
        {"Bronze", G::Bronze}, {"Silver", G::Silver}, {"Gold", G::Gold}, {"Platinum", G::Platinum}};
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float button_w = (ImGui::GetContentRegionAvail().x - spacing * 3) / 4;
    for (int i = 0; i < 4; ++i) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(GradeColor(grades[i].second)));
        if (ImGui::Button(grades[i].first, ImVec2(button_w, 0))) preview(grades[i].second);
        ImGui::PopStyleColor();
        if (i < 3) ImGui::SameLine();
    }
    const float half = (ImGui::GetContentRegionAvail().x - spacing) / 2;
    if (ImGui::Button("All four in a row", ImVec2(half, 0)))
        for (const auto& item : grades) preview(item.second);
    ImGui::SameLine();
    if (ImGui::Button("Three at once", ImVec2(-FLT_MIN, 0)))
        for (int i = 0; i < 3; ++i) preview(G::Bronze);
    ImGui::EndDisabled();
    Note("Previews look and sound like an unlock; nothing is unlocked or saved.", Ink::Faint);
    NoteF(Ink::Faint, "Sounds: %s", BbToast::SoundSummary().c_str());
    NoteF(Ink::Faint, "Pictures: %s", BbToast::PictureSummary().c_str());
    Note("Your own sounds, pictures, animations and texts go in user/custom_trophy (see README.md). "
         "Sounds reload here; pictures when the game starts.", Ink::Faint);
    if (ImGui::Button("Reload sounds")) BbToast::ReloadSounds();
    ImGui::Dummy(ImVec2(0, Px(6.0f)));
}

void AchievementsTab() {
    const bool gallery = NativePcMenuAvailable();
    auto entries = BbTrophies::Entries();
    const int total = int(entries.size());
    int earned = 0;
    for (const auto& entry : entries) earned += entry.unlocked_at > 0;
    const int score = BbTrophies::EarnedScore();
    PageTitle(gallery ? "Trophy gallery" : "Trophies");
    if (gallery)
        Note("Trophy artwork. Full details, dates and Gamerscore are in Pause > System > Achievements.");
    else {
        if (BbTrophies::TotalScore() == 1000)
            ImGui::Text("Gamerscore: %d / 1000G", score);
        else
            ImGui::Text("Gamerscore: -- / 1000G");
        ImGui::Text("%d of %d unlocked", earned, total);
        ImGui::ProgressBar(total ? float(earned) / total : 0.0f,
                           ImVec2(-1, 9.0f * base_scale), "");
    }
    ToastSection(entries);
    if (!BbTrophies::HasMetadata())
        Note("Names and scores need the trophy key from your own PS4. Icons and local unlock history still work.");
    ImGui::Dummy(ImVec2(0, Px(8.0f)));
    if (entries.empty()) {
        Note("No trophy pack in the selected game folder.", Ink::Faint);
        return;
    }
    ImGuiListClipper clipper;
    clipper.Begin(total, (gallery ? 72.0f : 98.0f) * base_scale + ImGui::GetStyle().ItemSpacing.y);
    while (clipper.Step())
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            AchievementRow(entries[i]);
    if (!gallery) ImGui::TextDisabled("Local achievements only. No online account is connected.");
}

// Line icons, the launcher's set (launcher/qml/NavIcon.qml) on its 24 × 24 grid.
void Glyph(ImDrawList* draw, const char* kind, ImVec2 center, float size, ImU32 ink, float stroke) {
    const float k = size / 24.0f;
    auto P = [&](float x, float y) { return ImVec2(center.x + (x - 12.0f) * k, center.y + (y - 12.0f) * k); };
    auto poly = [&](std::initializer_list<float> v, bool closed) {
        draw->PathClear();
        for (auto it = v.begin(); it != v.end(); it += 2) draw->PathLineTo(P(*it, *(it + 1)));
        draw->PathStroke(ink, closed ? ImDrawFlags_Closed : ImDrawFlags_None, stroke);
    };
    if (!std::strcmp(kind, "hunter")) { // a blade crossed with a compass rose
        poly({12, 1.5f, 12, 22.5f}, false);
        poly({3.5f, 12, 20.5f, 12}, false);
        poly({12, 5, 15, 12, 12, 19, 9, 12}, true);
        poly({9.5f, 20, 12, 22.5f, 14.5f, 20}, false);
    } else if (!std::strcmp(kind, "gear")) {
        draw->AddCircle(P(12, 12), 3.2f * k, ink, 0, stroke);
        draw->AddCircle(P(12, 12), 6.4f * k, ink, 0, stroke);
        for (int a = 0; a < 8; ++a) {
            const float t = float(a) * 3.14159265f / 4.0f;
            draw->AddLine(P(12 + 6.4f * std::cos(t), 12 + 6.4f * std::sin(t)),
                          P(12 + 9.0f * std::cos(t), 12 + 9.0f * std::sin(t)), ink, stroke);
        }
    } else if (!std::strcmp(kind, "puzzle")) { // a jigsaw piece, knobs on top and right
        draw->PathClear();
        draw->PathLineTo(P(4, 7.5f));
        draw->PathLineTo(P(9.2f, 7.5f));
        draw->PathBezierCubicCurveTo(P(8.2f, 5.8f), P(8.6f, 3.5f), P(11, 3.5f));
        draw->PathBezierCubicCurveTo(P(13.4f, 3.5f), P(13.8f, 5.8f), P(12.8f, 7.5f));
        draw->PathLineTo(P(17.5f, 7.5f));
        draw->PathLineTo(P(17.5f, 11.7f));
        draw->PathBezierCubicCurveTo(P(19.2f, 10.7f), P(21.5f, 11.1f), P(21.5f, 13.5f));
        draw->PathBezierCubicCurveTo(P(21.5f, 15.9f), P(19.2f, 16.3f), P(17.5f, 15.3f));
        draw->PathLineTo(P(17.5f, 20.5f));
        draw->PathLineTo(P(4, 20.5f));
        draw->PathStroke(ink, ImDrawFlags_Closed, stroke);
    } else if (!std::strcmp(kind, "chevron")) {
        poly({9, 5, 16, 12, 9, 19}, false);
    } else if (!std::strcmp(kind, "menu")) { // the Close mark: three bars in a circle
        draw->AddCircle(P(12, 12), 10.5f * k, ink, 0, stroke * 0.8f);
        for (int i = -1; i <= 1; ++i) poly({7.5f, 12 + 3.5f * i, 16.5f, 12 + 3.5f * i}, false);
    }
}

struct GuideSection {
    const char* icon;
    const char* label;
};
GuideSection GuideSectionOf(int tab, bool native) {
    switch (tab) {
    case 0: return {"hunter", "The Hunt"};
    case 1: return {"trophy", native ? "Trophy gallery" : "View trophies"};
    case 2: return {"gear", native ? "Image tools" : "Visual settings"};
    default: return {"puzzle", native ? "Gameplay extras" : "Mods & patches"};
    }
}
void SectionGlyph(ImDrawList* draw, int tab, ImVec2 center, float size, ImU32 ink) {
    const GuideSection section = GuideSectionOf(tab, NativePcMenuAvailable());
    if (!std::strcmp(section.icon, "trophy")) {
        TrophyGlyph(draw, center, size * 1.05f, float((ink >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f * 0.85f);
        return;
    }
    Glyph(draw, section.icon, center, size, ink, Px(2.0f));
}

// A row under the mouse takes the outline from the focused one; with neither, the first row has it.
bool hunt_row_hovered_last = false, hunt_row_focused_last = false;

void HomeTab() {
    const bool native = NativePcMenuAvailable();
    const auto entries = BbTrophies::Entries();
    const int earned = int(std::count_if(entries.begin(), entries.end(),
                                         [](const auto& e) { return e.unlocked_at > 0; }));
    const float s = base_scale;
    const float width = ImGui::GetContentRegionAvail().x;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImGui::PushFont(guide_serif_font, 48.0f);
    ImGui::TextUnformatted("The Hunt");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 10.0f * s));
    Rule(draw, ImGui::GetCursorScreenPos().x, ImGui::GetCursorScreenPos().x + width, ImGui::GetCursorScreenPos().y);
    ImGui::Dummy(ImVec2(0, 14.0f * s));
    ImGui::PushFont(guide_serif_font, 23.0f);
    ImGui::TextUnformatted(FpsText().c_str());
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 24.0f * s));

    // Trophy progress: the cup, the count, a crimson bar.
    const ImVec2 at = ImGui::GetCursorScreenPos();
    TrophyGlyph(draw, ImVec2(at.x + 21.0f * s, at.y + 24.0f * s), 44.0f * s, .85f);
    ImGui::SetCursorScreenPos(ImVec2(at.x + 70.0f * s, at.y));
    ImGui::PushFont(guide_serif_font, 22.0f);
    if (entries.empty()) ImGui::TextUnformatted("No trophy pack");
    else ImGui::Text("%d of %d trophies", earned, int(entries.size()));
    ImGui::PopFont();
    const char* local = "Local only";
    const ImVec2 local_size = ImGui::CalcTextSize(local);
    draw->AddText(ImVec2(at.x + width - local_size.x, at.y + 4.0f * s), ImGui::GetColorU32(Ink::Dim), local);
    const float bar_x = at.x + 70.0f * s, bar_y = at.y + 38.0f * s;
    const float bar_w = std::max(80.0f * s, width - 70.0f * s - local_size.x - 40.0f * s);
    draw->AddRectFilled(ImVec2(bar_x, bar_y), ImVec2(bar_x + bar_w, bar_y + 9.0f * s), IM_COL32(40, 45, 51, 255), 4.5f * s);
    if (!entries.empty() && earned > 0)
        draw->AddRectFilled(ImVec2(bar_x, bar_y),
                            ImVec2(bar_x + std::max(9.0f * s, bar_w * float(earned) / entries.size()), bar_y + 9.0f * s),
                            Ink::Accent, 4.5f * s);
    ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + 76.0f * s));

    // The sections as rows: icon, serif label, chevron. The focused row (or the one under the
    // mouse) has the crimson outline.
    bool any_hovered = false, any_focused = false;
    for (int i = 1; i <= 3; ++i) {
        ImGui::PushID(i);
        const float h = 70.0f * s;
        const ImVec2 min = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##guide_section", ImVec2(width, h), ImGuiButtonFlags_EnableNav)) active_tab = i;
        if (i == 1 && ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
        const bool hovered = ImGui::IsItemHovered();
        any_hovered |= hovered;
        const bool focused = ImGui::IsItemFocused();
        any_focused |= focused;
        const bool selected = hovered || (focused && !hunt_row_hovered_last) ||
                              (i == 1 && !hunt_row_hovered_last && !hunt_row_focused_last);
        const ImVec2 max(min.x + width, min.y + h);
        draw->AddRectFilled(min, max, selected ? IM_COL32(22, 25, 29, 250) : IM_COL32(17, 20, 24, 245), 6.0f * s);
        draw->AddRect(min, max, selected ? Ink::Accent : Ink::Line, 6.0f * s, 0, selected ? 1.6f * s : 1.0f * s);
        SectionGlyph(draw, i, ImVec2(min.x + 40.0f * s, min.y + h * 0.5f), 28.0f * s, IM_COL32(222, 217, 209, 255));
        ImFont* serif = guide_serif_font ? guide_serif_font : ImGui::GetFont();
        draw->AddText(serif, 23.0f * s, ImVec2(min.x + 84.0f * s, min.y + (h - 23.0f * s) * 0.5f - 1.0f * s),
                      Ink::Text, GuideSectionOf(i, native).label);
        Glyph(draw, "chevron", ImVec2(max.x - 30.0f * s, min.y + h * 0.5f), 18.0f * s, IM_COL32(207, 202, 194, 255), 1.8f * s);
        ImGui::Dummy(ImVec2(0, 4.0f * s));
        ImGui::PopID();
    }
    hunt_row_hovered_last = any_hovered;
    hunt_row_focused_last = any_focused;
    if (native) {
        ImGui::Dummy(ImVec2(0, 6.0f * s));
        Note("The game's own options, and the PC settings, are in Pause > System.", Ink::Faint);
    }
}

void Menu() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float s = base_scale;
    const float appear = std::clamp(float(ClockMs() - menu_opened_ms.load()) / 180.0f, 0.0f, 1.0f);
    ImGui::GetBackgroundDrawList()->AddRectFilled(viewport->Pos,
        ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y),
        IM_COL32(0, 0, 0, int(56 * appear)));
    const float panel_width = std::min(viewport->WorkSize.x,
        std::clamp(viewport->WorkSize.x * .43f, 500.0f * s, 840.0f * s));
    const float rail_width = 112.0f * s;
    const float pad = 40.0f * s;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x - (1.0f - appear) * 36.0f * s, viewport->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(panel_width, viewport->WorkSize.y));
    ImGui::SetNextWindowBgAlpha(.985f * appear);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, .6f + .4f * appear);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##bloodborne_guide", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 panel_pos = ImGui::GetWindowPos();
    const float panel_h = viewport->WorkSize.y;
    ImDrawList* frame = ImGui::GetWindowDrawList();
    frame->AddRectFilled(ImVec2(panel_pos.x + panel_width - 1.0f, panel_pos.y),
                         ImVec2(panel_pos.x + panel_width, panel_pos.y + panel_h), Ink::LineStrong);

    // The rail: the four sections as icons, the open one marked in crimson.
    ImGui::SetCursorPos(ImVec2(0, 0));
    // The rail, the page and its content share one navigation space (NavFlattened), so the D-pad
    // moves from a section icon straight into its rows and back.
    ImGui::BeginChild("##guide_rail", ImVec2(rail_width, 0), ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 rail_pos = ImGui::GetWindowPos();
    ImDrawList* rail = ImGui::GetWindowDrawList();
    rail->AddRectFilled(rail_pos, ImVec2(rail_pos.x + rail_width, rail_pos.y + panel_h), Ink::Rail);
    rail->AddRectFilled(ImVec2(rail_pos.x + rail_width - 1.0f, rail_pos.y),
                        ImVec2(rail_pos.x + rail_width, rail_pos.y + panel_h), Ink::Line);
    const bool native = NativePcMenuAvailable();
    // Console-style movement: along the rail the page follows the focus; Right enters the page at
    // its first option and Left, from anywhere on the page, returns to the section's icon (not
    // whichever icon happens to sit level with the option). The page is off the left-hand
    // controls while a slider or text field is being edited.
    const auto pressed = [](ImGuiKey a, ImGuiKey b, ImGuiKey c) {
        return ImGui::IsKeyPressed(a) || ImGui::IsKeyPressed(b) || ImGui::IsKeyPressed(c);
    };
    const bool to_page = rail_focused_last &&
        pressed(ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight, ImGuiKey_RightArrow);
    const bool to_rail = page_focused_last && !ImGui::IsAnyItemActive() &&
        pressed(ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft, ImGuiKey_LeftArrow);
    // The rail ends at its first and last icon rather than dropping into the page.
    if (rail_focused_last &&
        ((active_tab == 3 && pressed(ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown, ImGuiKey_DownArrow)) ||
         (active_tab == 0 && pressed(ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp, ImGuiKey_UpArrow))))
        ImGui::NavMoveRequestCancel();
    if (to_rail) ImGui::NavMoveRequestCancel();
    bool rail_focused = false;
    for (int i = 0; i < 4; ++i) {
        ImGui::SetCursorPos(ImVec2(16.0f * s, (50.0f + 104.0f * i) * s));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##rail", ImVec2(rail_width - 32.0f * s, 96.0f * s), ImGuiButtonFlags_EnableNav))
            active_tab = i;
        if (i == active_tab) {
            ImGui::SetItemDefaultFocus();
            if (to_rail) {
                ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                ImGui::SetNavCursorVisible(true);
            }
        }
        if (ImGui::IsItemFocused() || (to_rail && i == active_tab)) {
            rail_focused = true;
            if (!to_page) active_tab = i;
        }
        ImGui::PopID();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", GuideSectionOf(i, native).label);
        const bool selected = active_tab == i;
        const bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
        const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        if (selected || hot)
            rail->AddRectFilled(min, max, selected ? IM_COL32(30, 19, 22, 255) : IM_COL32(21, 24, 28, 255), 4.0f * s);
        if (selected)
            rail->AddRectFilled(ImVec2(rail_pos.x, min.y), ImVec2(rail_pos.x + 4.0f * s, max.y), Ink::Accent);
        SectionGlyph(rail, i, ImVec2((min.x + max.x) * .5f, (min.y + max.y) * .5f), 38.0f * s,
                     selected ? IM_COL32(242, 236, 231, 255) : hot ? IM_COL32(205, 200, 193, 255) : IM_COL32(143, 138, 132, 255));
        Rule(rail, min.x + 18.0f * s, max.x - 18.0f * s, max.y + 3.0f * s, IM_COL32(42, 47, 53, 255));
    }
    ImGui::EndChild();

    // The page: breadcrumb and Close, the section's content, the key help.
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::BeginChild("##guide_main", ImVec2(0, 0), ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 main_pos = ImGui::GetWindowPos();
    const ImVec2 main_size = ImGui::GetWindowSize();
    ImDrawList* main = ImGui::GetWindowDrawList();
    ImGui::SetCursorPos(ImVec2(pad, 50.0f * s));
    ImGui::PushFont(guide_serif_font, 20.0f);
    ImGui::TextUnformatted("BLOODBORNE");
    ImGui::PopFont();
    ImGui::SameLine(0.0f, 16.0f * s);
    ImGui::PushStyleColor(ImGuiCol_Text, Vec(Ink::Faint));
    ImGui::TextUnformatted("/");
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, 16.0f * s);
    ImGui::PushStyleColor(ImGuiCol_Text, Vec(Ink::Dim));
    ImGui::TextUnformatted("HUNTER'S GUIDE");
    ImGui::PopStyleColor();
    // How to close: the buttons that do it (L3 + R3), as key caps. Clickable with the mouse, but
    // not a navigation stop, so the D-pad never lands on it.
    {
        const float cap_w = 40.0f * s, cap_h = 28.0f * s, gap = 8.0f * s;
        const ImVec2 word = ImGui::CalcTextSize("Close");
        const ImVec2 plus = ImGui::CalcTextSize("+");
        const float total = cap_w * 2.0f + plus.x + gap * 3.0f + word.x;
        const float x0 = main_size.x - pad - total, y0 = 50.0f * s;
        ImGui::SetCursorPos(ImVec2(x0, y0));
        if (ImGui::InvisibleButton("Close guide", ImVec2(total, cap_h))) SetOpen(false);
        const bool close_hot = ImGui::IsItemHovered();
        const ImU32 ink = close_hot ? IM_COL32(255, 250, 245, 255) : Ink::Text;
        float x = main_pos.x + x0;
        const float y = main_pos.y + y0;
        const float ty = y + (cap_h - word.y) * 0.5f;
        for (const char* cap : {"L3", "R3"}) {
            main->AddRect(ImVec2(x, y), ImVec2(x + cap_w, y + cap_h), close_hot ? Ink::Dim : Ink::LineStrong, cap_h * 0.5f, 0, 1.2f * s);
            const ImVec2 t = ImGui::CalcTextSize(cap);
            main->AddText(ImVec2(x + (cap_w - t.x) * 0.5f, ty), ink, cap);
            x += cap_w + gap;
            if (cap[0] == 'L') {
                main->AddText(ImVec2(x, ty), Ink::Faint, "+");
                x += plus.x + gap;
            }
        }
        main->AddText(ImVec2(x, ty), ink, "Close");
    }
    Rule(main, main_pos.x + pad, main_pos.x + main_size.x - pad, main_pos.y + 112.0f * s);

    ImGui::SetCursorPos(ImVec2(pad, 142.0f * s));
    ImGui::BeginChild("##guide_content", ImVec2(main_size.x - 2.0f * pad,
                      std::max(100.0f * s, main_size.y - 272.0f * s)), ImGuiChildFlags_NavFlattened);
    rows_id = 0;
    if (to_page) {
        // The page's first option, as Tab would reach it (SetKeyboardFocusHere would also press it).
        ImGui::SetNavWindow(ImGui::GetCurrentWindow());
        ImGui::NavMoveRequestSubmit(ImGuiDir_None, ImGuiDir_Down, ImGuiNavMoveFlags_IsTabbing | ImGuiNavMoveFlags_FocusApi,
                                    ImGuiScrollFlags_KeepVisibleEdgeX | ImGuiScrollFlags_KeepVisibleEdgeY);
        GImGui->NavTabbingDir = 1;
        GImGui->NavTabbingCounter = 1;
        ImGui::SetNavCursorVisible(true);
    }
    switch (active_tab) {
    case 0: HomeTab(); break;
    case 1: AchievementsTab(); break;
    case 2: if (native) ImageToolsTab(); else SettingsTab(); break;
    case 3: ModsTab(); break;
    }
    CloseRows();
    ImGui::EndChild();
    rail_focused_last = rail_focused;
    page_focused_last = !rail_focused && ImGui::GetFocusID() != 0;
    Rule(main, main_pos.x + pad, main_pos.x + main_size.x - pad, main_pos.y + main_size.y - 112.0f * s);
    main->AddText(ImVec2(main_pos.x + pad, main_pos.y + main_size.y - 84.0f * s), Ink::Dim,
                  "D-pad  Move    ·    A  Select    ·    B  Back    ·    LB/RB  Sections");
    main->AddText(ImVec2(main_pos.x + pad, main_pos.y + main_size.y - 52.0f * s), Ink::Faint,
                  "Keyboard: Insert opens and closes the guide  ·  Arrows move  ·  Enter selects");
    ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleVar(2);
}

// The game's text dialog: what is typed, and how to finish (keyboard or controller).
void TextEntryBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.42f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(viewport->WorkSize.x * 0.32f, 0.0f),
                                        ImVec2(viewport->WorkSize.x * 0.8f, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin("##text_entry", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s", text_entry_prompt.c_str());
    ImGui::Separator();
    ImGui::SetWindowFontScale(1.4f);
    ImGui::Text("%s_", text_entry_text.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    ImGui::TextDisabled("Keyboard: type, Backspace to erase, Enter = OK, Esc = cancel");
    ImGui::TextDisabled("Controller: Cross (A) = OK, Circle (B) = cancel");
    ImGui::End();
}

void FpsCounter() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad,
                                   viewport->WorkPos.y + pad),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    const auto& s = BbSettings::Get();
    ImGui::Text("%s  %s", FpsText().c_str(),
                s.upscaler == BbSettings::UpscalerFsr3   ? "FSR 3.1"
                : s.upscaler == BbSettings::UpscalerFsr4 ? "FSR 4"
                : s.upscaler == BbSettings::UpscalerFsr411 ? "FSR 4.1.1"
                : s.upscaler == BbSettings::UpscalerTaa ? "TAA"
                : s.upscaler == BbSettings::UpscalerDlss ? "DLSS"
                                                         : "");
    ImGui::End();
}

// The notifications' Original theme (BbToast draws the other themes and keeps the queue): the
// green banner at the bottom. alpha: opacity; appear: 0 -> 1 as it fades in and rises.
void TrophyBanner(int id, const char* first_line, float alpha, float appear) {
    const auto entry = BbTrophies::Find(id);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float w = std::min(590.0f * base_scale, viewport->WorkSize.x - 24.0f * base_scale);
    const float h = 92.0f * base_scale;
    const float slide = (1.0f - appear) * 28.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * .5f - w * .5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y - h -
                                       30.0f * base_scale + slide));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::SetNextWindowBgAlpha(.96f * alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, h * .5f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(.055f, .48f, .065f, 1));
    ImGui::Begin("##trophy_toast", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetWindowPos();
    const float icon = 72.0f * base_scale;
    const ImVec2 center(pos.x + 49.0f * base_scale, pos.y + h * .5f);
    draw->AddCircleFilled(center, icon * .5f, IM_COL32(64, 170, 37, int(255 * alpha)));
    draw->AddCircle(center, icon * .49f, GradeColor(entry.grade, alpha), 0,
                    2.0f * base_scale);
    TrophyGlyph(draw, center, 45.0f * base_scale, alpha);
    const float text_x = pos.x + 103.0f * base_scale;
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.03f,
                  ImVec2(text_x, pos.y + 19.0f * base_scale),
                  IM_COL32(255, 255, 255, int(255 * alpha)), first_line);
    const std::string title = entry.score >= 0 ?
        std::to_string(entry.score) + "G - " + entry.name : entry.name;
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.12f,
                  ImVec2(text_x, pos.y + 49.0f * base_scale),
                  IM_COL32(255, 255, 255, int(255 * alpha)), title.c_str(), nullptr,
                  w - (text_x - pos.x) - 18.0f * base_scale);
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

// The ImGui style and fonts of the guide (Init; also the offscreen preview, tools/overlay_preview.cpp).
void ConfigureContext() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    // The launcher's theme (launcher/qml/Theme.qml): flat near-black surfaces, hairlines, crimson.
    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.PopupRounding = 6.0f;
    style.FrameRounding = 6.0f;
    style.GrabRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.WindowBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.FramePadding = ImVec2(12.0f, 8.0f);
    style.ItemSpacing = ImVec2(10.0f, 10.0f);
    style.CellPadding = ImVec2(2.0f, 4.0f);
    style.GrabMinSize = 14.0f;
    style.ScrollbarSize = 10.0f;
    style.DisabledAlpha = 0.4f;
    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = Vec(Ink::Text);
    c[ImGuiCol_TextDisabled] = Vec(Ink::Faint);
    c[ImGuiCol_WindowBg] = Vec(Ink::Panel);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = Vec(IM_COL32(21, 25, 30, 252));
    c[ImGuiCol_Border] = Vec(Ink::Line);
    c[ImGuiCol_FrameBg] = Vec(Ink::Surface);
    c[ImGuiCol_FrameBgHovered] = Vec(Ink::SurfaceHover);
    c[ImGuiCol_FrameBgActive] = Vec(Ink::SurfaceHover);
    c[ImGuiCol_Button] = Vec(Ink::Surface);
    c[ImGuiCol_ButtonHovered] = Vec(Ink::SurfaceHover);
    c[ImGuiCol_ButtonActive] = Vec(Ink::AccentSoft);
    c[ImGuiCol_Header] = Vec(Ink::AccentSoft);
    c[ImGuiCol_HeaderHovered] = Vec(Ink::SurfaceHover);
    c[ImGuiCol_HeaderActive] = Vec(Ink::AccentSoft);
    c[ImGuiCol_SliderGrab] = Vec(Ink::Accent);
    c[ImGuiCol_SliderGrabActive] = Vec(IM_COL32(217, 65, 78, 255));
    c[ImGuiCol_CheckMark] = Vec(Ink::Accent);
    c[ImGuiCol_Separator] = Vec(Ink::Line);
    c[ImGuiCol_TableBorderLight] = Vec(Ink::Line);
    c[ImGuiCol_TableBorderStrong] = Vec(Ink::Line);
    c[ImGuiCol_PlotHistogram] = Vec(Ink::Accent);
    c[ImGuiCol_NavCursor] = Vec(Ink::Accent);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = Vec(Ink::LineStrong);
    c[ImGuiCol_ScrollbarGrabHovered] = Vec(Ink::Faint);
    c[ImGuiCol_ScrollbarGrabActive] = Vec(Ink::Dim);

    // Text in Segoe UI (as the launcher) where Windows has it; the embedded DejaVu Sans fills in
    // any character it lacks and is the face everywhere else.
    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    bool system_sans = false;
#ifdef _WIN32
    if (const char* windows = std::getenv("WINDIR")) {
        const auto segoe = std::filesystem::path(windows) / "Fonts" / "segoeui.ttf";
        if (std::filesystem::is_regular_file(segoe))
            system_sans = io.Fonts->AddFontFromFileTTF(segoe.string().c_str(), 19.0f) != nullptr;
    }
#endif
    font_config.MergeMode = system_sans;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_font_ttf),
                                   int(bb_font_ttf_end - bb_font_ttf), 18.0f, &font_config);
#ifdef _WIN32
    // The system serif for the guide's titles, without bundling a Windows font.
    if (const char* windows = std::getenv("WINDIR")) {
        const auto georgia = std::filesystem::path(windows) / "Fonts" / "georgia.ttf";
        if (std::filesystem::is_regular_file(georgia))
            guide_serif_font = io.Fonts->AddFontFromFileTTF(georgia.string().c_str(), 28.0f);
    }
#endif
    // Hunter's Atlas: the map's serif, made from the player's own menu font by its generator.
    if (BbMap::Enabled()) {
        const auto map_font = BbAtlasPath("BB_MAP_FONT", BbAtlasPath("BB_MAP_DIR", "navigation-map/maps")
                                                             .parent_path() / "fonts/bloodborne-native.ttf");
        std::ifstream face_file(map_font, std::ios::binary | std::ios::ate);
        if (face_file) {
            const auto bytes = face_file.tellg();
            if (bytes > 0 && bytes < 16 * 1024 * 1024) {
                void* data = IM_ALLOC(size_t(bytes));
                face_file.seekg(0);
                face_file.read(static_cast<char*>(data), bytes);
                if (face_file) {
                    if (auto* face = io.Fonts->AddFontFromMemoryTTF(data, int(bytes), 24.0f))
                        BbMap::SetTitleFont(face);
                } else {
                    IM_FREE(data);
                }
            }
        }
    }
    // Trophy notifications: their fonts, options, pictures and sounds (decoded now, not at the
    // first toast).
    BbToast::Start(io.Fonts, &IconRef, &PrefetchIcon, &TrophyBanner);
}

// One frame's windows, between NewFrame and Render (Render; also the offscreen preview).
void DrawWindows(float display_height) {
    // UI scale follows the display height (1080p = 1).
    const float scale = std::max(display_height / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    if (menu_open) {
        Menu();
        if (BbMap::IsOpen()) BbMap::Close();
    }
    // Hunter's Atlas: the open map, or the Map tile beside the game's quick menu.
    if (BbMap::IsOpen()) BbMap::Draw();
    if (!menu_open && !text_entry_active && !BbMap::IsOpen()) BbAtlasMenu::Draw();
    ImGui::GetIO().MouseDrawCursor = menu_open || BbMap::IsOpen() || BbAtlasMenu::Visible();
    if (BbSettings::Get().show_fps && !menu_open && !BbMap::IsOpen()) {
        FpsCounter();
    }
    if (text_entry_active) {
        TextEntryBox();
    }
    if (BbToast::Busy()) {
        BbToast::Draw(base_scale);
    }
}

} // namespace

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "bbport";

    ConfigureContext();

    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(
                *static_cast<const vk::Instance*>(user), name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    info.DescriptorPoolSize = 256; // font atlas, the game's trophy icons, the notifications' pictures
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    initialized = true;
    BbMapArt::Init();
    BbMap::Init();
    std::printf("Overlay: menu ready (Insert or L3+R3)\n");
}

void UpdateTextInput(SDL_Window* window) {
    bool want = false;
    {
        std::scoped_lock lock{imgui_mutex};
        want = initialized && menu_open && ImGui::GetIO().WantTextInput;
    }
    if (want != SDL_TextInputActive(window)) {
        if (want) {
            SDL_StartTextInput(window);
        } else {
            SDL_StopTextInput(window);
        }
    }
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    // Hunter's Atlas first: the Map tile's mouse, and the open map's keys and mouse.
    BbAtlasMenu::Suppress(menu_open || text_entry_active);
    if (!text_entry_active && BbAtlasMenu::HandleMouse(event)) return true;
    if (!text_entry_active && BbMap::HandleEvent(event)) return true;
    const bool is_open = menu_open || BbMap::IsOpen();
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        if (down && !event.key.repeat &&
            (event.key.key == SDLK_INSERT || (is_open && event.key.key == SDLK_ESCAPE))) {
            if (BbMap::IsOpen()) BbMap::Close();
            SetOpen(event.key.key == SDLK_INSERT ? !menu_open : false);
            return true;
        }
        if (!is_open) {
            return false;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & SDL_KMOD_ALT) != 0);
        if (const ImGuiKey key = KeyFromSdl(event.key.key); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const u8 button = event.gbutton.button;
        if (button == SDL_GAMEPAD_BUTTON_LEFT_STICK) {
            l3_down = down;
        } else if (button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
            r3_down = down;
        }
        if (down && l3_down && r3_down && BbSettings::Get().overlay_pad_shortcut.load()) {
            SetOpen(!is_open);
            return true;
        }
        if (!is_open) {
            return false;
        }
        if (button == SDL_GAMEPAD_BUTTON_EAST && down) {
            if (active_tab == 0) SetOpen(false);
            else active_tab = 0;
            return true;
        }
        if (button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER ||
            button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) {
            if (down) active_tab = (active_tab +
                (button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER ? 1 : 3)) % 4;
            return true;
        }
        if (const ImGuiKey key = KeyFromGamepad(button); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (!is_open) return false;
        const float value = std::clamp(float(event.gaxis.value) / 32767.0f, -1.0f, 1.0f);
        const auto analog = [&](ImGuiKey key, float magnitude) {
            const float amount = std::clamp((magnitude - .24f) / .76f, 0.0f, 1.0f);
            io.AddKeyAnalogEvent(key, amount > 0.0f, amount);
        };
        if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) {
            analog(ImGuiKey_GamepadLStickLeft, -value);
            analog(ImGuiKey_GamepadLStickRight, value);
        } else if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
            analog(ImGuiKey_GamepadLStickUp, -value);
            analog(ImGuiKey_GamepadLStickDown, value);
        }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT: {
        // Typed characters (Ctrl+click on a slider, a text field): key events alone erase but
        // do not type. SDL sends them while text input is on (UpdateTextInput).
        if (!is_open) {
            return false;
        }
        io.AddInputCharactersUTF8(event.text.text);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!is_open) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!is_open) {
            return false;
        }
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT  ? 1
                           : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                      : -1;
        if (button >= 0) {
            io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!is_open) {
            return false;
        }
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        return true;
    default:
        return false;
    }
}

bool Visible() {
    return initialized && (menu_open || text_entry_active || BbSettings::Get().show_fps ||
                           BbMap::IsOpen() || BbAtlasMenu::Visible() || BbAtlasMenu::Animating() ||
                           BbToast::Busy());
}

bool CapturesInput() {
    return menu_open || text_entry_active || BbMap::CapturesInput();
}

uint32_t RoutePad(uint32_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry) {
    // Runs on the game's pad read. Not under imgui_mutex: the present thread holds it for a
    // whole overlay frame (the open map's drawing included), which stalled the game.
    // BbAtlasMenu and BbMap guard their own state.
    if (!initialized || menu_open || text_entry_active) return buttons;
    return BbAtlasMenu::RoutePad(buttons, lx, ly, rx, ry);
}

bool ReleasesMouse() {
    return CapturesInput() || BbAtlasMenu::Interacting();
}

void SetTextEntry(bool active, const std::string& prompt, const std::string& text) {
    std::scoped_lock lock{imgui_mutex};
    text_entry_active = active;
    text_entry_prompt = prompt;
    text_entry_text = text;
}

void LoadTrophies(const std::int64_t* timestamps, unsigned count) {
    std::scoped_lock lock{trophy_mutex};
    trophies.fill(0);
    if (timestamps) {
        for (unsigned i = 0; i < std::min<unsigned>(count, trophies.size()); ++i)
            trophies[i] = timestamps[i] ? 1 : 0;
    }
    BbToast::Clear();
}

void TrophyUnlocked(int id) {
    if (id < 0 || id >= int(trophies.size())) return;
    {
        std::scoped_lock lock{trophy_mutex};
        trophies[id] = 1;
    }
    BbToast::Enqueue(id, false);
}

// Called for every presented image: the interval for the FPS readout and BB_FPS_LOG.
void NotePresent() {
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    last_present_ms = ms;
    if (ms > 0.0f && ms < 1000.0f) {
        frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
    }
    // BB_FPS_LOG=1: presents per second and the slowest frame, every 5 seconds (test runs).
    static const bool fps_log = [] {
        const char* env = std::getenv("BB_FPS_LOG");
        return env && env[0] == '1';
    }();
    if (fps_log) {
        static auto window_start = now;
        static int frames = 0;
        static float worst = 0.0f;
        ++frames;
        worst = std::max(worst, ms < 1000.0f ? ms : 0.0f);
        const float elapsed = std::chrono::duration<float>(now - window_start).count();
        if (elapsed >= 5.0f) {
            std::printf("Frame rate: %.1f FPS, slowest frame %.1f ms\n", frames / elapsed, worst);
            window_start = now;
            frames = 0;
            worst = 0.0f;
        }
    }
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {
    {
        std::scoped_lock lock{imgui_mutex};
        if (initialized) {
            BbMap::PollTestCommands();
            BbMap::Tick();
            BbAtlasMenu::Suppress(menu_open || text_entry_active);
            BbAtlasMenu::Tick();
        }
    }
    const float ms = last_present_ms;
    if (!Visible()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    DrawWindows(float(extent.height));
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

} // namespace BbOverlay
