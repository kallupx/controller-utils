#include "touchpad_cursor.h"
#include "runtime.h"
#include <algorithm>
#include <cmath>
#include <iostream>

namespace ct {
namespace {
bool same_width(float a, float b) {
    return std::abs(a - b) < 1e-6f;
}
bool valid_width(float value) {
    return value > 0 && value < 1 && std::isfinite(value);
}
bool cursor_edges(vr::IVROverlay &overlay, vr::VROverlayHandle_t handle, Vector3 &left,
                  Vector3 &right) {
    vr::HmdMatrix34_t a{}, b{};
    if (overlay.GetTransformForOverlayCoordinates(handle, vr::TrackingUniverseStanding, {{0, .5f}},
                                                  &a) ||
        overlay.GetTransformForOverlayCoordinates(handle, vr::TrackingUniverseStanding, {{1, .5f}},
                                                  &b))
        return false;
    for (int i = 0; i < 3; ++i) {
        left[i] = a.m[i][3];
        right[i] = b.m[i][3];
    }
    return true;
}
} // namespace

bool CursorWidth::restore(vr::IVROverlay &overlay) const {
    if (!handle || !valid_width(original))
        return true;
    vr::VROverlayHandle_t current{};
    const auto error = overlay.FindOverlay("system.pointer", &current);
    if (error == vr::VROverlayError_UnknownOverlay || (!error && current != handle))
        return true;
    float value{};
    if (error || overlay.GetOverlayWidthInMeters(handle, &value))
        return false;
    return (!same_width(value, previous) && !same_width(value, applied)) ||
           same_width(value, original) || !overlay.SetOverlayWidthInMeters(handle, original);
}

int guard_cursor_width() {
    CursorWidth saved, next;
    while (std::cin >> next.handle >> next.original >> next.previous >> next.applied)
        saved = next;
    if (!saved.handle)
        return 0;
    // EOF also releases the cursor if the app is killed while a thumb is down.
    Runtime runtime;
    auto &overlay = *static_cast<vr::IVROverlay *>(runtime.interface(vr::IVROverlay_Version));
    for (int i = 0; i < 3; ++i) {
        if (saved.restore(overlay))
            return 0;
        sleep_for(.02);
    }
    return 1;
}

void TouchpadCursor::release(vr::IVROverlay &overlay) {
    if (!width.handle || !width.restore(overlay))
        return;
    width = {};
    touch_scale = lifted_at = 0;
    owner = vr::k_unTrackedDeviceIndexInvalid;
    if (guard) {
        try {
            guard->send("0 0 0 0\n");
        } catch (const std::exception &) {
            guard.reset();
        }
    }
}

void TouchpadCursor::lift(vr::IVROverlay &overlay, double now) {
    if (!width.handle)
        return;
    if (!lifted_at)
        lifted_at = now;
    // The compositor can draw the touchpad pose for several frames after the driver receives lift.
    if (now - lifted_at < .15 && owner == overlay.GetPrimaryDashboardDevice()) {
        Vector3 left{}, right{};
        float current{};
        if (!overlay.GetOverlayWidthInMeters(width.handle, &current) && valid_width(current) &&
            same_width(current, width.applied) &&
            cursor_edges(overlay, width.handle, left, right) &&
            distance(left, right) / current >= std::max(2., touch_scale - .35))
            return;
    }
    release(overlay);
}

bool TouchpadCursor::update(vr::IVROverlay &overlay, const RigidPose &head) {
    vr::VROverlayHandle_t handle{};
    float current{};
    if (overlay.FindOverlay("system.pointer", &handle) ||
        overlay.GetOverlayWidthInMeters(handle, &current) || !valid_width(current))
        return false;
    if (handle != width.handle || !same_width(width.applied, current)) {
        width = {handle, current, current, current};
        owner = overlay.GetPrimaryDashboardDevice();
        touch_scale = 0;
    }
    lifted_at = 0;

    // Start at the dashboard size measured with the ray behind the headset.
    float next = same_width(width.applied, width.original) ? width.original * .3f : width.applied;
    Vector3 left{}, right{};
    if (cursor_edges(overlay, handle, left, right)) {
        const auto eye = translation(head);
        const auto to_left = subtract(left, eye), to_right = subtract(right, eye);
        const double scale = distance(left, right) / current;
        const double angle = std::atan2(norm(cross(to_left, to_right)), dot(to_left, to_right));
        // The first compositor frame can still contain the ordinary controller ray.
        if (std::isfinite(scale) && scale >= 2 && angle > .0001 && angle < 1) {
            touch_scale = scale;
            next = float(current * (width.original * .75) / angle);
        }
    }
    if (!valid_width(next))
        return false;
    try {
        if (!guard)
            guard = std::make_unique<ChildProcess>(
                std::vector<std::string>{"/proc/self/exe", "--guard-cursor-width"}, "/");
        if (guard->exit_status()) {
            guard.reset();
            return false;
        }
        if (std::abs(current - next) <= current * .005f)
            return true;
        // Arm the guard before writing. An interrupted write can leave either width in use.
        guard->send(std::to_string(handle) + " " + std::to_string(width.original) + " " +
                    std::to_string(current) + " " + std::to_string(next) + "\n");
        if (overlay.SetOverlayWidthInMeters(handle, next))
            return false;
        width.previous = current;
        width.applied = next;
        return true;
    } catch (const std::exception &) {
        guard.reset();
        return false;
    }
}
} // namespace ct
