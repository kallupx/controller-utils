#pragma once
#include "app/settings.h"
#include "controller/device.h"
#include "interaction/input_control.h"
#include "platform/linux/child.h"
#include "steamvr/runtime.h"
#include "steamvr/steam_ui.h"
#include "acquisition.h"
#include "calibration.h"
#include "native_pose.h"
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace ct {
json::Value tracking_feedback(double now, const std::string &state, const std::string &message,
                              std::optional<double> progress = std::nullopt);

class TrackingSessionView {
  public:
    AlignmentGuide guide;
    std::string status = "Starting native controller tracking...";
    virtual ~TrackingSessionView() = default;
    virtual bool acquired() const = 0;
    virtual const InputControl::State &input_state() const = 0;
    virtual void controls(const std::optional<ControllerInput> &input, bool enabled,
                          bool typing = false) = 0;
    virtual void update_options(const Settings &options) = 0;
    virtual std::optional<BodyPoseMessage> step(const std::vector<ControllerInput> &reports) = 0;
    virtual void acquire() = 0;
    virtual void recenter() = 0;
    virtual void rebase(const RigidPose &delta) = 0;
    virtual json::Value presentation() = 0;
    virtual void close() = 0;
};

class TrackingSession final : public TrackingSessionView, private InputBackend {
    RuntimeView &runtime_;
    std::shared_ptr<ControllerDevice> controller_;
    TrackingCalibration calibration_;
    StationaryGyroBias bias_calibration_;
    std::function<void(const TrackingCalibration &)> save_calibration_;
    Settings options_;
    ChildFactory children_;
    bool closed_ = false, device_configured_ = false;
    bool helper_ready_ = false, helper_restored_ = false;
    std::unique_ptr<ChildChannel> helper_;
    std::unique_ptr<SteamUiSession> filter_;
    std::unique_ptr<NativeTracking> native_;
    std::optional<InputControl> input_;
    std::function<double()> monotonic_, raw_;
    std::function<void(double)> sleep_;
    double started_ = 0, last_imu_ = 0, imu_retry_ = 0;
    double last_helper_heartbeat_ = 0;
    double last_sent_ = 0, last_orientation_ = 0, last_controls_ = 0;
    double last_presentation_ = 0, last_health_ = 0, max_step_ms_ = 0;
    unsigned imu_samples_ = 0, native_poses_ = 0;
    bool imu_recovering_ = false, heading_requested_ = false;
    std::optional<int> brightness_;
    std::string serial_, alignment_hint_ = "Hold the gamepad still for a moment.";
    std::optional<NativeBodyPose> tracker_;
    void poll_helper(double now);
    void recover_imu(double now);
    NativeBodyPose new_mapper() const;
    bool has_filter() const override;
    bool filter_running() const override;
    void send_filter(const FilterRequest &request) override;
    std::vector<FilterEvent> poll_filter() override;
    void helper_command(uint8_t command) override;
    bool release_controls() override;
    std::optional<unsigned> pointer_device(const std::string &serial) override;
    bool pointer_selected(unsigned device) override;

  public:
    InitialAcquisition initial;
    TrackingSession(RuntimeView &runtime, std::shared_ptr<ControllerDevice> controller,
                    const TrackingCalibration &calibration, const Settings &options,
                    ChildFactory children, SteamUiFactory steam_ui,
                    std::function<double()> monotonic = {}, std::function<double()> raw = {},
                    std::function<void(double)> sleep = {},
                    std::function<void(const TrackingCalibration &)> save_calibration = {});
    TrackingSession(const TrackingSession &) = delete;
    TrackingSession &operator=(const TrackingSession &) = delete;
    ~TrackingSession() override;
    bool acquired() const override {
        return initial.ready;
    }
    const InputControl::State &input_state() const override;
    void controls(const std::optional<ControllerInput> &input, bool enabled,
                  bool typing = false) override;
    void update_options(const Settings &options) override;
    std::optional<BodyPoseMessage> step(const std::vector<ControllerInput> &reports) override;
    void acquire() override;
    void recenter() override;
    void rebase(const RigidPose &delta) override;
    json::Value presentation() override;
    void close() override;
};
} // namespace ct
