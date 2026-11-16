#pragma once

struct AlignmentOverlay {
    Dashboard &dashboard;
    vr::IVROverlay *api;
    vr::VROverlayHandle_t panel = 0;
    GLuint texture = 0;
    bool active = false, dirty = true;
    double timestamp = 0, progress = -1, last_paint = 0;
    std::string state, message;
    explicit AlignmentOverlay(Dashboard &owner) : dashboard(owner), api(owner.api) {}
    ~AlignmentOverlay() {
        if (panel) {
            api->HideOverlay(panel);
            api->ClearOverlayTexture(panel);
            api->DestroyOverlay(panel);
        }
        glDeleteTextures(1, &texture);
    }
    bool accept(const ct::ui::Command &command) {
        const auto next = std::get_if<ct::ui::Feedback>(&command);
        if (!next)
            return false;
        if (next->active) {
            const auto instruction = ButtonGlyphs::chords(next->message);
            dirty |= state != next->state || message != instruction || progress != next->progress;
            state = next->state;
            message = instruction;
            timestamp = next->time;
            progress = next->progress;
        }
        active = next->active;
        return true;
    }
    bool visible(double now) const {
        return active && now - timestamp >= -.02 && now - timestamp <= .5;
    }
    cairo_surface_t *paint(double now = 0) const {
        auto image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1100, 240);
        auto c = cairo_create(image);
        Dashboard::color(c, 0x0e141b);
        cairo_paint(c);
        const bool ready = state == "ready", error = state == "error";
        const uint32_t accent = ready ? 0x59bf40 : (error ? 0xe9b04c : 0x1a9fff);
        Dashboard::rectangle(c, 0, 0, 1100, 4, accent);
        dashboard.draw_icon(c, 34, 62, 98);
        Dashboard::color(c, 0x3d4450);
        cairo_set_line_width(c, 5);
        cairo_arc(c, 83, 111, 65, 0, 6.283185307);
        cairo_stroke(c);
        Dashboard::color(c, accent);
        const double start = progress < 0 ? now * 3 : -1.570796327;
        cairo_arc(c, 83, 111, 65, start, start + (progress < 0 ? 4.4 : 6.283185307 * progress));
        cairo_stroke(c);
        const std::string title = ready                   ? "Gamepad ready"
                                  : error                 ? "Gamepad tracking needs attention"
                                  : state == "waiting"    ? "Turn on your gamepad"
                                  : state == "connecting" ? "Connecting gamepad"
                                  : state == "imu"        ? "Hold your gamepad still"
                                  : state == "palm"       ? "Finding your gamepad"
                                  : state == "realign"    ? "Set gamepad heading"
                                  : state == "restart"    ? "Keep holding to restart"
                                                          : "Aligning your gamepad";
        Dashboard::text(c, 184, 39, "STEAM CONTROLLER", 18, 0x8b929a);
        Dashboard::text(c, 184, 84, title, 32);
        Dashboard::wrapped(c, 184, 126, message, 875, 2, 23, 0xb8bcbf);
        const int step = state == "ready"         ? 4
                         : state == "stabilizing" ? 3
                         : state == "palm"        ? 2
                         : state == "imu"         ? 1
                                                  : 0;
        const char *labels[] = {"Connect", "Motion", "Palm + LED", "Aligned"};
        for (int i = 0; i < 4; ++i) {
            Dashboard::color(c, i < step ? accent : 0x3d4450);
            cairo_arc(c, 190 + i * 220, 210, 4, 0, 6.283185307);
            cairo_fill(c);
            Dashboard::text(c, 204 + i * 220, 217, labels[i], 18, i < step ? 0xdcdedf : 0x8b929a);
        }
        cairo_destroy(c);
        return image;
    }
    void render(bool awake, double now) {
        if (!awake || !visible(now)) {
            if (panel)
                api->HideOverlay(panel);
            return;
        }
        if (!panel) {
            check(
                api->CreateOverlay("controllerutils.progress", "Gamepad tracking progress", &panel),
                "gamepad progress panel");
            check(api->SetOverlayFlag(panel, vr::VROverlayFlags_VisibleInDashboard, true),
                  "progress visibility");
            check(api->SetOverlayWidthInMeters(panel, .65f), "progress size");
            check(api->SetOverlaySortOrder(panel, 101), "progress order");
            const vr::VRTextureBounds_t bounds{0, 1, 1, 0};
            check(api->SetOverlayTextureBounds(panel, &bounds), "progress texture bounds");
            const vr::HmdMatrix34_t transform{{{1, 0, 0, 0}, {0, 1, 0, -.24f}, {0, 0, 1, -.85f}}};
            check(api->SetOverlayTransformTrackedDeviceRelative(panel, 0, &transform),
                  "progress position");
        }
        if (dirty || (progress < 0 && now - last_paint >= .05)) {
            auto image = paint(now);
            dashboard.upload(texture, panel, image);
            cairo_surface_destroy(image);
            dirty = false;
            last_paint = now;
        }
        check(api->ShowOverlay(panel), "gamepad progress show");
    }
};
