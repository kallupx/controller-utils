#include "session.h"
#include "platform/linux/resource.h"
#include <algorithm>
#include <cmath>
#include <exception>
#include <iostream>
#include <stdexcept>

namespace ct {
namespace {
std::string operation(json_object *event) {
    auto op = json::find(event, "op");
    return op && json_object_is_type(op, json_type_string) ? json::string(op) : "";
}
std::string message(json_object *event, const char *fallback, size_t limit) {
    auto value = json::find(event, "message");
    return (value ? json::string(value) : fallback).substr(0, limit);
}
bool restored(const std::vector<json::Value> &events, const std::string &name) {
    return std::any_of(events.begin(), events.end(),
                       [&](const auto &event) { return operation(event.get()) == name; });
}
void diagnostic(json_object *value) {
    std::cerr << json::encode(value) << '\n';
}
} // namespace
json::Value tracking_feedback(double now, const std::string &state, const std::string &text,
                              std::optional<double> progress) {
    json::Value result(json_object_new_object());
    json_object_object_add(result.get(), "op", json_object_new_string("tracking_feedback"));
    json_object_object_add(result.get(), "active", json_object_new_boolean(true));
    json_object_object_add(result.get(), "time", json_object_new_double(now));
    json_object_object_add(result.get(), "state", json_object_new_string(state.c_str()));
    json_object_object_add(result.get(), "message", json_object_new_string(text.c_str()));
    json_object_object_add(result.get(), "progress", json::value(progress));
    return result;
}
TrackingSession::TrackingSession(RuntimeView &runtime, std::shared_ptr<ControllerDevice> controller,
                                 const TrackingCalibration &calibration, const Settings &options,
                                 ChildFactory children, SteamUiFactory steam_ui,
                                 std::function<double()> monotonic, std::function<double()> raw,
                                 std::function<void(double)> sleep,
                                 std::function<void(const TrackingCalibration &)> save_calibration)
    : runtime_(runtime), controller_(std::move(controller)), calibration_(calibration),
      save_calibration_(std::move(save_calibration)), options_(options),
      children_(std::move(children)), monotonic_(monotonic ? std::move(monotonic) : monotonic_time),
      raw_(raw ? std::move(raw) : raw_time), sleep_(sleep ? std::move(sleep) : sleep_for) {
    started_ = last_imu_ = last_health_ = monotonic_();
    try {
        if (!controller_)
            throw std::invalid_argument("controller device is missing");
        if (runtime_.serial(0) != calibration_.headset_serial)
            throw std::runtime_error("tracking calibration belongs to another headset");
        if (!runtime_.head_pose())
            throw std::runtime_error("Put the Frame on to start tracking.");
        runtime_.queues().preflight();
        serial_ = controller_->serial();
        tracker_.emplace(new_mapper());
        input_.emplace(static_cast<InputBackend &>(*this), serial_, monotonic_, raw_);
        device_configured_ = true;
        controller_->enable_imu();
        update_options(options_);
        started_ = last_imu_ = monotonic_();
        helper_ = children_({"/usr/bin/sudo", "-n",
                             "/var/lib/controller-utils/controller-utils-helper", "--private"});
        if (!helper_)
            throw std::runtime_error("Native compatibility helper could not start.");
        try {
            filter_ = steam_ui(serial_, false);
            if (!filter_)
                throw std::runtime_error("Steam input filter could not start.");
        } catch (const std::exception &error) {
            const auto text = std::string(error.what()).substr(0, 300);
            input_->state.error = text.empty() ? "Steam input filter could not start." : text;
        }
    } catch (...) {
        try {
            close();
        } catch (const std::exception &error) {
            std::cerr << "Native startup cleanup: " << error.what() << '\n';
        }
        throw;
    }
}
TrackingSession::~TrackingSession() {
    try {
        close();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
    }
}
NativeBodyPose TrackingSession::new_mapper() const {
    return NativeBodyPose(calibration_.body, serial_, calibration_.emitter,
                          options_.smoothing_ms / 1000.);
}
const InputControl::State &TrackingSession::input_state() const {
    if (!input_)
        throw std::logic_error("Native tracking session is closed.");
    return input_->state;
}
bool TrackingSession::has_filter() const {
    return bool(filter_);
}
bool TrackingSession::filter_running() const {
    return filter_ && filter_->running();
}
void TrackingSession::send_filter(const FilterRequest &request) {
    filter_->send(request);
}
std::vector<FilterEvent> TrackingSession::poll_filter() {
    return filter_->poll();
}
void TrackingSession::helper_command(uint8_t command) {
    if (!helper_)
        throw std::runtime_error("Native compatibility helper is unavailable.");
    helper_->send(std::string(1, char(command)));
}
bool TrackingSession::release_controls() {
    return native_ && native_->controls(raw_(), std::nullopt,
                                        input_->state.input.value_or(InputControls{}), false);
}
std::optional<unsigned> TrackingSession::pointer_device(const std::string &serial) {
    std::vector<unsigned> matches;
    for (const auto &device : runtime_.devices())
        if (device.device_class == 2 && runtime_.serial(device.index) == serial)
            matches.push_back(device.index);
    if (matches.size() != 1 || matches[0] >= 64)
        return std::nullopt;
    return matches[0];
}
void TrackingSession::controls(const std::optional<ControllerInput> &input, bool enabled,
                               bool typing) {
    if (closed_)
        throw std::runtime_error("Native tracking session is closed.");
    input_->controls(input, enabled, typing);
}
void TrackingSession::update_options(const Settings &options) {
    if (closed_)
        throw std::runtime_error("Native tracking session is closed.");
    if (options.brightness < 0 || options.brightness > 100)
        throw std::invalid_argument("invalid brightness");
    if (options.smoothing_ms < 0 || options.smoothing_ms > 150)
        throw std::invalid_argument("invalid smoothing_ms");
    options_ = options;
    tracker_->motion.smoothing = options.smoothing_ms / 1000.;
    if (brightness_ != options.brightness) {
        controller_->enable_ir(unsigned(options.brightness));
        brightness_ = options.brightness;
    }
}
void TrackingSession::poll_helper(double now) {
    for (auto &event : helper_->poll()) {
        const auto op = operation(event.get());
        if (op == "native_helper_ready") {
            auto isolated = json::find(event.get(), "isolated");
            if (!isolated || !json_object_is_type(isolated, json_type_boolean) ||
                !json::boolean(isolated))
                throw std::runtime_error("The helper did not confirm isolated native tracking.");
            helper_ready_ = true;
        } else if (op == "input_gate_ready")
            input_->state.gate_ready = true;
        else if (op == "beam_gate_ready")
            input_->state.beam_ready = true;
        else if (op == "beam_gate_error") {
            input_->state.beam_ready = input_->state.beam_hidden = false;
            std::cerr << "Controller Utils beam: "
                      << message(event.get(), "Native beam control is unavailable.", 1000) << '\n';
        } else if (op == "input_gate_waiting" || op == "beam_gate_waiting")
            std::cerr << "Controller Utils UI startup: " << message(event.get(), "", 1000) << '\n';
        else if (op == "input_gate_state") {
            try {
                const auto index = json::integer(json::field(event.get(), "index"));
                if (index < -1 || index > 15)
                    throw std::invalid_argument("invalid gate index");
                if (input_->gate_ack(int(index),
                                     json::number(json::field(event.get(), "monotonic_time"))))
                    last_controls_ = 0;
            } catch (const std::exception &) {
                input_->fail("Invalid native input gate confirmation.");
            }
        } else if (op == "input_gate_error") {
            input_->state.gate_ready = false;
            const auto text = message(event.get(), "Native UI control is unavailable.", 1000);
            std::cerr << "Controller Utils UI control: " << text << '\n';
            input_->fail(text);
        } else if (op == "native_helper_restored")
            helper_restored_ = true;
        else if (op == "native_helper_error")
            throw std::runtime_error(message(event.get(), "Native compatibility failed.", 1000));
        else if (op == "native_helper_quality") {
            double stamp;
            bool good;
            try {
                stamp = json::number(json::field(event.get(), "time"));
                good = json::boolean(json::field(event.get(), "good"));
            } catch (const std::exception &) {
                throw std::invalid_argument("Invalid native acquisition quality.");
            }
            const double age = raw_() - stamp;
            if (!initial.ready && native_ && native_->session_start &&
                stamp >= *native_->session_start && 0 <= age && age <= 1)
                if (initial.update(stamp, good))
                    helper_command('l');
        }
    }
    if (helper_->exit_status())
        throw std::runtime_error("Native compatibility helper stopped. Restart tracking.");
    if (now - last_helper_heartbeat_ >= .2) {
        helper_command('.');
        last_helper_heartbeat_ = now;
    }
    if (!helper_ready_ && now - started_ > 15)
        throw std::runtime_error("Native compatibility is not installed or did not become ready.");
}
void TrackingSession::recover_imu(double now) {
    const auto age = now - last_imu_;
    imu_recovering_ = age > .08;
    if (age > 5)
        throw std::runtime_error(
            "Controller IMU stream stopped. Reconnect the gamepad and start again.");
    if (imu_recovering_ && now >= imu_retry_) {
        controller_->resume_imu();
        controller_->enable_ir(unsigned(options_.brightness));
        imu_retry_ = now + .5;
    }
}
std::optional<BodyPoseMessage> TrackingSession::step(const std::vector<ControllerInput> &reports) {
    if (closed_)
        throw std::runtime_error("Native tracking session is closed.");
    if (reports.size() > 256)
        throw std::invalid_argument("controller report batch exceeded capacity");
    double now = raw_();
    const double received = monotonic_();
    poll_helper(received);
    const auto head = runtime_.head_pose(2);
    if (!head)
        throw std::runtime_error("Headset asleep. Tracking will resume when worn.");
    if (!native_ && helper_ready_ && received - started_ >= 2 && tracker_->clock.scale &&
        received - last_imu_ <= .15)
        native_ = std::make_unique<NativeTracking>(runtime_.queues(), controller_profile(serial_),
                                                   monotonic_, sleep_);
    for (const auto &report : reports) {
        ++imu_samples_;
        const auto stamp = tracker_->sample(report.report, report.received, now - received, head);
        if (stamp && !calibration_.gyro_calibrated) {
            if (const auto bias = bias_calibration_.sample(report.report, *stamp)) {
                calibration_.body.gyro_bias_report_rad_s = *bias;
                calibration_.gyro_calibrated = true;
                tracker_->bias = *bias;
                tracker_->calibration = calibration_.body;
                if (tracker_->rotation)
                    tracker_->rotation->bias = *bias;
                if (save_calibration_) {
                    try {
                        save_calibration_(calibration_);
                    } catch (const std::exception &error) {
                        std::cerr << "Controller calibration could not be saved: " << error.what()
                                  << '\n';
                    }
                }
            }
        }
        if (!tracker_->clock.points.empty() &&
            tracker_->clock.points.back().second == report.received)
            last_imu_ = report.received;
        if (native_ && native_->session_start && stamp && *stamp >= *native_->session_start &&
            -.02 <= now - *stamp && now - *stamp <= .15)
            native_->sample(*stamp, report.report);
    }
    recover_imu(received);
    if (!tracker_->clock.scale && received - started_ > 5)
        throw std::runtime_error(
            "Controller IMU clock did not synchronize. Reconnect the gamepad and start again.");
    now = raw_();
    if (native_) {
        const auto pose = native_->poll(now);
        if (pose)
            native_poses_ += unsigned(tracker_->observe(*pose, now));
    }
    const auto action = guide.update(input_->state.buttons, input_->state.raw_received, now);
    if (action == AlignmentAction::Restart)
        acquire();
    else if (action == AlignmentAction::Heading)
        recenter();
    alignment_hint_ = "Hold the gamepad still for a moment.";
    if (initial.ready && (heading_requested_ || !tracker_->palm_aligned)) {
        const bool heading = heading_requested_;
        bool aligned = false;
        try {
            tracker_->align_head(now, *head);
            aligned = true;
        } catch (const std::invalid_argument &error) {
            alignment_hint_ = error.what();
        }
        if (aligned) {
            heading_requested_ = false;
            guide.finish(now);
            json::Value health(json_object_new_object());
            json_object_object_add(health.get(), "op", json_object_new_string("alignment_health"));
            json_object_object_add(health.get(), "mode",
                                   json_object_new_string(heading ? "headset" : "headset_startup"));
            json_object_object_add(health.get(), "time", json_object_new_double(now));
            json_object_object_add(health.get(), "head", json::value(*head));
            json_object_object_add(health.get(), "orientation",
                                   tracker_->orientation ? json::value(*tracker_->orientation)
                                                         : nullptr);
            json_object_object_add(health.get(), "native_orientation",
                                   json::value(tracker_->native->native_orientation));
            json_object_object_add(health.get(), "emitter", json::value(tracker_->native->emitter));
            Vector3 mean{};
            unsigned count = 0;
            for (const auto &[stamp, omega] : tracker_->gyro)
                if (stamp >= now - .4 && stamp <= now + .02) {
                    mean = add(mean, omega);
                    ++count;
                }
            json_object_object_add(health.get(), "gyro_mean_body_rad_s",
                                   count ? json::value(scale(mean, 1. / count)) : nullptr);
            diagnostic(health.get());
        }
    }
    const bool owned = input_->poll(monotonic_(), initial.ready && tracker_->palm_aligned &&
                                                      !heading_requested_ && bool(native_));
    const double control_received = monotonic_();
    now = raw_();
    if (native_ && input_->state.received && -.02 <= control_received - *input_->state.received &&
        control_received - *input_->state.received <= .15 && now - last_controls_ >= .01) {
        const auto body =
            initial.ready && tracker_->palm_aligned ? tracker_->input_pose(now) : std::nullopt;
        if (native_->controls(now, body, input_->state.input.value_or(InputControls{}),
                              owned && bool(body))) {
            input_->state.native_active = owned && bool(body);
            last_controls_ = now;
        }
    }
    input_->update_beam(monotonic_());
    const auto &s = input_->state;
    if (imu_recovering_)
        status = "Restoring controller IMU reporting...";
    else if (!helper_ready_)
        status = "Waiting for native controller tracking...";
    else if (!native_)
        status = "Synchronizing the controller IMU...";
    else if (!calibration_.gyro_calibrated)
        status = "Hold the gamepad still to calibrate its motion sensors.";
    else if (tracker_->rotation && !tracker_->rotation->orientation)
        status =
            "Point the gamepad's top edge where your headset faces, buttons up. Hold it still.";
    else if (!tracker_->native)
        status = "Native tracking is finding the gamepad. Hold it in front of the headset.";
    else if (!initial.ready)
        status = "Point the gamepad's top edge where your headset faces. Hold it still while "
                 "tracking settles.";
    else if (!tracker_->palm_aligned)
        status = "Point the gamepad where your headset faces. " + alignment_hint_;
    else if (heading_requested_)
        status = "Look ahead and point the gamepad the same way. " + alignment_hint_;
    else if (s.error)
        status = "Tracking continues. UI control unavailable: " + *s.error;
    else if (s.typing && s.keyboard_health == "recovering")
        status = "Restoring Steam keyboard input. Lift both thumbs to finish recovery.";
    else if (s.enabled && !s.gate_ready)
        status = "Tracking continues. Waiting for native UI control.";
    else if (s.enabled && !owned && s.received && received - *s.received <= .15)
        status = "Release the gamepad buttons and pads to enable UI control.";
    else if (!tracker_->strict_native(now))
        status = "LED out of view. Following estimated IMU movement. Menu controls wait for native "
                 "tracking.";
    else
        status = "Tracking via Bluetooth and the native Frame service.";
    max_step_ms_ = std::max(max_step_ms_, (monotonic_() - received) * 1000);
    if (received - last_health_ >= 5) {
        json::Value health(json_object_new_object());
        json_object_object_add(health.get(), "op", json_object_new_string("tracking_health"));
        json_object_object_add(health.get(), "backend", json_object_new_string("xrservice"));
        json_object_object_add(health.get(), "time", json_object_new_double(now));
        json_object_object_add(health.get(), "imu_samples", json_object_new_int64(imu_samples_));
        json_object_object_add(health.get(), "native_poses", json_object_new_int64(native_poses_));
        json_object_object_add(health.get(), "startup_ready",
                               json_object_new_boolean(initial.ready));
        json_object_object_add(health.get(), "rejected_native_poses",
                               json_object_new_int64(tracker_->rejected_poses));
        json_object_object_add(health.get(), "rejected_ui_inputs",
                               json_object_new_int64(native_ ? native_->rejected_inputs : 0));
        json_object_object_add(
            health.get(), "orientation_source",
            json_object_new_string(tracker_->rotation ? "calibrated_imu" : "native"));
        json_object_object_add(health.get(), "imu_age_ms",
                               json_object_new_double((received - last_imu_) * 1000));
        json_object_object_add(health.get(), "pose_age_ms",
                               tracker_->native
                                   ? json_object_new_double((now - tracker_->native->time) * 1000)
                                   : nullptr);
        json_object_object_add(health.get(), "max_step_ms", json_object_new_double(max_step_ms_));
        diagnostic(health.get());
        last_health_ = received;
        max_step_ms_ = 0;
    }
    const auto pose =
        initial.ready && tracker_->palm_aligned ? tracker_->message(now) : std::nullopt;
    if (pose && now - last_sent_ >= .01 && pose->orientation_time > last_orientation_) {
        last_sent_ = now;
        last_orientation_ = pose->orientation_time;
        return pose;
    }
    return std::nullopt;
}
void TrackingSession::acquire() {
    if (closed_)
        throw std::runtime_error("Native tracking session is closed.");
    input_->release();
    heading_requested_ = false;
    if (native_) {
        auto old = std::move(native_);
        old->close();
    }
    auto clock = std::move(tracker_->clock);
    auto rotation = std::move(tracker_->rotation);
    tracker_.emplace(new_mapper());
    tracker_->clock = std::move(clock);
    tracker_->rotation = std::move(rotation);
    guide.begin(raw_());
    initial = InitialAcquisition{};
    helper_command('r');
    last_orientation_ = last_sent_ = last_controls_ = 0;
}
void TrackingSession::recenter() {
    if (closed_)
        throw std::runtime_error("Native tracking session is closed.");
    input_->release();
    heading_requested_ = true;
    guide.begin(raw_());
}
void TrackingSession::rebase(const RigidPose &delta) {
    validate_pose(delta);
}
json::Value TrackingSession::presentation() {
    const double now = raw_();
    if (now - last_presentation_ < .05)
        return {};
    last_presentation_ = now;
    const auto hold = guide.progress(now);
    if (hold) {
        if (guide.hold_fired)
            return tracking_feedback(now, "restart",
                                     "Release View + Menu for heading correction, or keep holding "
                                     "to 5 seconds to restart tracking.",
                                     hold);
        return tracking_feedback(
            now, "realign", "Point the gamepad where you are looking. Keep holding View + Menu.",
            hold);
    }
    if (!helper_ready_ || !native_)
        return tracking_feedback(
            now, "connecting",
            "Point the gamepad's top edge where your headset faces. Connecting native tracking.");
    if (!tracker_->rotation || !tracker_->rotation->orientation)
        return tracking_feedback(
            now, "imu",
            "Point the gamepad's top edge where your headset faces, buttons up. Hold it still.");
    if (!tracker_->native)
        return tracking_feedback(now, "palm",
                                 "Finding your hands and the LED. Point the gamepad's top edge "
                                 "where your headset faces and hold still.");
    if (!initial.ready)
        return tracking_feedback(now, "stabilizing",
                                 "Point the gamepad's top edge where your headset faces. Hold "
                                 "still while position tracking settles.");
    if (heading_requested_)
        return tracking_feedback(
            now, "realign", "Look ahead and point the gamepad the same way. " + alignment_hint_);
    if (!tracker_->palm_aligned)
        return tracking_feedback(now, "stabilizing",
                                 "Point the gamepad where your headset faces. " + alignment_hint_);
    if (guide.completed && now - *guide.completed < 2)
        return tracking_feedback(now, "ready",
                                 "Gamepad aligned. View + Menu: hold 1 second to set heading. Keep "
                                 "holding to 5 seconds to restart tracking.",
                                 1);
    return json::parse(R"({"op":"tracking_feedback","active":false})");
}
void TrackingSession::close() {
    if (closed_)
        return;
    closed_ = true;
    std::vector<std::string> errors;
    if (input_)
        errors = input_->release();
    auto collect = [&](auto function) {
        try {
            function();
        } catch (const std::exception &error) {
            errors.emplace_back(error.what());
        }
    };
    if (filter_)
        collect([&] { filter_->close(); });
    if (native_)
        collect([&] { native_->close(); });
    if (device_configured_)
        collect([&] { controller_->restore(); });
    if (helper_)
        collect([&] {
            const auto events = helper_->close("q");
            for (const auto &event : events)
                if (operation(event.get()) == "native_helper_error")
                    diagnostic(event.get());
            if (helper_ready_ && !helper_restored_ && !restored(events, "native_helper_restored"))
                errors.emplace_back("native compatibility restoration was not confirmed");
        });
    if (!errors.empty()) {
        std::string text = "Native tracking cleanup: ";
        for (const auto &error : errors) {
            if (text != "Native tracking cleanup: ")
                text += "; ";
            text += error;
        }
        throw std::runtime_error(text);
    }
}
} // namespace ct
