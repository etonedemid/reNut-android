#pragma once
// Android: the screen before the game, in NutStaller's blueprint style (its
// bg.png art and the Andy font, from the APK's assets). Picks the game
// folder, the Vulkan driver and the settings; Play starts the game.
//
// Drawn in the overlay's ImGui on a 1200x675 design canvas, scaled to fit the
// screen like NutStaller's Viewbox.

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_keyboard.h>
#include <rex/cvar.h>
#include <rex/rex_app.h>
#include <rex/ui/image_decode.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "renut_engine/app/android_driver.h"
#include "renut_engine/app/path_config_store.h"
#include "renut_engine/app/xiso.h"
#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/ui/touch_controls.h"
#include "renut_engine/gpu/backend/native_window.h"
#include "renut_engine/gpu/overlay/overlay_ui.h"

class AndroidManager : public renut::gpu::overlay::Dialog {
public:
    using CompletionFn = std::function<void(rex::PathConfig)>;

    ~AndroidManager() override {
        extract_.cancel = true;
        if (extract_thread_.joinable())
            extract_thread_.join();
    }

    void Init(const std::string &app_name, const rex::PathConfig &defaults, CompletionFn on_play) {
        app_name_ = app_name;
        defaults_ = defaults;
        on_play_ = std::move(on_play);
        std::string game = "/sdcard/reNut/game";
        if (auto saved = PathConfigStore::TryLoad(app_name))
            game = saved->game_data_root.string();
        SetBuf(game_buf_, game);
        selected_driver_ = renut::android::SelectedDriver();
        RefreshDrivers();
        RefreshIsos();
        visible_ = true;
    }

    void Draw() override {
        const bool back = renut::touch::TakeBackPress();
        if (!visible_) {
            if (started_)
                DrawInGame(back);
            return;
        }
        LoadAssets();
        ImGuiIO &io = ImGui::GetIO();
        panel_x_ = 634.0f;
        panel_y_ = 281.0f;
        settings_h_ = 316.0f;

        // The design canvas, letterboxed in the blueprint colour.
        scale_ = std::min(io.DisplaySize.x / kW, io.DisplaySize.y / kH);
        origin_ = ImVec2((io.DisplaySize.x - kW * scale_) * 0.5f, (io.DisplaySize.y - kH * scale_) * 0.5f);

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("reNut manager", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize, kBlueprint);
        if (bg_)
            dl->AddImage(bg_->GetTexRef(), P(0, 0), P(kW, kH));

        if (NavButton("Set Up Game", 402, 335, 185, 32, page_ == Page::Game)) page_ = Page::Game;
        if (NavButton("Drivers", 402, 371, 185, 32, page_ == Page::Drivers)) page_ = Page::Drivers;
        if (NavButton("Settings", 402, 409, 185, 32, page_ == Page::Settings)) page_ = Page::Settings;
        if (NavButton("Credits", 402, 445, 185, 32, page_ == Page::Credits)) page_ = Page::Credits;
        if (NavButton("Play", 402, 496, 185, 36, false, 24)) Play();

        Panel(634, 281, 551, 382);
        switch (page_) {
        case Page::Game: DrawGame(); break;
        case Page::Drivers: DrawDrivers(); break;
        case Page::Settings: DrawSettings(); break;
        case Page::Credits: DrawCredits(); break;
        }

        ImGui::End();
        ImGui::PopStyleVar(2);

        // Android's keyboard comes up only while a text field wants it.
        if (io.WantTextInput != text_input_) {
            text_input_ = io.WantTextInput;
            if (SDL_Window *window = renut::gpu::GetGameSdlWindow())
                text_input_ ? SDL_StartTextInput(window) : SDL_StopTextInput(window);
        }
    }

private:
    enum class Page { Game, Drivers, Settings, Credits };
    static constexpr float kW = 1200.0f, kH = 675.0f;
    static constexpr ImU32 kBlueprint = IM_COL32(45, 82, 168, 255);
    static constexpr ImU32 kInk = IM_COL32(255, 255, 255, 255);
    static constexpr ImU32 kInkDim = IM_COL32(255, 255, 255, 200);
    static constexpr ImU32 kShadow = IM_COL32(0, 0, 40, 120);
    static constexpr ImU32 kLineDim = IM_COL32(255, 255, 255, 110);
    static constexpr ImU32 kNavy = IM_COL32(10, 20, 60, 255);
    static constexpr size_t kBufSize = 512;

    // --- assets -------------------------------------------------------------

    void LoadAssets() {
        if (assets_loaded_)
            return;
        assets_loaded_ = true;
        size_t size = 0;
        // Relative paths are the APK's assets on Android.
        if (void *ttf = SDL_LoadFile("ANDYB.TTF", &size)) {
            void *copy = IM_ALLOC(size);  // the atlas frees it with IM_FREE
            std::memcpy(copy, ttf, size);
            SDL_free(ttf);
            font_ = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(copy, int(size), 22.0f);
            renut::touch::SetFont(font_);
        }
        if (void *png = SDL_LoadFile("bg.png", &size)) {
            int w = 0, h = 0;
            auto pixels = rex::ui::DecodeImageRGBA(static_cast<const uint8_t *>(png), size, w, h);
            SDL_free(png);
            if (!pixels.empty()) {
                bg_ = IM_NEW(ImTextureData)();
                bg_->Create(ImTextureFormat_RGBA32, w, h);
                std::memcpy(bg_->GetPixels(), pixels.data(), pixels.size());
                ImGui::RegisterUserTexture(bg_);
            }
        }
    }

    // --- drawing helpers, in design units --------------------------------------

    ImVec2 P(float x, float y) const { return ImVec2(origin_.x + x * scale_, origin_.y + y * scale_); }
    ImVec2 S(float w, float h) const { return ImVec2(w * scale_, h * scale_); }
    ImFont *Font() const { return font_ ? font_ : ImGui::GetFont(); }

    // Andy text with NutStaller's soft drop shadow, vertically centred.
    void Text(const std::string &text, float x, float y, float w, float h, float size,
              ImU32 ink = kInk, bool center = false, bool shadow = true) {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const float px = size * scale_;
        const ImVec2 extent = Font()->CalcTextSizeA(px, FLT_MAX, w * scale_, text.c_str());
        ImVec2 pos = P(x, y);
        pos.y += (h * scale_ - extent.y) * 0.5f;
        if (center)
            pos.x += (w * scale_ - extent.x) * 0.5f;
        const ImVec4 clip(P(x, y).x, P(x, y).y, P(x + w, y + h).x, P(x + w, y + h).y);
        if (shadow)
            dl->AddText(Font(), px, ImVec2(pos.x + 2 * scale_, pos.y + 2 * scale_), kShadow,
                        text.c_str(), nullptr, w * scale_, &clip);
        dl->AddText(Font(), px, pos, ink, text.c_str(), nullptr, w * scale_, &clip);
    }

    static std::string Upper(std::string s) {
        for (char &c : s)
            c = char(std::toupper(static_cast<unsigned char>(c)));
        return s;
    }

    // BkButton: a gradient slab, darker on the left, with a faint line under it.
    bool NavButton(const std::string &label, float x, float y, float w, float h, bool selected,
                   float size = 22) {
        ImGui::SetCursorScreenPos(P(x, y));
        ImGui::PushID(label.c_str());
        ImGui::PushID(int(x * 1000 + y));
        const bool clicked = ImGui::InvisibleButton("##b", S(w, h));
        ImGui::PopID();
        ImGui::PopID();
        const bool down = ImGui::IsItemActive();
        const bool hot = ImGui::IsItemHovered();
        const int left = selected || down ? 250 : hot ? 225 : 185;
        const int right = selected || down ? 90 : hot ? 45 : 15;
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImU32 l = IM_COL32(4, 10, 34, left), r = IM_COL32(4, 10, 34, right);
        dl->AddRectFilledMultiColor(P(x, y), P(x + w, y + h), l, r, r, l);
        dl->AddLine(P(x, y + h - 0.5f), P(x + w, y + h - 0.5f), kLineDim);
        Text(Upper(label), x + 10, y + 1, w - 14, h - 2, size);
        return clicked;
    }

    // BkPagePanel: translucent navy with a dashed border.
    void Panel(float x, float y, float w, float h) {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(P(x, y), P(x + w, y + h), IM_COL32(14, 28, 80, 170));
        Dashed(x + 1.5f, y + 1.5f, w - 3, h - 3, kLineDim, 3);
    }

    void Dashed(float x, float y, float w, float h, ImU32 col, float dash, float thick = 1) {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        auto line = [&](float x0, float y0, float x1, float y1) {
            const float len = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
            for (float t = 0; t < len; t += dash * 2) {
                const float e = std::min(t + dash, len);
                dl->AddLine(P(x0 + (x1 - x0) * t / len, y0 + (y1 - y0) * t / len),
                            P(x0 + (x1 - x0) * e / len, y0 + (y1 - y0) * e / len), col,
                            thick * scale_);
            }
        };
        line(x, y, x + w, y);
        line(x + w, y, x + w, y + h);
        line(x + w, y + h, x, y + h);
        line(x, y + h, x, y);
    }

    // Page coordinates are relative to the panel.
    void Label(const std::string &text, float x, float y, float w, float h, float size = 18,
               bool dim = false) {
        Text(text, panel_x_ + x, panel_y_ + y, w, h, size, dim ? kInkDim : kInk);
    }

    bool Button(const std::string &text, float x, float y, float w, float h, float size = 20,
                bool selected = false) {
        return NavButton(text, panel_x_ + x, panel_y_ + y, w, h, selected, size);
    }

    // BkValueBox: a navy slab with an editable value.
    bool Field(const char *id, std::string &buf, float x, float y, float w, float h) {
        ImGui::SetCursorScreenPos(P(panel_x_ + x, panel_y_ + y));
        ImGui::PushFont(Font(), 18 * scale_);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(kNavy));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImGui::ColorConvertU32ToFloat4(kNavy));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(30 / 255.f, 55 / 255.f, 130 / 255.f, 1));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(6 * scale_, (h * scale_ - 18 * scale_) * 0.5f));
        ImGui::SetNextItemWidth(w * scale_);
        const bool changed = ImGui::InputText(id, buf.data(), kBufSize);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
        ImGui::PopFont();
        return changed;
    }

    static void SetBuf(std::string &buf, const std::string &value) {
        buf = value;
        buf.resize(kBufSize, '\0');
    }

    // --- pages -----------------------------------------------------------------

    void DrawGame() {
        Label("SET UP GAME", 18, 16, 400, 30, 26);
        Label("GAME FOLDER (THE ONE WITH DEFAULT.XEX)", 19, 58, 500, 26, 16, true);
        Field("##game", game_buf_, 19, 88, 513, 32);

        const std::filesystem::path game = game_buf_.c_str();
        std::error_code ec;
        const bool storage = renut::android::HasStorageAccess();
        std::string state;
        if (!storage)
            state = "RENUT CAN'T READ SHARED STORAGE YET";
        else if (std::filesystem::exists(game / "default.xex", ec))
            state = "DEFAULT.XEX FOUND - READY TO PLAY";
        else
            state = "NO DEFAULT.XEX IN THIS FOLDER";
        Label(state, 19, 128, 400, 28, 18, !storage);
        if (!storage && Button("Allow file access", 19, 160, 240, 32))
            renut::android::RequestStorageAccess();

        DrawExtract(storage);
        Label("DRIVER: " + Upper(renut::android::DriverStatus()), 19, 350, 513, 24, 15, true);
        if (!error_.empty())
            Label(error_, 19, 200, 513, 24, 16);
    }

    // --- disc image extraction (extract-xiso) ------------------------------------

    void RefreshIsos() {
        isos_.clear();
        std::error_code ec;
        for (const char *dir : {"/sdcard/reNut", "/sdcard/Download"}) {
            for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
                std::string ext = entry.path().extension().string();
                for (char &c : ext)
                    c = char(std::tolower(static_cast<unsigned char>(c)));
                if (entry.is_regular_file(ec) && ext == ".iso")
                    isos_.push_back(entry.path());
            }
        }
        std::sort(isos_.begin(), isos_.end());
    }

    bool Extracting() const { return extract_.running; }

    void DrawExtract(bool storage) {
        if (extract_thread_.joinable() && !extract_.running)
            extract_thread_.join();

        Label("OR EXTRACT A DISC IMAGE (.ISO IN /SDCARD/RENUT OR DOWNLOAD):", 19, 206, 513, 22, 15,
              true);
        if (Extracting()) {
            const float done = extract_.total ? float(extract_.done) / float(extract_.total) : 0.0f;
            ImDrawList *dl = ImGui::GetWindowDrawList();
            // BkProgress: a white outline filled from the left.
            dl->AddRect(P(panel_x_ + 19, panel_y_ + 236), P(panel_x_ + 419, panel_y_ + 252), kInk);
            dl->AddRectFilled(P(panel_x_ + 21, panel_y_ + 238),
                              P(panel_x_ + 21 + 396 * done, panel_y_ + 250), kInk);
            if (Button("Cancel", 430, 230, 105, 28, 17))
                extract_.cancel = true;
            std::string current;
            {
                std::lock_guard lock(extract_.mutex);
                current = extract_.current;
            }
            Label(std::to_string(int(done * 100)) + "% " + Upper(current), 19, 262, 513, 22, 14,
                  true);
            return;
        }
        float y = 232;
        if (!storage) {
            Label("ALLOW FILE ACCESS TO SEE THEM", 19, y, 513, 22, 15, true);
            return;
        }
        if (isos_.empty())
            Label("NONE FOUND", 19, y, 300, 22, 15, true);
        for (size_t i = 0; i < isos_.size() && i < 2; ++i, y += 32) {
            if (Button("Extract " + isos_[i].filename().string(), 19, y, 405, 28, 17))
                StartExtract(isos_[i]);
        }
        if (Button("Rescan", 430, 232, 105, 28, 17))
            RefreshIsos();
        if (!extract_message_.empty())
            Label(extract_message_, 19, 300, 513, 40, 15);
    }

    void StartExtract(const std::filesystem::path &iso) {
        std::filesystem::path out = game_buf_.c_str();
        if (out.empty())
            out = "/sdcard/reNut/game";
        SetBuf(game_buf_, out.string());
        error_.clear();
        extract_message_.clear();
        extract_.cancel = false;
        extract_.running = true;
        extract_thread_ = std::thread([this, iso, out] {
            const bool ok = renut::xiso::Extract(iso, out, extract_);
            {
                std::lock_guard lock(extract_.mutex);
                extract_message_ = ok ? "EXTRACTED TO " + Upper(out.string())
                                      : "EXTRACTION FAILED: " + Upper(extract_.error);
            }
            extract_.running = false;
        });
    }

    void RefreshDrivers() {
        drivers_ = renut::android::InstalledDrivers();
        zips_ = renut::android::DriverZips();
    }

    void DrawDrivers() {
        Label("VULKAN DRIVER", 18, 16, 400, 30, 26);
        Label("NOW: " + Upper(renut::android::DriverStatus()), 19, 50, 513, 24, 16, true);

        float y = 82;
        if (Button("System driver", 19, y, 300, 30, 20, selected_driver_.empty()))
            selected_driver_.clear();
        y += 36;
        for (const auto &d : drivers_) {
            if (y > 260)
                break;
            if (Button(d.name, 19, y, 300, 30, 20, selected_driver_ == d.id))
                selected_driver_ = d.id;
            if (Button("Remove", 330, y, 110, 30, 18)) {
                renut::android::RemoveDriver(d.id);
                if (selected_driver_ == d.id)
                    selected_driver_.clear();
                RefreshDrivers();
                return;
            }
            y += 36;
        }

        // Zips in /sdcard/reNut/drivers, not yet installed.
        y = std::max(y + 6, 200.0f);
        Label("INSTALL FROM /SDCARD/RENUT/DRIVERS:", 19, y, 513, 22, 15, true);
        y += 26;
        if (zips_.empty())
            Label("NO DRIVER ZIPS THERE (ADRENOTOOLS FORMAT, E.G. TURNIP)", 19, y, 513, 22, 15, true);
        for (const auto &zip : zips_) {
            if (y > 300)
                break;
            if (Button("+ " + zip.filename().string(), 19, y, 420, 28, 17)) {
                std::string error;
                const std::string id = renut::android::InstallDriverZip(zip, &error);
                driver_message_ = id.empty() ? "COULDN'T INSTALL: " + Upper(error) : "INSTALLED " + Upper(id);
                if (!id.empty())
                    selected_driver_ = id;
                RefreshDrivers();
                return;
            }
            y += 32;
        }
        if (Button("Rescan", 450, 82, 85, 30, 18))
            RefreshDrivers();

        const bool changed = selected_driver_ != renut::android::SelectedDriver() ||
                             selected_driver_ != renut::android::LoadedDriver();
        if (changed) {
            if (Button("Restart to apply", 19, 336, 240, 32)) {
                renut::android::SelectDriver(selected_driver_);
                SaveSettings();
                renut::android::RestartApp();
            }
        } else if (!driver_message_.empty()) {
            Label(driver_message_, 19, 336, 513, 32, 16, true);
        }
    }

    // reNut's own options (and a few of the SDK's), straight from the cvars.
    static bool ShownInSettings(const rex::cvar::FlagEntry &f) {
        static const char *kHidden[] = {"renut_gpu_dred", "renut_vulkan_debug_layers",
                                        "renut_gpu_backend", "renut_pipeline_list",
                                        "renut_shader_capture_dir", "renut_mods_folder",
                                        "renut_online_trace", "renut_gamertag", "renut_online"};
        for (const char *h : kHidden)
            if (f.name == h)
                return false;
        if (f.name == "audio_mute" || f.name == "vibration")
            return true;
        if (f.category.rfind("Nuts&Bolts/", 0) != 0 || f.category == "Nuts&Bolts/Debug")
            return false;
        return f.type == rex::cvar::FlagType::Boolean ||
               (f.type == rex::cvar::FlagType::String && f.constraints.HasAllowedValues());
    }

    void DrawSettings() {
        Label("SETTINGS", 18, 16, 300, 30, 26);
        if (!settings_message_.empty())
            Label(settings_message_, 300, 16, 235, 30, 16, true);

        ImGui::SetCursorScreenPos(P(panel_x_ + 12, panel_y_ + 54));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::BeginChild("##settings", S(527, settings_h_), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar);
        // Touch scrolling: drag the list.
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
            ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f)) {
            ImGui::SetScrollY(ImGui::GetScrollY() - ImGui::GetIO().MouseDelta.y);
        }

        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImVec2 top = ImGui::GetCursorScreenPos();
        const float row = 38 * scale_;
        int n = 0;
        std::string category;
        for (const auto &f : rex::cvar::GetRegistry()) {
            if (!ShownInSettings(f))
                continue;
            const ImVec2 at(top.x, top.y + n * row);
            const std::string value = f.getter ? f.getter() : std::string();
            std::string title = f.name;
            if (title.rfind("renut_", 0) == 0)
                title = title.substr(6);
            std::replace(title.begin(), title.end(), '_', ' ');

            dl->AddText(Font(), 18 * scale_, ImVec2(at.x + 2 * scale_, at.y + 10 * scale_), kShadow,
                        Upper(title).c_str());
            dl->AddText(Font(), 18 * scale_, ImVec2(at.x, at.y + 8 * scale_), kInkDim,
                        Upper(title).c_str());

            // The value: a slab that steps through the choices on a tap.
            ImGui::SetCursorScreenPos(ImVec2(at.x + 300 * scale_, at.y + 3 * scale_));
            ImGui::PushID(f.name.c_str());
            const ImVec2 size = S(210, 30);
            const ImVec2 a = ImGui::GetCursorScreenPos();
            // A press that turned into a scroll is no tap.
            const float slop = 12.0f * scale_;
            const bool tapped = ImGui::InvisibleButton("##v", size) &&
                                ImGui::GetIO().MouseDragMaxDistanceSqr[0] < slop * slop;
            const ImU32 slab = ImGui::IsItemActive() ? IM_COL32(30, 55, 130, 255) : kNavy;
            dl->AddRectFilled(a, ImVec2(a.x + size.x, a.y + size.y), slab);
            std::string shown = value;
            if (f.type == rex::cvar::FlagType::Boolean)
                shown = value == "true" ? "ON" : "OFF";
            const ImVec2 ext = Font()->CalcTextSizeA(18 * scale_, FLT_MAX, 0, Upper(shown).c_str());
            dl->AddText(Font(), 18 * scale_,
                        ImVec2(a.x + (size.x - ext.x) * 0.5f, a.y + (size.y - ext.y) * 0.5f), kInk,
                        Upper(shown).c_str());
            ImGui::PopID();
            if (tapped) {
                std::string next = value == "true" ? "false" : "true";
                if (f.type == rex::cvar::FlagType::String) {
                    const auto &values = f.constraints.allowed_values;
                    auto it = std::find(values.begin(), values.end(), value);
                    next = it == values.end() || it + 1 == values.end() ? values.front() : *(it + 1);
                }
                rex::cvar::SetFlagByName(f.name, next);
                SaveSettings();
            }
            ++n;
        }
        ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + n * row));
        ImGui::Dummy(ImVec2(1, 1));
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    void SaveSettings() {
        if (!defaults_.config_path.empty()) {
            rex::cvar::SaveConfig(defaults_.config_path);
            settings_message_ = "SAVED";
        }
    }

    void DrawCredits() {
        Label("CREDITS", 16, 14, 300, 30, 26);
        Label("RENUT BY MASTERSPIKE52 AND CONTRIBUTORS", 16, 56, 516, 26);
        static const char *kLines[] = {
            "REXGLUE TEAM - REXGLUE SDK",
            "SOLARCOOKIES - MIDASM HOOKS AND CRT FUNCTIONS",
            "VALCOMDRIFTY - RENUT LOGO",
            "OLIEGAMERTV - NUTS AND BOLTS DOCUMENTATION",
            "BYLAWS - LIBADRENOTOOLS",
            "ANDROID PORT - ETONEDEMID",
        };
        for (int i = 0; i < int(std::size(kLines)); i++)
            Label(kLines[i], 16, 88 + i * 26.0f, 516, 24, 16, true);
    }

    // --- in game: the side menu on the back gesture ------------------------------

    void SetMenuOpen(bool open) {
        menu_open_ = open;
        menu_page_ = 0;
        renut::touch::SetBlocked(open);
        // The game holds still under the menu (the last frame stays up).
        renut::gpu::Video::SetPaused(open);
        if (!open && text_input_) {
            text_input_ = false;
            if (SDL_Window *window = renut::gpu::GetGameSdlWindow())
                SDL_StopTextInput(window);
        }
    }

    void DrawInGame(bool back) {
        if (back) {
            if (renut::touch::Editing())
                renut::touch::BeginEdit();  // stays in edit mode: Done leaves it
            else
                SetMenuOpen(!menu_open_);
        }
        if (!menu_open_)
            return;
        LoadAssets();
        ImGuiIO &io = ImGui::GetIO();
        scale_ = std::min(io.DisplaySize.x / kW, io.DisplaySize.y / kH);
        origin_ = ImVec2(0, (io.DisplaySize.y - kH * scale_) * 0.5f);
        panel_x_ = 0;
        panel_y_ = 0;
        settings_h_ = 590.0f;

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("reNut side menu", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse |
                         ImGuiWindowFlags_NoBackground);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        // Dim the game; a tap beside the menu closes it.
        dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 20, 110));
        ImGui::SetCursorScreenPos(P(560, 0));
        if (ImGui::InvisibleButton("##outside", ImVec2(io.DisplaySize.x, io.DisplaySize.y)))
            SetMenuOpen(false);
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(P(560, 0).x, io.DisplaySize.y), IM_COL32(14, 28, 80, 235));
        Dashed(558.5f, -2, 0, kH + 4, kLineDim, 3);

        if (menu_page_ == 1) {
            DrawSettings();
            if (Button("< Back", 400, 14, 130, 32, 20))
                menu_page_ = 0;
        } else {
            Label("RENUT", 24, 18, 300, 40, 32);
            Label("PAUSED", 24, 56, 300, 24, 16, true);
            float y = 110;
            if (Button("Resume", 24, y, 300, 36, 22)) SetMenuOpen(false);
            y += 46;
            if (Button("Settings", 24, y, 300, 36, 22)) menu_page_ = 1;
            y += 46;
            if (Button("Touch layout", 24, y, 300, 36, 22)) {
                SetMenuOpen(false);
                renut::touch::BeginEdit();
            }
            y += 46;
            const bool touch = renut::touch::Enabled();
            if (Button(touch ? "Touch controls: on" : "Touch controls: off", 24, y, 300, 36, 22))
                renut::touch::SetEnabled(!touch);
            y += 46;
            if (Button("Exit to menu", 24, y + 20, 300, 36, 22)) {
                SaveSettings();
                renut::android::RestartApp();
            }
            if (renut::touch::GamepadConnected())
                Label("GAMEPAD CONNECTED: TOUCH CONTROLS HIDDEN", 24, 560, 520, 22, 15, true);
            Label("MAPPINGS: A B X Y RIGHT THUMB, STICK LEFT THUMB, DRAG RIGHT HALF = CAMERA", 24, 590,
                  520, 22, 13, true);
            Label("BACK GESTURE: OPEN / CLOSE THIS MENU", 24, 616, 520, 22, 15, true);
        }

        ImGui::End();
        ImGui::PopStyleVar(2);
    }

    // --- play ------------------------------------------------------------------

    void Play() {
        if (Extracting())
            return;
        const std::filesystem::path game = game_buf_.c_str();
        std::error_code ec;
        if (!std::filesystem::exists(game / "default.xex", ec)) {
            page_ = Page::Game;
            error_ = renut::android::HasStorageAccess() ? "NO DEFAULT.XEX THERE: PICK THE GAME FOLDER"
                                                        : "ALLOW FILE ACCESS FIRST";
            return;
        }
        rex::PathConfig cfg = defaults_;
        cfg.game_data_root = game;
        PathConfigStore::SavedPaths saved;
        saved.game_data_root = cfg.game_data_root;
        saved.user_data_root = cfg.user_data_root;
        saved.update_data_root = cfg.update_data_root;
        PathConfigStore::Save(app_name_, saved);
        visible_ = false;
        started_ = true;
        renut::touch::SetStarted(true);
        if (text_input_) {
            text_input_ = false;
            if (SDL_Window *window = renut::gpu::GetGameSdlWindow())
                SDL_StopTextInput(window);
        }
        on_play_(std::move(cfg));
    }

    std::string app_name_;
    rex::PathConfig defaults_;
    CompletionFn on_play_;
    bool visible_ = false;
    bool assets_loaded_ = false;
    bool text_input_ = false;
    ImFont *font_ = nullptr;
    ImTextureData *bg_ = nullptr;
    float scale_ = 1.0f;
    ImVec2 origin_;
    Page page_ = Page::Game;
    std::string game_buf_;
    std::string error_;
    std::string selected_driver_;
    std::string driver_message_;
    std::string settings_message_;
    std::vector<renut::android::Driver> drivers_;
    std::vector<std::filesystem::path> zips_;
    std::vector<std::filesystem::path> isos_;
    float panel_x_ = 634.0f, panel_y_ = 281.0f, settings_h_ = 316.0f;
    bool started_ = false;
    bool menu_open_ = false;
    int menu_page_ = 0;  // 0 main, 1 settings
    renut::xiso::Progress extract_;
    std::thread extract_thread_;
    std::string extract_message_;  // written by the worker under extract_.mutex
};
