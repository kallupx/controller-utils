#include "fake_openvr_overlay.h"
#include "steamvr/touchpad_cursor.h"
#include <cassert>
#include <cmath>
#include <iostream>

using namespace vr;
namespace {
struct Overlay final : ct_test::Overlay {
    float width = .0225f, depth = 1.f, scale = .8f, x = 0, tilt = 0;
    VROverlayHandle_t handle = 3;
    TrackedDeviceIndex_t primary = 1;
    TrackedDeviceIndex_t GetPrimaryDashboardDevice() override {
        return primary;
    }
    EVROverlayError FindOverlay(const char *key, VROverlayHandle_t *out) override {
        assert(!std::strcmp(key, "system.pointer"));
        *out = handle;
        return {};
    }
    EVROverlayError GetOverlayWidthInMeters(VROverlayHandle_t h, float *out) override {
        assert(h == handle);
        *out = width;
        return {};
    }
    EVROverlayError SetOverlayWidthInMeters(VROverlayHandle_t h, float value) override {
        assert(h == handle);
        width = value;
        return {};
    }
    EVROverlayError GetTransformForOverlayCoordinates(VROverlayHandle_t h,
                                                      ETrackingUniverseOrigin origin,
                                                      HmdVector2_t uv,
                                                      HmdMatrix34_t *out) override {
        assert(h == handle && origin == TrackingUniverseStanding);
        *out = {};
        const float edge = (uv.v[0] - .5f) * width * scale;
        out->m[0][3] = x + edge * std::cos(tilt);
        out->m[2][3] = -depth + edge * std::sin(tilt);
        return {};
    }
};
} // namespace
int main(int argc, char **argv) {
    if (argc == 2 && std::string(argv[1]) == "--guard-cursor-width") {
        std::string line;
        while (std::getline(std::cin, line)) {
        }
        return 0;
    }
    Overlay overlay;
    ct::TouchpadCursor cursor;
    const auto head = ct::rigid_pose({1, 0, 0, 0}, {});
    const float original = overlay.width;
    assert(cursor.update(overlay, head) && overlay.width < original);
    for (auto [depth, scale] :
         {std::pair{1.f, 2.69f}, {.93f, 2.66f}, {1.15f, 2.74f}, {.6f, 2.52f}, {2.f, 2.95f}}) {
        overlay.depth = depth;
        overlay.scale = scale;
        assert(cursor.update(overlay, head));
        assert(std::abs(overlay.width * scale / depth - .75f * original) < .0001f);
    }
    overlay.x = .45f;
    overlay.tilt = -.4f;
    assert(cursor.update(overlay, head));
    HmdMatrix34_t a{}, b{};
    overlay.GetTransformForOverlayCoordinates(overlay.handle, TrackingUniverseStanding, {{0, .5f}},
                                              &a);
    overlay.GetTransformForOverlayCoordinates(overlay.handle, TrackingUniverseStanding, {{1, .5f}},
                                              &b);
    const double angle = std::atan2(b.m[0][3], -b.m[2][3]) - std::atan2(a.m[0][3], -a.m[2][3]);
    assert(std::abs(angle - .75f * original) < .0001);
    const auto held_width = overlay.width;
    cursor.lift(overlay, 10);
    cursor.lift(overlay, 10.04);
    assert(overlay.width == held_width);
    overlay.scale = 1;
    cursor.lift(overlay, 10.06);
    assert(overlay.width == original);
    overlay.scale = 2.68;
    assert(cursor.update(overlay, head));
    cursor.lift(overlay, 11);
    assert(overlay.width < original);
    assert(cursor.update(overlay, head));
    cursor.lift(overlay, 12);
    assert(overlay.width < original);
    cursor.lift(overlay, 12.16);
    assert(overlay.width == original);
    assert(cursor.update(overlay, head));
    ++overlay.primary;
    cursor.lift(overlay, 13);
    assert(overlay.width == original);
    assert(cursor.update(overlay, head));
    overlay.width = .03f;
    cursor.release(overlay);
    assert(overlay.width == .03f);
    ct::CursorWidth recovery{overlay.handle, original, .006f, .007f};
    for (float interrupted : {.006f, .007f}) {
        overlay.width = interrupted;
        recovery.restore(overlay);
        assert(overlay.width == original);
    }
    ++overlay.handle;
    overlay.width = .007f;
    recovery.restore(overlay);
    assert(overlay.width == .007f);
}
