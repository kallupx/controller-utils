#pragma once
#include "app/settings.h"
#include "steamvr/codec.h"
#include "platform/json.h"
#include "acquisition.h"
#include <memory>
#include <stdexcept>

namespace ct {
json::Value tracking_feedback(double now, const std::string &state, const std::string &message,
                              std::optional<double> progress = std::nullopt);
json::Value alignment_hold_feedback(const AlignmentGuide &guide, double now);

struct TrackingInputState {
    std::optional<std::string> error;
    std::string keyboard_health = "off";
};

struct TrackingResume {
    std::string headset_serial, controller_serial;
    Vector3 led, up;
    BodyPoseMessage pose;
};

struct CameraDisconnected : std::runtime_error {
    CameraDisconnected()
        : std::runtime_error("SteamVR camera stream interrupted. Reconnecting...") {}
};

class TrackingSessionView {
  public:
    AlignmentGuide guide;
    std::string status = "Starting controller tracking...";
    virtual ~TrackingSessionView() = default;
    virtual bool acquired() const = 0;
    virtual std::optional<TrackingResume> checkpoint() const = 0;
    virtual const TrackingInputState &input_state() const = 0;
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

} // namespace ct
