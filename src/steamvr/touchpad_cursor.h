#pragma once
#include "platform/linux/child.h"
#include "tracking/math.h"
#include <openvr.h>

namespace ct {
enum class MenuCursor { Inactive, Laser, Touchpad };

struct CursorWidth {
    vr::VROverlayHandle_t handle = 0;
    float original = 0, previous = 0, applied = 0;
    bool restore(vr::IVROverlay &overlay) const;
};

class TouchpadCursor {
    CursorWidth width;
    std::unique_ptr<ChildProcess> guard;
    vr::TrackedDeviceIndex_t owner = vr::k_unTrackedDeviceIndexInvalid;
    double touch_scale = 0, lifted_at = 0;

  public:
    bool update(vr::IVROverlay &overlay, const RigidPose &head);
    void lift(vr::IVROverlay &overlay, double now);
    void release(vr::IVROverlay &overlay);
};
int guard_cursor_width();
} // namespace ct
