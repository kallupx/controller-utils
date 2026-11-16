#pragma once
#include <openvr_driver.h>
#include "tip_cursor.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace ct {
struct TrackedInputState {
    double time = 0;
    bool enabled = false;
    float trigger = 0, grip = 0, stick_x = 0, stick_y = 0;
    float pad_x = 0, pad_y = 0, scroll_x = 0, scroll_y = 0;
    bool bumper = false, back = false, stick_click = false, stick_touch = false;
    bool pad_touch = false, pad_click = false, scroll_touch = false, scroll_click = false;
};

// Called from the provider's RunFrame thread. The provider owns this object until
// OpenVR deactivates it, input is disconnected unless both pose and controls are fresh.
class TrackedInput final : public vr::ITrackedDeviceServerDriver {
    vr::IVRServerDriverHost *host;
    vr::IVRDriverInput *input;
    vr::CVRPropertyHelpers properties;
    vr::TrackedDeviceIndex_t index = vr::k_unTrackedDeviceIndexInvalid;
    vr::PropertyContainerHandle_t container = vr::k_ulInvalidPropertyContainer;
    std::string serial;
    bool registered = false, connected = false, trigger_down = false, grip_down = false,
         armed = false, dirty = false;
    double last_input_time = 0;
    vr::VRInputComponentHandle_t trigger_value = 0, trigger_click = 0, trigger_touch = 0;
    vr::VRInputComponentHandle_t grip_value = 0, grip_click = 0, grip_touch = 0;
    vr::VRInputComponentHandle_t stick_x = 0, stick_y = 0, stick_click = 0, stick_touch = 0;
    vr::VRInputComponentHandle_t bumper = 0, back = 0, tip = 0, activate = 0;
    vr::VRInputComponentHandle_t pad_x = 0, pad_y = 0, pad_touch = 0, pad_click = 0;
    vr::VRInputComponentHandle_t scroll_x = 0, scroll_y = 0, scroll_touch = 0, scroll_click = 0;
    vr::DriverPose_t last_pose{};
    TipCursor cursor;
    PadMotionFilter scroll_motion;
    std::array<float, 2> scroll_position{};
    std::atomic<uint64_t> cursor_updates{0};
    std::atomic<unsigned> input_status{0};

    static vr::DriverPose_t disconnected_pose() {
        vr::DriverPose_t pose{};
        pose.qRotation.w = pose.qWorldFromDriverRotation.w = pose.qDriverFromHeadRotation.w = 1;
        pose.result = vr::TrackingResult_Uninitialized;
        return pose;
    }
    static bool fresh(double value, double now) {
        return std::isfinite(value) && std::isfinite(now) && value > 0 && now - value >= -.02 &&
               now - value <= .15;
    }
    static bool quaternion_valid(const vr::HmdQuaternion_t &q) {
        const double norm = q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z;
        return std::isfinite(norm) && std::abs(norm - 1) < .02;
    }
    static bool pose_valid(const vr::DriverPose_t &pose) {
        if (!pose.deviceIsConnected || !pose.poseIsValid ||
            pose.result != vr::TrackingResult_Running_OK || !quaternion_valid(pose.qRotation) ||
            !quaternion_valid(pose.qWorldFromDriverRotation) ||
            !quaternion_valid(pose.qDriverFromHeadRotation))
            return false;
        for (const double *vector :
             {pose.vecPosition, pose.vecVelocity, pose.vecAcceleration, pose.vecAngularVelocity,
              pose.vecAngularAcceleration, pose.vecWorldFromDriverTranslation,
              pose.vecDriverFromHeadTranslation})
            for (unsigned i = 0; i < 3; ++i)
                if (!std::isfinite(vector[i]))
                    return false;
        return true;
    }
    void boolean(const char *path, vr::VRInputComponentHandle_t &handle) {
        if (input->CreateBooleanComponent(container, path, &handle) != vr::VRInputError_None)
            throw std::runtime_error("cannot create native boolean input");
    }
    void scalar(const char *path, vr::VRInputComponentHandle_t &handle, bool two_sided = false) {
        if (input->CreateScalarComponent(container, path, &handle, vr::VRScalarType_Absolute,
                                         two_sided ? vr::VRScalarUnits_NormalizedTwoSided
                                                   : vr::VRScalarUnits_NormalizedOneSided) !=
            vr::VRInputError_None)
            throw std::runtime_error("cannot create native scalar input");
    }
    void property(vr::ETrackedPropertyError error) {
        if (error != vr::TrackedProp_Success)
            throw std::runtime_error("cannot set native input property");
    }
    bool set_boolean(vr::VRInputComponentHandle_t handle, bool value, double offset = 0) {
        return !handle ||
               input->UpdateBooleanComponent(handle, value, offset) == vr::VRInputError_None;
    }
    bool set_scalar(vr::VRInputComponentHandle_t handle, float value, double offset = 0) {
        return !handle ||
               input->UpdateScalarComponent(handle, value, offset) == vr::VRInputError_None;
    }
    static Matrix matrix(const vr::HmdQuaternion_t &q, const double *position) {
        const double s = 2 / (q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
        Matrix result = identity();
        result[0] = 1 - s * (q.y * q.y + q.z * q.z);
        result[1] = s * (q.x * q.y + q.w * q.z);
        result[2] = s * (q.x * q.z - q.w * q.y);
        result[4] = s * (q.x * q.y - q.w * q.z);
        result[5] = 1 - s * (q.x * q.x + q.z * q.z);
        result[6] = s * (q.y * q.z + q.w * q.x);
        result[8] = s * (q.x * q.z + q.w * q.y);
        result[9] = s * (q.y * q.z - q.w * q.x);
        result[10] = 1 - s * (q.x * q.x + q.y * q.y);
        for (int i = 0; i < 3; ++i)
            result[12 + i] = position[i];
        return result;
    }
    static Matrix world_body(const vr::DriverPose_t &body) {
        return multiply(
            matrix(body.qWorldFromDriverRotation, body.vecWorldFromDriverTranslation),
            multiply(matrix(body.qRotation, body.vecPosition),
                     matrix(body.qDriverFromHeadRotation, body.vecDriverFromHeadTranslation)));
    }
    bool set_tip(const Matrix &matrix) {
        vr::HmdMatrix34_t offset{};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                offset.m[r][c] = matrix[4 * c + r];
        return !tip || input->UpdatePoseComponent(tip, &offset, 0) == vr::VRInputError_None;
    }
    void release() {
        for (auto handle :
             {trigger_click, trigger_touch, grip_click, grip_touch, stick_click, stick_touch,
              bumper, back, pad_touch, pad_click, scroll_touch, scroll_click, activate})
            set_boolean(handle, false);
        for (auto handle :
             {trigger_value, grip_value, stick_x, stick_y, pad_x, pad_y, scroll_x, scroll_y})
            set_scalar(handle, 0);
        cursor.reset();
        scroll_motion.reset();
        scroll_position = {};
        set_tip(TipCursor::offset());
        trigger_down = grip_down = false;
    }

  public:
    TrackedInput(vr::IVRServerDriverHost *host_, vr::IVRDriverInput *input_,
                 vr::IVRProperties *properties_)
        : host(host_), input(input_), properties(properties_), last_pose(disconnected_pose()) {
        if (!host || !input || !properties_)
            throw std::invalid_argument("missing OpenVR input interface");
    }
    bool register_device(const std::string &actual_serial) {
        if (registered)
            return serial == "controllerutils-sc26-input-" + actual_serial;
        if (actual_serial.size() < 8 || actual_serial.size() > 19 ||
            !std::all_of(actual_serial.begin(), actual_serial.end(),
                         [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }))
            throw std::invalid_argument("invalid native input serial");
        serial = "controllerutils-sc26-input-" + actual_serial;
        registered =
            host->TrackedDeviceAdded(serial.c_str(), vr::TrackedDeviceClass_Controller, this);
        return registered;
    }
    vr::EVRInitError Activate(uint32_t object_id) override {
        index = object_id;
        container = properties.TrackedDeviceToPropertyContainer(index);
        try {
            property(properties.SetStringProperty(container, vr::Prop_SerialNumber_String,
                                                  serial.c_str()));
            property(properties.SetStringProperty(container, vr::Prop_TrackingSystemName_String,
                                                  "controllerutils_native"));
            property(properties.SetStringProperty(container, vr::Prop_ModelNumber_String,
                                                  "Steam Controller (2026) tracking"));
            // Steam replaces an empty name with generic_controller, this valid zero-area mesh draws
            // nothing.
            property(properties.SetStringProperty(container, vr::Prop_RenderModelName_String,
                                                  "{controllerutils_native}sc26_input"));
            property(properties.SetStringProperty(container, vr::Prop_ManufacturerName_String,
                                                  "Controller Utils"));
            property(properties.SetStringProperty(container, vr::Prop_ControllerType_String,
                                                  "controllerutils_sc26"));
            property(
                properties.SetStringProperty(container, vr::Prop_InputProfilePath_String,
                                             "{controllerutils_native}/input/sc26_profile.json"));
            property(properties.SetInt32Property(container, vr::Prop_ControllerRoleHint_Int32,
                                                 vr::TrackedControllerRole_OptOut));
            property(properties.SetInt32Property(
                container, vr::Prop_ControllerHandSelectionPriority_Int32, -1000));
            boolean("/input/trigger/click", trigger_click);
            boolean("/input/trigger/touch", trigger_touch);
            scalar("/input/trigger/value", trigger_value);
            boolean("/input/grip/click", grip_click);
            boolean("/input/grip/touch", grip_touch);
            scalar("/input/grip/value", grip_value);
            scalar("/input/thumbstick/x", stick_x, true);
            scalar("/input/thumbstick/y", stick_y, true);
            boolean("/input/thumbstick/click", stick_click);
            boolean("/input/thumbstick/touch", stick_touch);
            scalar("/input/cursorpad/x", pad_x, true);
            scalar("/input/cursorpad/y", pad_y, true);
            boolean("/input/cursorpad/touch", pad_touch);
            boolean("/input/cursorpad/click", pad_click);
            scalar("/input/trackpad/x", scroll_x, true);
            scalar("/input/trackpad/y", scroll_y, true);
            boolean("/input/trackpad/touch", scroll_touch);
            boolean("/input/trackpad/click", scroll_click);
            boolean("/input/bumper/click", bumper);
            boolean("/input/b/click", back);
            boolean("/input/activate/click", activate);
            // Steam also synthesizes /pose/tip from render-model components, keep the moving cursor
            // separate.
            if (input->CreatePoseComponent(container, "/pose/cursor", &tip) !=
                vr::VRInputError_None)
                throw std::runtime_error("cannot create native pointer pose");
            // Match the existing gamepad ray: from its top edge along body -Z.
            if (!set_tip(TipCursor::offset()))
                throw std::runtime_error("cannot set native pointer pose");
            release();
            last_pose = disconnected_pose();
            host->TrackedDevicePoseUpdated(index, last_pose, sizeof(last_pose));
            return vr::VRInitError_None;
        } catch (...) {
            release();
            index = vr::k_unTrackedDeviceIndexInvalid;
            container = vr::k_ulInvalidPropertyContainer;
            return vr::VRInitError_Driver_Failed;
        }
    }
    void update(const vr::DriverPose_t &body, double pose_time, const TrackedInputState &state,
                double now) {
        if (index == vr::k_unTrackedDeviceIndexInvalid)
            return;
        const bool values_valid = std::isfinite(state.trigger) && state.trigger >= 0 &&
                                  state.trigger <= 1 && std::isfinite(state.grip) &&
                                  state.grip >= 0 && state.grip <= 1 &&
                                  std::isfinite(state.stick_x) && std::abs(state.stick_x) <= 1 &&
                                  std::isfinite(state.stick_y) && std::abs(state.stick_y) <= 1 &&
                                  std::isfinite(state.pad_x) && std::abs(state.pad_x) <= 1 &&
                                  std::isfinite(state.pad_y) && std::abs(state.pad_y) <= 1 &&
                                  std::isfinite(state.scroll_x) && std::abs(state.scroll_x) <= 1 &&
                                  std::isfinite(state.scroll_y) && std::abs(state.scroll_y) <= 1;
        if (!state.enabled || !fresh(state.time, now) || !fresh(pose_time, now) ||
            state.time < last_input_time || !values_valid || !pose_valid(body)) {
            yield();
            return;
        }
        if (!connected) {
            dirty = true;
            if (properties.SetInt32Property(container,
                                            vr::Prop_ControllerHandSelectionPriority_Int32,
                                            1000) != vr::TrackedProp_Success) {
                yield();
                return;
            }
            if (properties.SetInt32Property(container, vr::Prop_ControllerRoleHint_Int32,
                                            vr::TrackedControllerRole_RightHand) !=
                vr::TrackedProp_Success) {
                yield();
                return;
            }
            connected = true;
        }
        if (state.time != last_input_time && !armed) {
            armed = state.trigger < .1f && state.grip < .1f &&
                    std::hypot(state.stick_x, state.stick_y) < .06f && !state.bumper &&
                    !state.back && !state.stick_click && !state.pad_click && !state.scroll_click;
        }
        try {
            // Recompute the local offset even for repeated HID samples: native
            // root pose updates must not move a cursor held in world space.
            const bool held = armed && (state.trigger >= (trigger_down ? .45f : .5f) ||
                                        state.grip >= (grip_down ? .45f : .5f) || state.pad_click ||
                                        state.scroll_click || state.stick_click || state.bumper);
            if (!set_tip(cursor.update(world_body(body), state.pad_x, state.pad_y, state.time,
                                       armed && state.pad_touch, held))) {
                yield();
                return;
            }
            input_status.store(1u | (armed ? 2u : 0u) | (state.pad_touch ? 4u : 0u) |
                                   (cursor.active ? 8u : 0u),
                               std::memory_order_relaxed);
            if (cursor.active)
                cursor_updates.fetch_add(1, std::memory_order_relaxed);
        } catch (const std::exception &) {
            yield();
            return;
        }
        last_pose = body;
        // The UI publishes its predicted body pose at state.time, pose_time is
        // the original native observation used only for the freshness lease.
        last_pose.poseTimeOffset = state.time - now;
        host->TrackedDevicePoseUpdated(index, last_pose, sizeof(last_pose));
        if (state.time == last_input_time)
            return;
        last_input_time = state.time;
        if (!armed)
            return;
        if (state.scroll_touch && !scroll_motion.active)
            scroll_position = {state.scroll_x, state.scroll_y};
        const auto scroll_delta =
            scroll_motion.update(state.scroll_x, state.scroll_y, state.time, state.scroll_touch);
        for (int i = 0; i < 2; ++i)
            scroll_position[i] = state.scroll_touch ? scroll_position[i] + scroll_delta[i] : 0;
        trigger_down = state.trigger >= (trigger_down ? .45f : .5f);
        grip_down = state.grip >= (grip_down ? .45f : .5f);
        const double offset = state.time - now;
        bool ok = true;
        // Do not short circuit: all components must receive releases even if one update fails.
        ok &= set_boolean(activate, true, offset);
        ok &= set_scalar(trigger_value, state.trigger, offset);
        ok &= set_boolean(trigger_click, trigger_down, offset);
        ok &= set_boolean(trigger_touch, state.trigger > .05f, offset);
        ok &= set_scalar(grip_value, state.grip, offset);
        ok &= set_boolean(grip_click, grip_down, offset);
        ok &= set_boolean(grip_touch, state.grip > .05f, offset);
        ok &= set_scalar(stick_x, state.stick_x, offset);
        ok &= set_scalar(stick_y, state.stick_y, offset);
        ok &= set_boolean(stick_click, state.stick_click, offset);
        ok &= set_boolean(stick_touch, state.stick_touch, offset);
        ok &= set_scalar(pad_x, state.pad_touch ? state.pad_x : 0, offset);
        ok &= set_scalar(pad_y, state.pad_touch ? state.pad_y : 0, offset);
        ok &= set_boolean(pad_touch, state.pad_touch, offset);
        ok &= set_boolean(pad_click, state.pad_click, offset);
        ok &= set_scalar(scroll_x, scroll_position[0], offset);
        ok &= set_scalar(scroll_y, scroll_position[1], offset);
        ok &= set_boolean(scroll_touch, state.scroll_touch, offset);
        ok &= set_boolean(scroll_click, state.scroll_click, offset);
        ok &= set_boolean(bumper, state.bumper, offset);
        ok &= set_boolean(back, state.back, offset);
        if (!ok)
            yield();
    }
    void yield() {
        if (index == vr::k_unTrackedDeviceIndexInvalid || !dirty)
            return;
        release();
        connected = false;
        armed = false;
        last_input_time = 0;
        input_status.store(0, std::memory_order_relaxed);
        dirty = properties.SetInt32Property(container, vr::Prop_ControllerRoleHint_Int32,
                                            vr::TrackedControllerRole_OptOut) !=
                vr::TrackedProp_Success;
        last_pose = disconnected_pose();
        host->TrackedDevicePoseUpdated(index, last_pose, sizeof(last_pose));
    }
    bool active() const {
        return connected;
    }
    uint32_t device_index() const {
        return index;
    }
    void Deactivate() override {
        yield();
        index = vr::k_unTrackedDeviceIndexInvalid;
        container = vr::k_ulInvalidPropertyContainer;
    }
    void EnterStandby() override {
        yield();
    }
    void *GetComponent(const char *) override {
        return nullptr;
    }
    void DebugRequest(const char *request, char *response, uint32_t size) override {
        if (!response || !size)
            return;
        response[0] = 0;
        if (request && std::string(request) == "cursor_status")
            std::snprintf(
                response, size, "{\"flags\":%u,\"cursor_updates\":%llu}",
                input_status.load(std::memory_order_relaxed),
                static_cast<unsigned long long>(cursor_updates.load(std::memory_order_relaxed)));
    }
    vr::DriverPose_t GetPose() override {
        return last_pose;
    }
};
} // namespace ct
