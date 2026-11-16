#pragma once
#include <cairo/cairo.h>
#include <cstdlib>
#include <fontconfig/fontconfig.h>
#include "button_glyphs.h"
#include "ui/messages.h"

struct Dashboard {
    static constexpr int width = 1200, height = 800;
    static constexpr int padding = 24, content_x = 256 + padding, content_right = width - padding;
    static constexpr int content_width = content_right - content_x;
    vr::IVROverlay *api;
    ct::ui::EventSink emit;
    vr::VROverlayHandle_t panel = 0, thumbnail = 0;
    GLuint texture = 0, icon = 0;
    bool dirty = true, submitted = false, icon_ready = false, enabled = false,
         tracking_available = true;
    bool gamepad_connected = false, recovery = false;
    bool status_attention = false;
    bool autostart = true, input_enabled = false, pinned = true, show_requested = false;
    bool capturing = false;
    bool automatic_updates = true, updates_configured = false, update_busy = false;
    bool repair_busy = false;
    std::string repair_status = "Helper permissions are restored automatically.";
    std::string app_version = "Development", update_status = "Update server is not configured.";
    std::string input_state = "Off";
    bool steam_shortcuts = true;
    std::string pointer_label = "R4";
    int brightness = 100, smoothing = 40, page = 0, pointer_buttons = 0x80;
    cairo_surface_t *artwork = nullptr;
    const char *hovered = nullptr;
    const char *pressed = nullptr;
    std::string status = "Waiting for controller";
    enum Kind { Action, Navigation, Switch, Step };
    struct Button {
        int x, y, w, h, page;
        Kind kind;
        const char *action;
        const char *label;
    };
    const Button buttons[18] = {
        {280, 136, content_width, 96, 0, Switch, "toggle", ""},
        {280, 256, 440, 64, 0, Action, "recenter", "Realign"},
        {736, 256, 440, 64, 0, Action, "acquire", "Restart tracking"},
        {942, 407, 58, 58, 0, Step, "smoothing_down", "-"},
        {1094, 407, 58, 58, 0, Step, "smoothing_up", "+"},
        {280, 136, content_width, 96, 1, Switch, "input_toggle", ""},
        {954, 262, 198, 60, 1, Action, "pointer_binding", "R4"},
        {280, 576, content_width, 96, 1, Switch, "steam_shortcuts", ""},
        {280, 136, content_width, 80, 2, Switch, "autostart", ""},
        {280, 228, content_width, 80, 2, Switch, "pinned", ""},
        {280, 366, content_width, 80, 2, Switch, "automatic_updates", ""},
        {960, 480, 192, 60, 2, Action, "check_updates", "Check now"},
        {280, 636, 440, 60, 2, Action, "inspect_permissions", "Check permissions"},
        {736, 636, 440, 60, 2, Action, "repair_permissions", "Repair permissions"},
        {16, 140, 224, 64, -1, Navigation, "tracking_tab", "Tracking"},
        {16, 212, 224, 64, -1, Navigation, "controls_tab", "Controls"},
        {16, 284, 224, 64, -1, Navigation, "app_tab", "App"},
        {976, 740, 200, 44, -1, Action, "quit", "Exit app"}};
    explicit Dashboard(vr::IVROverlay *overlay, ct::ui::EventSink events,
                       const std::filesystem::path &root = {})
        : api(overlay), emit(std::move(events)) {
        // Use SteamVR's installed font, do not redistribute Valve's font files.
        const char *home = std::getenv("HOME");
        const std::string user = home ? home : "";
        for (const auto &directory :
             {std::string("/opt/steamvr/resources/webinterface/fonts/"),
              user + "/.local/share/Steam/steamapps/common/SteamVR/resources/webinterface/fonts/"})
            for (const auto &name : {"motiva-sans-regular.ttf", "motiva-sans-bold.ttf"}) {
                const auto path = directory + name;
                if (std::filesystem::is_regular_file(path))
                    FcConfigAppFontAddFile(nullptr,
                                           reinterpret_cast<const FcChar8 *>(path.c_str()));
            }
        artwork =
            cairo_image_surface_create_from_png((root / "assets/controller-icon.png").c_str());
        if (cairo_surface_status(artwork) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(artwork);
            artwork = nullptr;
            throw std::runtime_error("Controller Utils icon is missing or invalid");
        }
    }
    ~Dashboard() {
        for (auto handle : {thumbnail, panel})
            if (handle) {
                api->HideOverlay(handle);
                api->ClearOverlayTexture(handle);
                api->DestroyOverlay(handle);
            }
        glDeleteTextures(1, &texture);
        glDeleteTextures(1, &icon);
        if (artwork)
            cairo_surface_destroy(artwork);
    }
    void initialize() {
        check(api->CreateDashboardOverlay("org.controllerutils.frame", "Controller Utils", &panel,
                                          &thumbnail),
              "settings panel");
        check(api->SetOverlayFlag(panel, vr::VROverlayFlags_NoDashboardTab, true),
              "prepare taskbar icon");
        check(api->SetOverlayWidthInMeters(panel, 1.6f), "settings size");
        const vr::HmdVector2_t scale{float(width), float(height)};
        check(api->SetOverlayMouseScale(panel, &scale), "settings pointer scale");
        check(api->SetOverlayInputMethod(panel, vr::VROverlayInputMethod_Mouse),
              "settings pointer");
        const vr::VRTextureBounds_t bounds{0, 1, 1, 0};
        check(api->SetOverlayTextureBounds(panel, &bounds), "settings texture bounds");
        const vr::VRTextureBounds_t icon_bounds{0, 0, 1, 1};
        check(api->SetOverlayTextureBounds(thumbnail, &icon_bounds), "settings icon bounds");
        // Steam requests thumbnail metrics at app registration, including while the headset is
        // asleep.
        auto image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 128, 128);
        auto c = cairo_create(image);
        // Steam's taskbar reads thumbnail pixels without applying the overlay texture bounds.
        cairo_translate(c, 0, 128);
        cairo_scale(c, 1, -1);
        draw_icon(c, 8, 8, 112);
        upload(icon, thumbnail, image);
        cairo_destroy(c);
        cairo_surface_destroy(image);
    }
    bool accept(const ct::ui::Command &command) {
        if (const auto op = std::get_if<ct::ui::Operation>(&command);
            op && *op == ct::ui::Operation::ShowSettings) {
            show_requested = dirty = true;
            return true;
        }
        if (const auto update = std::get_if<ct::ui::Updates>(&command)) {
            dirty |= app_version != update->version || update_status != update->message ||
                     automatic_updates != update->automatic ||
                     updates_configured != update->configured || update_busy != update->busy;
            app_version = update->version;
            update_status = update->message;
            automatic_updates = update->automatic;
            updates_configured = update->configured;
            update_busy = update->busy;
            return true;
        }
        if (const auto repair = std::get_if<ct::ui::RepairStatus>(&command)) {
            dirty |= repair_status != repair->message || repair_busy != repair->busy;
            repair_status = repair->message;
            repair_busy = repair->busy;
            return true;
        }
        const auto next = std::get_if<ct::ui::Status>(&command);
        if (!next)
            return false;
        if (next->recovery) {
            dirty |= recovery != *next->recovery;
            recovery = *next->recovery;
        }
        if (next->tracking_available) {
            dirty |= tracking_available != *next->tracking_available;
            tracking_available = *next->tracking_available;
        }
        if (next->gamepad_connected) {
            dirty |= gamepad_connected != *next->gamepad_connected;
            gamepad_connected = *next->gamepad_connected;
        }
        if (next->status_attention) {
            dirty |= status_attention != *next->status_attention;
            status_attention = *next->status_attention;
        }
        dirty |= next->message != status || next->enabled != enabled ||
                 brightness != next->brightness || smoothing != next->smoothing;
        dirty |= autostart != next->autostart || pointer_buttons != next->pointer_buttons ||
                 pinned != next->pinned;
        if (panel && icon_ready && pinned != next->pinned)
            check(api->SetOverlayFlag(panel, vr::VROverlayFlags_NoDashboardTab, !next->pinned),
                  "taskbar pin");
        status = next->message;
        enabled = next->enabled;
        brightness = int(next->brightness);
        smoothing = int(next->smoothing);
        autostart = next->autostart;
        pointer_buttons = next->pointer_buttons;
        pinned = next->pinned;
        dirty |= pointer_label != next->pointer_label || capturing != next->binding_capture;
        pointer_label = next->pointer_label;
        capturing = next->binding_capture;
        dirty |= steam_shortcuts != next->steam_shortcuts;
        steam_shortcuts = next->steam_shortcuts;
        return true;
    }
    const char *hit(float x, float y) const {
        if (!std::isfinite(x) || !std::isfinite(y))
            return nullptr;
        for (const auto &b : buttons)
            if ((b.page == -1 || b.page == page) && available(b) && x >= b.x && x < b.x + b.w &&
                y >= b.y && y < b.y + b.h)
                return b.action;
        return nullptr;
    }
    bool available(const Button &button) const {
        const std::string action = button.action;
        if (action == "check_updates")
            return updates_configured && !update_busy;
        if (action == "inspect_permissions" || action == "repair_permissions")
            return !repair_busy;
        if (action == "input_toggle" && recovery)
            return false;
        if ((action == "acquire" || action == "recenter") && recovery)
            return true;
        if (action == "recenter" || action == "acquire")
            return enabled && tracking_available && gamepad_connected;
        if (action == "smoothing_down")
            return smoothing > 0;
        if (action == "smoothing_up")
            return smoothing < 150;
        return true;
    }
    const char *pointer(const vr::VREvent_t &event) {
        if (event.eventType == vr::VREvent_MouseMove) {
            // OpenVR supplies panel coordinates, the texture flip is not an input transform.
            const auto next = hit(event.data.mouse.x, event.data.mouse.y);
            dirty |= next != hovered;
            hovered = next;
        } else if (event.eventType == vr::VREvent_FocusLeave ||
                   event.eventType == vr::VREvent_OverlayHidden) {
            dirty |= hovered != nullptr;
            hovered = pressed = nullptr;
        } else if (event.eventType == vr::VREvent_MouseButtonDown &&
                   event.data.mouse.button == vr::VRMouseButton_Left) {
            pressed = hovered;
            dirty |= pressed != nullptr;
        } else if (event.eventType == vr::VREvent_MouseButtonUp &&
                   event.data.mouse.button == vr::VRMouseButton_Left) {
            const auto action = pressed == hovered ? hovered : nullptr;
            dirty |= pressed != nullptr;
            pressed = nullptr;
            return action;
        }
        return nullptr;
    }
    void events() {
        vr::VREvent_t event{};
        for (int count = 0; count < 64 && api->PollNextOverlayEvent(panel, &event, sizeof(event));
             ++count) {
            if (const auto action = pointer(event)) {
                if (std::string(action) == "tracking_tab" ||
                    std::string(action) == "controls_tab" || std::string(action) == "app_tab") {
                    page = std::string(action) == "tracking_tab"
                               ? 0
                               : (std::string(action) == "controls_tab" ? 1 : 2);
                    hovered = pressed = nullptr;
                    dirty = true;
                } else
                    emit(ct::ui::Action{action});
            }
        }
    }
    static void color(cairo_t *c, uint32_t value) {
        cairo_set_source_rgb(c, ((value >> 16) & 255) / 255., ((value >> 8) & 255) / 255.,
                             (value & 255) / 255.);
    }
    static void rectangle(cairo_t *c, double x, double y, double w, double h, uint32_t value) {
        color(c, value);
        cairo_rectangle(c, x, y, w, h);
        cairo_fill(c);
    }
    static void text(cairo_t *c, double x, double y, const std::string &value, double size = 26,
                     uint32_t ink = 0xdcdedf) {
        color(c, ink);
        ButtonGlyphs::instance().layout(c, value, size, true, x, y, ink);
    }
    static void centered(cairo_t *c, double x, double y, double w, double h,
                         const std::string &value, double size, uint32_t ink = 0xdcdedf,
                         bool left = false) {
        const auto bounds = ButtonGlyphs::instance().layout(c, value, size);
        cairo_save(c);
        cairo_rectangle(c, x, y, w, h);
        cairo_clip(c);
        text(c, std::round(x + (left ? 0 : (w - bounds.width) / 2) - bounds.x_bearing),
             std::round(y + (h - bounds.height) / 2 - bounds.y_bearing), value, size, ink);
        cairo_restore(c);
    }
    static void wrapped(cairo_t *c, double x, double y, const std::string &value, double limit,
                        int lines = 2, double size = 22, uint32_t ink = 0x8b929a) {
        cairo_set_font_size(c, size);
        std::string line;
        size_t start = 0;
        while (start < value.size() && lines > 0) {
            const auto end = value.find(' ', start);
            const auto word = value.substr(start, end == std::string::npos ? end : end - start);
            const auto candidate = line.empty() ? word : line + " " + word;
            const auto extents = ButtonGlyphs::instance().layout(c, candidate, size);
            if (!line.empty() && extents.x_advance > limit) {
                if (--lines == 0) {
                    line += "...";
                    break;
                }
                text(c, x, y, line, size, ink);
                y += size + 9;
                line = word;
            } else
                line = candidate;
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
        cairo_save(c);
        cairo_rectangle(c, x, y - size - 2, limit, size + 12);
        cairo_clip(c);
        text(c, x, y, line, size, ink);
        cairo_restore(c);
    }
    static double pair(cairo_t *c, double x, double y, double w, double h, const std::string &title,
                       const std::string &description, double title_size = 27,
                       double description_size = 20, uint32_t title_ink = 0xdcdedf,
                       uint32_t description_ink = 0x8b929a, double gap = 10) {
        auto layer = cairo_recording_surface_create(CAIRO_CONTENT_COLOR_ALPHA, nullptr);
        auto label = cairo_create(layer);
        cairo_set_font_face(label, cairo_get_font_face(c));
        text(label, 0, 0, title, title_size, title_ink);
        const auto heading = ButtonGlyphs::instance().layout(label, title, title_size);
        const auto detail = ButtonGlyphs::instance().layout(label, description, description_size);
        wrapped(label, 0, heading.y_bearing + heading.height + gap - detail.y_bearing, description,
                w, 2, description_size, description_ink);
        double left = 0, top = 0, ink_width = 0, ink_height = 0;
        cairo_recording_surface_ink_extents(layer, &left, &top, &ink_width, &ink_height);
        const double dy = std::round(y + (h - ink_height) / 2 - top);
        cairo_save(c);
        cairo_rectangle(c, x, y, w, h);
        cairo_clip(c);
        cairo_set_source_surface(c, layer, x - left, dy);
        cairo_paint(c);
        cairo_restore(c);
        cairo_destroy(label);
        cairo_surface_destroy(layer);
        return dy + heading.y_bearing + heading.height / 2;
    }
    void row(cairo_t *c, int y, const std::string &title, const std::string &description,
             int h = 96, int text_width = 752) const {
        bool focus = false;
        for (const auto &b : buttons)
            if (b.page == page && b.y == y && b.kind == Switch)
                focus = hovered == b.action && available(b);
        rectangle(c, content_x, y, content_width, h, focus ? 0xffffff : 0x23262e);
        if (description.empty())
            centered(c, 304, y, text_width, h, title, 27, focus ? 0x0e141b : 0xdcdedf, true);
        else
            pair(c, 304, y, text_width, h, title, description, 27, 20, focus ? 0x0e141b : 0xdcdedf,
                 focus ? 0x3d4450 : 0x8b929a);
    }
    void draw_icon(cairo_t *c, double x, double y, double size) const {
        const double w = cairo_image_surface_get_width(artwork),
                     h = cairo_image_surface_get_height(artwork);
        const double scale = size / std::max(w, h);
        cairo_save(c);
        cairo_translate(c, x + (size - w * scale) / 2, y + (size - h * scale) / 2);
        cairo_scale(c, scale, scale);
        cairo_set_source_surface(c, artwork, 0, 0);
        cairo_paint(c);
        cairo_restore(c);
    }
    void upload(GLuint &target, vr::VROverlayHandle_t handle, cairo_surface_t *image) {
        if (!target)
            glGenTextures(1, &target);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, target);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        cairo_surface_flush(image);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, cairo_image_surface_get_width(image),
                     cairo_image_surface_get_height(image), 0, GL_BGRA, GL_UNSIGNED_BYTE,
                     cairo_image_surface_get_data(image));
        glFinish();
        if (glGetError() != GL_NO_ERROR)
            throw std::runtime_error("settings GPU upload failed");
        const vr::Texture_t value{reinterpret_cast<void *>(uintptr_t(target)),
                                  vr::TextureType_OpenGL, vr::ColorSpace_Auto};
        check(api->SetOverlayTexture(handle, &value), "settings GPU texture");
    }
    cairo_surface_t *paint() const {
        auto image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
        auto c = cairo_create(image);
        color(c, 0x0e141b);
        cairo_paint(c);
        rectangle(c, 0, 0, 256, height, 0x1b2029);
        cairo_select_font_face(c, "Motiva Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        draw_icon(c, 24, 38, 48);
        pair(c, 88, 26, 144, 72, "Controller", "Utils", 25, 25, 0xdcdedf, 0xdcdedf, 6);
        text(c, 280, 66, page == 0 ? "Tracking" : (page == 1 ? "Controls" : "App"), 40);
        text(c, 280, 101,
             page == 0 ? "Automatically track your connected Steam Controller"
                       : (page == 1 ? "Navigate Steam and your desktop"
                                    : "Startup, taskbar and software updates"),
             21, 0x8b929a);
        if (page == 0) {
            row(c, 136, "Enable tracking", "");
            centered(c, 280, 328, 440, 40,
                     recovery ? "Opens the uninstaller on desktop."
                              : "Hold [View] + [Menu] for 1 second.",
                     19, 0x8b929a);
            centered(c, 736, 328, 440, 40,
                     recovery ? "Restarts SteamVR." : "Hold [View] + [Menu] for 5 seconds.", 19,
                     0x8b929a);
            row(c, 388, "Motion smoothing", "Softens position corrections when the LED returns.",
                96, 614);
            centered(c, 1000, 388, 94, 96, std::to_string(smoothing) + " ms", 22);
            if (status_attention) {
                rectangle(c, content_x, 508, content_width, 128, 0x23262e);
                rectangle(c, content_x, 508, 4, 128, 0xe9b04c);
                pair(c, 304, 508, content_width - 48, 128, "Needs attention", status, 24, 20,
                     0xe9b04c, 0xb8bcbf);
            }
        } else if (page == 1) {
            row(c, 136, "Control Steam UI",
                input_enabled ? input_state
                              : "Press " + ButtonGlyphs::binding(pointer_label) +
                                    " or use this switch to turn on.");
            row(c, 244, "Toggle shortcut",
                "Press once to turn both aiming and the pad cursor on or off.", 96, 626);
            text(c, 280, 390, "WHILE UI CONTROL IS ON", 19, 0x8b929a);
            centered(c, 280, 408, 188, 48, "Point or swipe", 24, 0xdcdedf, true);
            centered(c, 492, 408, 684, 48,
                     "Aim the controller. Swipe [RightPad] for a cursor. Lift to aim again.", 21,
                     0xb8bcbf, true);
            centered(c, 280, 464, 188, 48, "Click", 24, 0xdcdedf, true);
            centered(c, 492, 464, 684, 48, "[R2] or [RightPadClick] to click. [L2] to right-click.",
                     21, 0xb8bcbf, true);
            centered(c, 280, 520, 188, 48, "Scroll", 24, 0xdcdedf, true);
            centered(c, 492, 520, 684, 48, "[RightStick] or [LeftPad] to scroll.", 21, 0xb8bcbf,
                     true);
            row(c, 576, "[Steam] shortcuts",
                "Hold to recenter. Double-press for passthrough. [Steam] + [R1] to screenshot.");
            text(c, 280, 708,
                 "Keyboard: [Steam] + [X]. Look at keys, touch pads to type. Lift both to aim.", 20,
                 0xb8bcbf);
        } else {
            row(c, 136, "Run at startup", "Start Controller Utils automatically with SteamVR.", 80);
            row(c, 228, "Pin to taskbar", "Keep the controller icon in Steam's bottom bar.", 80);
            text(c, 280, 344, "SOFTWARE UPDATES", 19, 0x8b929a);
            row(c, 366, "Automatic updates",
                "Download automatically. Install when SteamVR next starts.", 80);
            rectangle(c, content_x, 458, content_width, 104, 0x23262e);
            pair(c, 304, 458, 632, 104, "Version " + app_version, update_status, 25, 21, 0xdcdedf,
                 0xb8bcbf);
            wrapped(c, 280, 591, repair_status, content_width, 2, 20);
        }
        rectangle(c, 256, 724, width - 256, 1, 0x3d4450);
        centered(c, 280, 724, 640, 76, "Steam Controller for Steam Frame", 20, 0x8b929a, true);
        centered(c, 24, 724, 208, 76, app_version, 18, 0x8b929a, true);
        for (const auto &b : buttons) {
            if (b.page != -1 && b.page != page)
                continue;
            const std::string action = b.action;
            const bool focus = hovered == b.action && available(b);
            if (b.kind == Switch) {
                const bool on =
                    action == "toggle" ? enabled
                    : action == "autostart"
                        ? autostart
                        : (action == "pinned"
                               ? pinned
                               : (action == "input_toggle"
                                      ? input_enabled
                                      : (action == "automatic_updates" ? automatic_updates
                                                                       : steam_shortcuts)));
                const double cy = b.y + b.h / 2.;
                color(c, on ? 0x1a9fff : 0x67707b);
                cairo_new_path(c);
                cairo_arc(c, content_right - padding - 54, cy, 18, 1.570796327, 4.712388980);
                cairo_arc(c, content_right - padding - 18, cy, 18, 4.712388980, 7.853981634);
                cairo_close_path(c);
                cairo_fill(c);
                color(c, 0xffffff);
                cairo_new_path(c);
                cairo_arc(c, on ? content_right - padding - 18 : content_right - padding - 54, cy,
                          13, 0, 6.283185307);
                cairo_fill(c);
                continue;
            }
            const bool selected = (action == "tracking_tab" && page == 0) ||
                                  (action == "controls_tab" && page == 1) ||
                                  (action == "app_tab" && page == 2);
            rectangle(c, b.x, b.y, b.w, b.h,
                      focus ? (pressed == b.action ? 0xb8bcbf : 0xffffff)
                            : (selected || b.kind != Navigation ? 0x3d4450 : 0x1b2029));
            if (selected && !focus)
                rectangle(c, b.x, b.y, 4, b.h, 0x1a9fff);
            std::string label = recovery && action == "acquire"    ? "Retry and restart VR"
                                : recovery && action == "recenter" ? "Uninstall"
                                                                   : b.label;
            if (action == "pointer_binding")
                label = capturing ? "Press a button..." : ButtonGlyphs::binding(pointer_label);
            const double size =
                b.kind == Step ? 30 : (action == "pointer_binding" || action == "quit" ? 21 : 24);
            cairo_save(c);
            cairo_rectangle(c, b.x + 12, b.y, b.w - 24, b.h);
            cairo_clip(c);
            centered(c, b.kind == Navigation ? b.x + 24 : b.x, b.y,
                     b.kind == Navigation ? b.w - 48 : b.w, b.h, label, size,
                     !available(b) ? 0x67707b : (focus ? 0x0e141b : 0xffffff),
                     b.kind == Navigation);
            cairo_restore(c);
        }
        cairo_destroy(c);
        return image;
    }
    void render(bool head_valid) {
        if (!icon_ready) {
            uint32_t w = 0, h = 0;
            if (!api->GetOverlayTextureSize(thumbnail, &w, &h) && w == 128 && h == 128) {
                check(api->SetOverlayFlag(panel, vr::VROverlayFlags_NoDashboardTab, !pinned),
                      "show taskbar icon");
                icon_ready = true;
            }
        }
        if (!head_valid || (!dirty && submitted))
            return;
        auto image = paint();
        upload(texture, panel, image);
        cairo_surface_destroy(image);
        if (show_requested) {
            api->ShowDashboard("org.controllerutils.frame");
            show_requested = false;
        }
        submitted = true;
        dirty = false;
    }
};
