#pragma once
#include "tracked_input.h"
#include "steamvr/protocol.h"

namespace ct {
inline void publish_input(TrackedInput &tracked, const wire::AppInput &app_input, double time) {
    vr::DriverPose_t body{};
    body.qWorldFromDriverRotation.w = body.qDriverFromHeadRotation.w = 1;
    body.qRotation = {app_input.orientation[0], app_input.orientation[1], app_input.orientation[2],
                      app_input.orientation[3]};
    std::copy(app_input.position, app_input.position + 3, body.vecPosition);
    body.deviceIsConnected = true;
    body.poseIsValid = bool(app_input.flags & 2u);
    body.result =
        body.poseIsValid ? vr::TrackingResult_Running_OK : vr::TrackingResult_Uninitialized;
    ct::TrackedInputState controls;
    controls.time = app_input.time;
    controls.enabled = bool(app_input.flags & 1u);
    controls.activate = !(app_input.flags & 4u);
    controls.cursor_only = app_input.flags & wire::TouchpadCursor;
    controls.trigger = app_input.trigger;
    controls.grip = app_input.grip;
    controls.stick_x = app_input.stick_x;
    controls.stick_y = app_input.stick_y;
    controls.back = app_input.buttons & 1u;
    controls.bumper = app_input.buttons & 2u;
    controls.stick_touch = app_input.buttons & 4u;
    controls.stick_click = app_input.buttons & 8u;
    controls.pad_x = app_input.pad_x;
    controls.pad_y = app_input.pad_y;
    controls.pad_touch = app_input.buttons & 16u;
    controls.pad_click = app_input.buttons & 32u;
    controls.scroll_x = app_input.scroll_x;
    controls.scroll_y = app_input.scroll_y;
    controls.scroll_touch = app_input.buttons & 64u;
    controls.scroll_click = app_input.buttons & 128u;
    tracked.update(body, app_input.pose_time, controls, time);
}
} // namespace ct
