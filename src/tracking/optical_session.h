#pragma once
#include "session_view.h"
#include "calibration.h"
#include "position.h"
#include "controller/device.h"
#include "platform/linux/child.h"
#include "steamvr/runtime.h"
#include "steamvr/steam_ui.h"
#include "steamvr/optical_socket.h"

namespace ct {
class OpticalSession final : public TrackingSessionView {
    RuntimeView &runtime;
    std::shared_ptr<ControllerDevice> device;
    TrackingCalibration cal;
    std::function<void(const TrackingCalibration &)> save_calibration;
    StationaryGyroBias gyro_calibration;
    bool restart_anchor = false;
    Settings settings;
    OrientationFilter imu;
    InertialPosition position;
    PoseHistory heads;
    std::optional<Quaternion> alignment;
    std::optional<BodyPoseMessage> last_valid_pose;
    std::optional<Vector3> last_up;
    std::optional<TrackingResume> resume;
    double resume_seen = 0, resume_still_since = 0, resume_motion_since = 0;
    struct InputState : TrackingInputState {
        std::optional<InputControls> input;
        std::optional<double> received, raw_received, filter_confirmed;
        std::optional<int> filter_index;
        uint32_t buttons = 0;
        bool enabled = false, typing = false, native_active = false, activation_confirmed = false;
        bool gamepad_neutral = false;
        bool filter_requested = false, filter_active = false, filter_typing = false;
    } input;
    std::unique_ptr<SteamUiSession> filter;
    std::unique_ptr<ChildChannel> camera;
    FileDescriptor driver, input_gate, beam_gate;
    std::string serial;
    double started = 0, fix = 0, notified = 0, sent = 0, lit = 0, last_imu = 0, last_filter = 0,
           filter_epoch = 0, last_rendered_orientation = 0, last_rendered_fix = 0,
           last_presentation = 0;
    bool closed = false, source_ready = false;
    double gate_sent = 0, gate_confirmed = 0, gate_retry = 0, beam_sent = 0, imu_retry = 0;
    bool input_gate_seen = false, input_gate_absent = false;
    double filter_attempt = 0, filter_retry = 0;
    void clear_filter_confirmation(double now);
    int gate_index = -1;
    bool confirm_input_gate(int index, double now);
    bool update_beam(double now);
    void send_command(const char *op);
    bool check_resume(double time, const Vector3 &led, const Quaternion &orientation);
    void discard_resume();
    void observe_motion(const ControllerReport &report);
    bool showing_resume(double now) const;
    void observation(json_object *event, double now);

  public:
    OpticalSession(RuntimeView &r, std::shared_ptr<ControllerDevice> d, TrackingCalibration c,
                   const Settings &options, const std::filesystem::path &root,
                   const ChildFactory &children, const SteamUiFactory &ui,
                   std::function<void(const TrackingCalibration &)> save = {},
                   const std::optional<TrackingResume> &saved = {});
    ~OpticalSession() override;
    bool acquired() const override {
        return alignment && fix && !guide.started && !closed;
    }
    std::optional<TrackingResume> checkpoint() const override;
    const TrackingInputState &input_state() const override {
        return input;
    }
    void controls(const std::optional<ControllerInput> &report, bool enabled, bool typing) override;
    void update_options(const Settings &options) override;
    std::optional<BodyPoseMessage> step(const std::vector<ControllerInput> &reports) override;
    void acquire() override;
    void recenter() override;
    void rebase(const RigidPose &delta) override;
    json::Value presentation() override;
    void close() override;
};
} // namespace ct
