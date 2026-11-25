#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include "ui/button_glyphs.h"
#include "ui/body_pose.h"
#include "interaction/pointer_math.h"
#include "ui/gaze.h"
#include "ui/vr.h"
#include "ui/dashboard.h"
#include "ui/alignment.h"
#include "ui/pointer.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "fake_openvr_interfaces.h"
#pragma GCC diagnostic pop

namespace {
template <class Widget> bool accept_text(Widget &widget, const std::string &text) {
    const auto command = ct::ui::command(ct::json::parse(text).get());
    return command && widget.accept(*command);
}
void discard(ct::ui::Event) {}
struct PointerSystem : ct_test::System {
    bool connected = true;
    vr::ETrackedDeviceClass device_class = vr::TrackedDeviceClass_Controller;
    std::string type = "frame_controller";
    vr::ETrackedPropertyError error = vr::TrackedProp_Success;
    bool IsTrackedDeviceConnected(uint32_t device) override {
        return connected && device >= 1 && device <= 3;
    }
    vr::ETrackedDeviceClass GetTrackedDeviceClass(uint32_t) override {
        return device_class;
    }
    uint32_t GetStringTrackedDeviceProperty(uint32_t device, vr::ETrackedDeviceProperty property,
                                            char *value, uint32_t size,
                                            vr::ETrackedPropertyError *result) override {
        const auto name = device == 2 ? "controllerutils_sc26" : type;
        assert(property == vr::Prop_ControllerType_String && size > name.size());
        *result = error;
        std::memcpy(value, name.c_str(), name.size() + 1);
        return uint32_t(name.size() + 1);
    }
};
struct GripInput {
    using Input = ct::steamvr::ControllerInput;
    inline static uint64_t down = 0;
    inline static int result = 0, error = 0;
    inline static uint32_t tag = 33, size = 72, type = 1, visibility = 3;
    inline static uint8_t invalid_boolean = 0;
    inline static unsigned reads = 0;
    static uint64_t container(uint32_t device) {
        assert(device == 1 || device == 3);
        return device + 100;
    }
    static int read(uint64_t root, Input::PathRead *batch, uint32_t count) {
        assert(count == 1 && root >= 101 && root <= 103);
        assert(batch->size == sizeof(Input::InputValue) &&
               std::string(batch->name) == "/input/grip/click");
        ++reads;
        Input::InputValue value{};
        value.type = type;
        value.visibility = visibility;
        value.value[0] = invalid_boolean ? invalid_boolean : bool(down & (1ull << (root - 100)));
        std::memcpy(batch->buffer, &value, sizeof(value));
        batch->required = size;
        batch->tag = tag;
        batch->error = error;
        return result;
    }
};
void pointer_handoff() {
    PointerSystem system;
    Vr runtime;
    runtime.system = &system;
    runtime.controller_input = {GripInput::read, GripInput::container};
    std::vector<bool> events;
    Dashboard dashboard(nullptr, [&](const ct::ui::Event &event) {
        if (const auto change = std::get_if<ct::ui::PointerEnabled>(&event))
            events.push_back(change->enabled);
    });
    ControllerPointer pointer(runtime, dashboard);
    pointer.input_time = 100;
    pointer.toggle(100, 0x80);
    const auto poll = [&] { pointer.grips(runtime.controller_input.grips(system)); };
    for (uint32_t device : {1u, 3u}) {
        pointer.set_enabled(true);
        events.clear();
        poll();
        assert(GripInput::reads && pointer.grip_known == ((1ull << 1) | (1ull << 3)));
        GripInput::down = 1ull << device;
        poll();
        poll();
        assert(!pointer.enabled && !dashboard.input_enabled && events == std::vector<bool>{false});
        pointer.buttons = 0x80;
        pointer.input_time += .01;
        assert(pointer.toggle(pointer.input_time, 0x80));
        assert(events == (std::vector<bool>{false, true}));
        poll();
        poll();
        assert(pointer.enabled);
        GripInput::down = 0;
        poll();
        assert(pointer.enabled);
        pointer.buttons = 0;
        pointer.input_time += .01;
        pointer.toggle(pointer.input_time, 0x80);
    }
    const auto rejected = [&] {
        GripInput::down = 1ull << 1;
        poll();
        assert(pointer.enabled && pointer.grip_known == 0);
        GripInput::down = 0;
    };
    for (auto field :
         {&GripInput::tag, &GripInput::size, &GripInput::type, &GripInput::visibility}) {
        const auto saved = *field;
        *field = saved + 1;
        rejected();
        *field = saved;
    }
    GripInput::invalid_boolean = 2;
    rejected();
    GripInput::invalid_boolean = 0;
    GripInput::result = 1;
    rejected();
    GripInput::result = 0;
    GripInput::error = 1;
    rejected();
    GripInput::error = 0;
    system.type = "";
    rejected();
    system.type = "frame_controller";
    system.error = vr::TrackedProp_UnknownProperty;
    rejected();
    system.error = vr::TrackedProp_Success;
    system.device_class = vr::TrackedDeviceClass_HMD;
    rejected();
    system.device_class = vr::TrackedDeviceClass_Controller;
    system.connected = false;
    rejected();
    system.connected = true;
    GripInput::down = 1ull << 1;
    poll();
    assert(pointer.enabled);
    GripInput::down = 0;
    poll();
    GripInput::down = 1ull << 1;
    poll();
    assert(!pointer.enabled);
}

} // namespace
int main() {
    struct FontCleanup {
        ~FontCleanup() {
            ButtonGlyphs::instance().cache.clear();
            cairo_debug_reset_static_data();
            FcFini();
        }
    } fonts;
    try {
        pointer_handoff();
        Dashboard dashboard(nullptr, discard);
        if (!dashboard.hit(320, 190) || std::string(dashboard.hit(320, 190)) != "toggle" ||
            dashboard.hit(20, 20) || dashboard.hit(270, 190))
            throw std::runtime_error("dashboard hit test failed");
        vr::VREvent_t event{};
        event.eventType = vr::VREvent_MouseMove;
        event.data.mouse.x = 320;
        event.data.mouse.y = 190;
        dashboard.pointer(event);
        event.eventType = vr::VREvent_MouseButtonDown;
        event.data.mouse = {};
        event.data.mouse.button = vr::VRMouseButton_Left;
        dashboard.pointer(event);
        event.eventType = vr::VREvent_MouseButtonUp;
        const auto action = dashboard.pointer(event);
        if (!action || std::string(action) != "toggle" || dashboard.pointer(event))
            throw std::runtime_error("dashboard pointer click failed");
        event.eventType = vr::VREvent_MouseButtonDown;
        dashboard.pointer(event);
        event.eventType = vr::VREvent_FocusLeave;
        dashboard.pointer(event);
        event.eventType = vr::VREvent_MouseButtonUp;
        if (dashboard.pointer(event) || dashboard.hovered)
            throw std::runtime_error("dashboard pointer leave failed");
        accept_text(dashboard, R"({"op":"app_info","version":"0.1.0"})");
        const auto check_layout = [&](cairo_surface_t *surface) {
            cairo_surface_flush(surface);
            const auto data = cairo_image_surface_get_data(surface);
            const auto stride = cairo_image_surface_get_stride(surface);
            const auto check_text = [&](int x, int y, int w, int h) {
                uint32_t background = 0;
                std::memcpy(&background, data + y * stride + x * 4, 4);
                int top = y + h, bottom = y - 1;
                for (int yy = y; yy < y + h; ++yy)
                    for (int xx = x; xx < x + w; ++xx) {
                        uint32_t pixel = 0;
                        std::memcpy(&pixel, data + yy * stride + xx * 4, 4);
                        if ((pixel & 0xffffffu) != (background & 0xffffffu)) {
                            top = std::min(top, yy);
                            bottom = std::max(bottom, yy);
                        }
                    }
                if (bottom < top || std::abs((top + bottom + 1) / 2. - (y + h / 2.)) > 1)
                    throw std::runtime_error("settings text is not vertically centered at " +
                                             std::to_string(x) + "," + std::to_string(y));
            };
            for (const auto &b : dashboard.buttons)
                if ((b.page == -1 || b.page == dashboard.page) &&
                    std::string(b.action) != "apply_update") {
                    if (b.kind == Dashboard::Switch)
                        check_text(b.x + 24, b.y, b.w - 144, b.h);
                    else
                        check_text(b.x + 12, b.y, b.w - 24, b.h);
                }
            if (dashboard.page == 2)
                check_text(304, 458, 580, 104);
        };
        {
            Vr unused;
            ControllerPointer pointer(unused, dashboard);
            accept_text(pointer, R"({"op":"keyboard_typing","active":true})");
            if (!pointer.keyboard_typing)
                throw std::runtime_error("keyboard mode was discarded");
            bool bad_keyboard = false;
            try {
                accept_text(pointer, R"({"op":"keyboard_typing","active":1})");
            } catch (const std::exception &) {
                bad_keyboard = true;
            }
            if (!bad_keyboard || !pointer.keyboard_typing)
                throw std::runtime_error("invalid keyboard mode changed state");
            for (bool enabled : {false, true}) {
                pointer.enabled = enabled;
                dashboard.hovered = dashboard.pressed = "toggle";
                dashboard.dirty = false;
                accept_text(pointer, R"({"op":"input_off"})");
                if (pointer.enabled || dashboard.hovered || dashboard.pressed || !dashboard.dirty)
                    throw std::runtime_error(
                        "tracking failure left dashboard selection or press active");
            }
            const auto input = [&](double time, uint32_t buttons) {
                return accept_text(
                    pointer, "{\"op\":\"controls\",\"time\":" + std::to_string(time) +
                                 ",\"buttons\":" + std::to_string(buttons) +
                                 ",\"triggers\":[0,0],\"left_pad\":[0,0,0],\"right_pad\":[0,0,"
                                 "0],\"left_stick\":[0,0],\"right_stick\":[0,0]}");
            };
            input(100, 0);
            if (pointer.toggle(100, 0x80))
                throw std::runtime_error("pointer enabled without a button press");
            input(100.01, 0x80);
            if (!pointer.toggle(100.01, 0x80) || !pointer.toggle(100.01, 0x80))
                throw std::runtime_error("pointer toggle did not latch");
            input(100.02, 0x80);
            if (!pointer.toggle(100.02, 0x80))
                throw std::runtime_error("held pointer button toggled twice");
            input(100.03, 0);
            pointer.toggle(100.03, 0x80);
            input(100.04, 0x80);
            if (pointer.toggle(100.04, 0x80) || pointer.toggle(100.3, 0x80))
                throw std::runtime_error("pointer disable or stale-input guard failed");
            bool rejected = false;
            try {
                accept_text(pointer, "{\"op\":\"controls\",\"time\":101,\"buttons\":1e100}");
            } catch (const std::exception &) {
                rejected = true;
            }
            if (!rejected)
                throw std::runtime_error("invalid pointer input accepted");
        }
        auto image = dashboard.paint();
        check_layout(image);
        const auto rc = cairo_surface_write_to_png(image, "build/dashboard-check.png");
        cairo_surface_destroy(image);
        if (rc != CAIRO_STATUS_SUCCESS)
            throw std::runtime_error("dashboard image failed");
        dashboard.enabled = true;
        if (dashboard.hit(320, 310) || dashboard.hit(900, 310))
            throw std::runtime_error("tracking actions enabled without gamepad");
        dashboard.gamepad_connected = true;
        if (!dashboard.hit(320, 310) || !dashboard.hit(900, 310) ||
            std::string(dashboard.hit(900, 310)) != "acquire")
            throw std::runtime_error("connected tracking actions unavailable");
        if (dashboard.hit(320, 345) || dashboard.hit(900, 345))
            throw std::runtime_error("shortcut note became a button");
        dashboard.status = "Tracking starts automatically when your gamepad connects.";
        image = dashboard.paint();
        check_layout(image);
        const auto enabled_rc =
            cairo_surface_write_to_png(image, "build/dashboard-tracking-enabled-check.png");
        cairo_surface_destroy(image);
        if (enabled_rc != CAIRO_STATUS_SUCCESS)
            throw std::runtime_error("enabled tracking image failed");
        dashboard.page = 1;
        dashboard.input_enabled = true;
        dashboard.input_state = "Laser - swipe right pad for cursor";
        image = dashboard.paint();
        check_layout(image);
        const auto controls_rc =
            cairo_surface_write_to_png(image, "build/dashboard-controls-check.png");
        cairo_surface_destroy(image);
        if (controls_rc != CAIRO_STATUS_SUCCESS)
            throw std::runtime_error("controls image failed");
        dashboard.page = 2;
        dashboard.update = ct::ui::SoftwareUpdate{"Check for a new release.", true, false, false};
        assert(std::string(dashboard.hit(320, 610)) == "debug_logging");
        assert(!dashboard.hit(1050, 685));
        accept_text(
            dashboard,
            R"({"op":"app_info","version":"0.1.0","debug":{"recording":true,"available":true,"busy":true,"message":"Saving to Downloads..."}})");
        assert(dashboard.debug_recording && !dashboard.hit(320, 610) && !dashboard.hit(1050, 685));
        accept_text(
            dashboard,
            R"({"op":"app_info","version":"0.1.0","debug":{"recording":false,"available":true,"busy":false,"message":"Saved in Downloads."}})");
        assert(!dashboard.debug_recording &&
               std::string(dashboard.hit(1050, 685)) == "save_debug_log");
        assert(std::string(dashboard.hit(320, 394)) == "automatic_updates");
        assert(std::string(dashboard.hit(1000, 500)) == "check_updates");
        dashboard.update->ready = true;
        assert(std::string(dashboard.hit(1000, 500)) == "apply_update");
        dashboard.update->busy = true;
        assert(!dashboard.hit(1000, 500));
        dashboard.update->ready = dashboard.update->busy = false;
        image = dashboard.paint();
        check_layout(image);
        cairo_surface_flush(image);
        const auto app_pixels = cairo_image_surface_get_data(image);
        const auto app_stride = cairo_image_surface_get_stride(image);
        for (int y = 0; y < Dashboard::height; ++y)
            for (int x = 0; x < Dashboard::width; ++x) {
                uint32_t pixel = 0;
                std::memcpy(&pixel, app_pixels + y * app_stride + x * 4, 4);
                if ((pixel & 0xffffffu) == 0x1a9fffu && !dashboard.hit(float(x), float(y)))
                    throw std::runtime_error("settings accent escaped its control");
            }
        const auto app_rc = cairo_surface_write_to_png(image, "build/dashboard-app-check.png");
        cairo_surface_destroy(image);
        if (app_rc != CAIRO_STATUS_SUCCESS)
            throw std::runtime_error("app settings image failed");
        accept_text(dashboard, R"({"op":"app_info","version":"0.1.0"})");
        image = dashboard.paint();
        const auto offline_rc =
            cairo_surface_write_to_png(image, "build/dashboard-app-preview.png");
        cairo_surface_destroy(image);
        if (offline_rc != CAIRO_STATUS_SUCCESS)
            throw std::runtime_error("offline settings image failed");
        dashboard.hovered = dashboard.hit(1080, 180);
        image = dashboard.paint();
        check_layout(image);
        cairo_surface_destroy(image);
        dashboard.hovered = nullptr;
        AlignmentOverlay alignment(dashboard);
        accept_text(
            alignment,
            R"({"op":"tracking_feedback","active":true,"time":10,"state":"palm","message":"Hold the gamepad in front of Frame, buttons up, with its top edge roughly away from you.","progress":null})");
        if (!alignment.visible(10.1) || alignment.visible(10.6) || alignment.visible(9.9))
            throw std::runtime_error("alignment freshness failed");
        image = alignment.paint();
        const auto alignment_rc = cairo_surface_write_to_png(image, "build/alignment-check.png");
        cairo_surface_destroy(image);
        accept_text(alignment, R"({"op":"tracking_feedback","active":false})");
        if (alignment.visible(10.1) || alignment_rc != CAIRO_STATUS_SUCCESS)
            throw std::runtime_error("alignment check failed");
        for (
            const auto *command :
            {R"({"op":"tracking_feedback","active":true,"time":10,"state":"unknown","message":"test","progress":null})",
             R"({"op":"tracking_feedback","active":true,"time":10,"state":"palm","message":"test","progress":1.1})",
             R"({"op":"tracking_feedback","active":true,"time":10,"state":"palm","message":"test"})"}) {
            bool rejected = false;
            try {
                accept_text(alignment, command);
            } catch (const std::exception &) {
                rejected = true;
            }
            if (!rejected)
                throw std::runtime_error("invalid gamepad feedback was accepted");
        }
        accept_text(
            alignment,
            R"({"op":"tracking_feedback","active":true,"time":10,"state":"realign","message":"Look ahead and point the gamepad the same way. Hold the gamepad still for a moment.","progress":null})");
        image = alignment.paint();
        const auto heading_rc =
            cairo_surface_write_to_png(image, "build/alignment-heading-check.png");
        cairo_surface_destroy(image);
        if (heading_rc != CAIRO_STATUS_SUCCESS)
            throw std::runtime_error("heading feedback image failed");
        accept_text(
            alignment,
            R"({"op":"tracking_feedback","active":true,"time":10,"state":"restart","message":"Release View + Menu for heading correction, or keep holding to 5 seconds to restart tracking.","progress":0.5})");
        image = alignment.paint();
        const auto restart_rc =
            cairo_surface_write_to_png(image, "build/alignment-restart-check.png");
        cairo_surface_destroy(image);
        if (restart_rc != CAIRO_STATUS_SUCCESS)
            throw std::runtime_error("restart feedback image failed");
        accept_text(
            alignment,
            R"({"op":"tracking_feedback","active":true,"time":10,"state":"ready","message":"Gamepad aligned. Point where you look and hold View + Menu for one second to set heading.","progress":1})");
        image = alignment.paint();
        const auto ready_rc = cairo_surface_write_to_png(image, "build/alignment-ready-check.png");
        cairo_surface_destroy(image);
        if (ready_rc != CAIRO_STATUS_SUCCESS)
            throw std::runtime_error("ready feedback image failed");
        std::cout << "{\"dashboard_check\":true}" << std::endl;
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
