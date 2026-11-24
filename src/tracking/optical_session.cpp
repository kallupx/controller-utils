#include "optical_session.h"
#include "platform/linux/resource.h"
#include <algorithm>
#include <iostream>

namespace ct {
namespace {
double optical_smoothing(const Settings &settings) {
    return settings.smoothing_ms / 500.;
}
FileDescriptor connect_gate(const char *channel) {
    FileDescriptor gate(socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    sockaddr_un address{};
    const auto length = optical::address(address, channel);
    if (!gate || connect(gate.get(), reinterpret_cast<sockaddr *>(&address), length) ||
        !optical::same_user(gate.get()))
        gate.reset();
    return gate;
}
} // namespace
OpticalSession::OpticalSession(RuntimeView &r, std::shared_ptr<ControllerDevice> d,
                               TrackingCalibration c, const Settings &options,
                               const std::filesystem::path &root, const ChildFactory &children,
                               const SteamUiFactory &ui,
                               std::function<void(const TrackingCalibration &)> save,
                               const std::optional<TrackingResume> &saved)
    : runtime(r), device(std::move(d)), cal(std::move(c)), save_calibration(std::move(save)),
      settings(options), imu(cal.body, device->serial(), cal.emitter),
      position(optical_smoothing(options)), serial(device->serial()) {
    started = last_imu = raw_time();
    status = "Waiting for Frame's tracking cameras.";
    if (runtime.serial(0) != cal.headset_serial || !runtime.head_pose())
        throw std::runtime_error("Put the calibrated Frame on to start tracking.");
    if (saved && saved->headset_serial == cal.headset_serial && saved->controller_serial == serial)
        resume = saved;
    try {
        driver.reset(socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
        sockaddr_un a{};
        auto size = optical::address(a);
        if (!driver || connect(driver.get(), reinterpret_cast<sockaddr *>(&a), size) ||
            !optical::same_user(driver.get()))
            throw std::runtime_error(
                "Controller Utils optical driver is unavailable. Restart SteamVR.");
        camera =
            children({"/usr/bin/python3", "-B", (root / "optical/frame_camera_runtime.py").string(),
                      cal.headset_serial});
        try {
            filter = ui(serial, false);
        } catch (const std::exception &e) {
            input.error = e.what();
        }
    } catch (...) {
        close();
        throw;
    }
}
OpticalSession::~OpticalSession() {
    try {
        close();
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
    }
}
void OpticalSession::send_command(const char *op) {
    json::Value msg(json_object_new_object());
    json_object_object_add(msg.get(), "op", json_object_new_string(op));
    json_object_object_add(msg.get(), "time", json_object_new_double(raw_time()));
    camera->send(msg.get());
}
void OpticalSession::acquire() {
    discard_resume();
    restart_anchor = false;
    send_command("reacquire");
    guide.begin(raw_time());
}
void OpticalSession::recenter() {
    const auto head = runtime.head_pose(2);
    if (resume || !alignment || !imu.orientation || !head) {
        acquire();
        return;
    }
    const auto back = rotate(multiply(*alignment, *imu.orientation), {0, 0, 1});
    if (std::min(std::hypot((*head)[2], (*head)[10]), std::hypot(back[0], back[2])) < .25)
        return;
    const double half = (std::atan2((*head)[2], (*head)[10]) - std::atan2(back[0], back[2])) / 2;
    alignment = multiply(Quaternion{std::cos(half), 0, std::sin(half), 0}, *alignment);
    if (!guide.started)
        guide.finish(raw_time());
}
void OpticalSession::rebase(const RigidPose &delta) {
    validate_pose(delta);
    // Optical poses use raw space. The renderer already applies the standing transform.
}
void OpticalSession::update_options(const Settings &options) {
    if (lit && options.brightness != settings.brightness)
        device->enable_ir(options.brightness);
    settings = options;
    position.smoothing = optical_smoothing(options);
}
void OpticalSession::controls(const std::optional<ControllerInput> &report, bool enabled,
                              bool typing) {
    input.enabled = enabled;
    input.typing = typing;
    if (report && (!input.received || report->received > *input.received)) {
        auto controls = input_controls(report->report);
        const auto previous = input.received && report->received - *input.received <= .15
                                  ? input.input.value_or(InputControls{})
                                  : InputControls{};
        const auto pressed = [](bool touched, double pressure, bool held) {
            return touched && pressure >= (held ? .075 : .12);
        };
        controls.pad_click |=
            pressed(controls.pad_touch, report->report.right_pad[2], previous.pad_click);
        controls.scroll_click |=
            pressed(controls.scroll_touch, report->report.left_pad[2], previous.scroll_click);
        input.input = controls;
        input.buttons = report->report.buttons;
        input.received = report->received;
        input.raw_received = report->received + raw_time() - monotonic_time();
    }
}
bool OpticalSession::confirm_input_gate(int index, double now) {
    if (index < 0 || index >= 16) {
        input_gate.reset();
        gate_index = -1;
        gate_sent = gate_confirmed = 0;
        return false;
    }
    if (!input_gate)
        return false;
    if (gate_index != index || now - gate_sent >= .15) {
        gate_sent = now;
        int32_t value = index;
        if (gate_index != index)
            gate_confirmed = 0;
        gate_index = index;
        if (send(input_gate.get(), &value, sizeof(value), MSG_NOSIGNAL) != sizeof(value)) {
            input_gate.reset();
            gate_index = -1;
            gate_confirmed = 0;
            return false;
        }
    }
    int32_t ack;
    while (true) {
        const auto n = recv(input_gate.get(), &ack, sizeof(ack), MSG_TRUNC);
        if (n < 0 && errno == EAGAIN)
            break;
        if (n != sizeof(ack)) {
            input_gate.reset();
            gate_index = -1;
            gate_confirmed = 0;
            return false;
        }
        if (ack == index)
            gate_confirmed = now;
    }
    return gate_confirmed && now - gate_confirmed < .45;
}
void OpticalSession::clear_filter_confirmation(double now) {
    input.filter_active = false;
    input.filter_index.reset();
    input.filter_confirmed.reset();
    filter_epoch = now;
}
void OpticalSession::update_beam(double now) {
    if (!input.native_active || !input.activation_confirmed || !input.input ||
        !input.input->pad_touch) {
        beam_gate.reset();
        beam_sent = 0;
        return;
    }
    if (now - beam_sent < .15)
        return;
    beam_sent = now;
    const auto index = runtime.primary_dashboard_device();
    if (!index || *index >= vr::k_unMaxTrackedDeviceCount ||
        runtime.serial(*index) != "controllerutils-sc26-input-" + serial) {
        beam_gate.reset();
        return;
    }
    if (!beam_gate)
        beam_gate = connect_gate("beam");
    const int32_t device_index = *index;
    if (beam_gate && send(beam_gate.get(), &device_index, sizeof(device_index), MSG_NOSIGNAL) !=
                         sizeof(device_index))
        beam_gate.reset();
}
void OpticalSession::observation(json_object *event, double now) {
    auto op = json::string(json::field(event, "op"));
    if (op == "error")
        throw std::runtime_error(json::string(json::field(event, "message")));
    if (op == "disconnected")
        throw CameraDisconnected();
    if (op == "waiting" && !source_ready) {
        status = json::string(json::field(event, "message"));
        return;
    }
    if (op == "ready") {
        if (source_ready)
            return;
        device->enable_imu();
        device->enable_ir(0);
        send_command("dark");
        if (resume)
            send_command("check_resume");
        last_imu = raw_time();
        source_ready = true;
        return;
    }
    if (op == "light") {
        if (!source_ready) {
            send_command("request_ready");
            return;
        }
        device->enable_ir(settings.brightness);
        lit = now;
        send_command("lit");
        return;
    }
    if (op != "seed" && op != "led" && op != "bearing")
        throw std::runtime_error("Unknown camera observation");
    const double t = json::number(json::field(event, "time"));
    if (op == "seed" && fix && !guide.started)
        return;
    if (guide.started && t < *guide.started)
        return;
    const auto head = heads.at(t);
    if ((op != "seed" && t <= fix) || now - t < -.02 || now - t > (op == "seed" ? .55 : .15) ||
        !head || !imu.clock.scale) {
        if (op == "seed")
            acquire();
        return;
    }
    Vector3 point;
    std::optional<Quaternion> measured;
    const auto before = imu.history.at(t);
    if (before)
        measured = quaternion(*before);
    if (op == "seed") {
        const auto pose = json::numbers<12>(json::field(event, "transform"));
        validate_pose(pose);
        point = translation(pose);
        if (!finite(point) || norm(point) > 1.6) {
            acquire();
            return;
        }
        const auto world = multiply(quaternion(*head), quaternion(pose));
        if (!measured) {
            if (imu.orientation) {
                acquire();
                return;
            }
            imu.orientation = world;
            imu.orientation_time = t;
            measured = world;
        }
        discard_resume();
        alignment = multiply(world, conjugate(*measured));
        restart_anchor = bool(guide.started);
        if (!restart_anchor)
            guide.finish(now);
    } else if (op == "led")
        point = json::numbers<3>(json::field(event, "point"));
    if (!measured || t <= fix || now - t > .15)
        return;
    if (op == "bearing") {
        if (!alignment || !fix)
            return;
        const auto emitter = rotate(multiply(*alignment, *measured), cal.emitter);
        const auto origin = json::numbers<3>(json::field(event, "origin"));
        const auto ray = json::numbers<3>(json::field(event, "direction"));
        if (finite(origin) && norm(origin) < .5 && std::abs(norm(ray) - 1) < .001)
            position.correct_bearing(t, transform_point(*head, origin),
                                     rotate(quaternion(*head), unit(ray)), emitter, now);
        return;
    }
    if (!finite(point) || norm(point) > 1.6)
        return;
    const auto led = transform_point(*head, point);
    const bool confirming_resume = resume && op == "led";
    if (confirming_resume && !check_resume(t, led, *measured))
        return;
    if (!alignment)
        return;
    const auto emitter = rotate(multiply(*alignment, *measured), cal.emitter);
    const auto world = subtract(led, emitter);
    if (restart_anchor) {
        const auto estimate = position.at(t);
        const auto current = position.value(now);
        if (current && distance(estimate ? estimate->position : *current, world) > 1.5)
            position = InertialPosition(position.smoothing);
    }
    if (!position.time)
        position.sample(std::max(t, imu.orientation_time.value_or(t)), {}, 0);
    if (position.observe(t, world, now)) {
        fix = t;
        if (confirming_resume) {
            send_command("resume");
            resume.reset();
        }
        if (restart_anchor) {
            restart_anchor = false;
            guide.finish(now);
        }
    }
}
bool OpticalSession::check_resume(double time, const Vector3 &led, const Quaternion &orientation) {
    if (!position.observations.empty() && time <= position.observations.back().time)
        return false;
    auto expected = resume->led;
    const auto predicted = position.at(time);
    const bool moving = resume_motion_since && time >= resume_motion_since - .02;
    if (moving && predicted && alignment)
        expected = add(predicted->position, rotate(multiply(*alignment, orientation), cal.emitter));
    if (distance(led, expected) > (moving ? .22 : .15)) {
        discard_resume();
        return false;
    }
    if (!alignment && !resume_still_since)
        return false;
    if (resume_seen && time > resume_seen && time - resume_seen <= .15) {
        if (!alignment)
            alignment = multiply(resume->pose.orientation, conjugate(orientation));
        return true;
    }
    resume_seen = time;
    return false;
}
void OpticalSession::discard_resume() {
    if (!resume)
        return;
    resume.reset();
    alignment.reset();
    position = InertialPosition(position.smoothing);
}
void OpticalSession::observe_motion(const ControllerReport &report) {
    const auto acceleration = imu.body_vector(report.acceleration);
    const double gravity = norm(acceleration);
    const double angular = norm(subtract(report.angular_velocity, imu.bias));
    const bool quiet = angular < .15 && std::abs(gravity - 9.80665) < .8;
    if (quiet)
        last_up = scale(acceleration, 1 / gravity);
    if (!resume)
        return;
    const bool tilted = quiet && dot(*last_up, resume->up) < .94;
    if (alignment) {
        if (!resume_motion_since && (tilted || angular > .3 || std::abs(gravity - 9.80665) > .8))
            resume_motion_since = last_imu;
    } else if (angular > .65 || tilted ||
               (resume_still_since && std::abs(gravity - 9.80665) > 1.8)) {
        discard_resume();
    } else if (!quiet) {
        resume_still_since = 0;
        resume_seen = 0;
    } else if (!resume_still_since) {
        resume_still_since = last_imu;
    }
}
bool OpticalSession::showing_resume(double now) const {
    return resume && now - last_imu < .15 &&
           (alignment || (resume_still_since && last_imu - resume_still_since >= .12));
}
std::optional<TrackingResume> OpticalSession::checkpoint() const {
    if (resume)
        return resume_motion_since ? std::nullopt : resume;
    if (!acquired() || !imu.orientation || !imu.orientation_time)
        return {};
    const auto now = raw_time();
    const auto estimated = position.value(now);
    if (!estimated)
        return {};
    const auto orientation = multiply(*alignment, *imu.orientation);
    const auto up = last_up.value_or(rotate(conjugate(orientation), {0, 1, 0}));
    const auto led = add(*estimated, rotate(orientation, cal.emitter));
    return TrackingResume{cal.headset_serial,
                          serial,
                          led,
                          up,
                          {fix, now, *estimated, *imu.orientation_time, orientation}};
}
std::optional<BodyPoseMessage> OpticalSession::step(const std::vector<ControllerInput> &reports) {
    const double now = raw_time(), mono = monotonic_time();
    const auto head = runtime.head_pose(2);
    if (!head)
        throw std::runtime_error("Headset asleep. Tracking will resume when worn.");
    heads.add(now, head);
    if (resume && resume_motion_since && now - resume_motion_since > .85)
        discard_resume();
    for (const auto &r : reports) {
        if (!source_ready)
            break;
        const auto old = imu.orientation_time;
        const auto tick = imu.clock.last_tick;
        // During a swing the accelerometer is a poor gravity reference for the gyro.
        const double gravity_gain = !alignment || position.stationary ? 2. : .4;
        imu.sample(r.report, r.received, now - mono, head, gravity_gain);
        if (imu.clock.last_tick != tick) {
            last_imu = r.received + now - mono;
            observe_motion(r.report);
            if (!cal.gyro_calibrated) {
                if (fix && !position.stationary)
                    gyro_calibration = {};
                else if (const auto bias = gyro_calibration.sample(r.report, r.received)) {
                    cal.body.gyro_bias_report_rad_s = imu.bias = *bias;
                    cal.gyro_calibrated = true;
                    if (imu.previous)
                        imu.previous->second =
                            imu.body_vector(subtract(r.report.angular_velocity, *bias));
                    try {
                        if (save_calibration)
                            save_calibration(cal);
                    } catch (const std::exception &error) {
                        std::cerr << "Could not save controller gyro bias: " << error.what()
                                  << '\n';
                    }
                }
            }
        }
        if (!imu.orientation_time || old == imu.orientation_time)
            continue;
        const double t = *imu.orientation_time;
        if (!alignment && showing_resume(now) && now - t >= -.02 && now - t <= .15) {
            alignment = multiply(resume->pose.orientation, conjugate(*imu.orientation));
            position.sample(t, {}, 0);
            position.observe(t, resume->pose.position, now);
            continue;
        }
        if (alignment && imu.orientation && (!position.time || t > *position.time)) {
            if (resume && !resume_motion_since)
                position.sample(t, {}, 0);
            else {
                const auto acc = rotate(*imu.orientation, imu.body_vector(r.report.acceleration));
                // Remove gravity before applying the palm alignment's estimated tilt.
                position.sample(t, rotate(*alignment, subtract(acc, {0, 9.80665, 0})),
                                norm(r.report.angular_velocity));
            }
        }
    }
    const auto worker_exit = camera->exit_status();
    for (const auto &event : camera->poll())
        observation(event.get(), raw_time());
    if (worker_exit)
        throw std::runtime_error("Camera worker stopped.");
    if (source_ready && now - notified >= .015) {
        json::Value msg(json_object_new_object());
        json_object_object_add(msg.get(), "op", json_object_new_string("sample"));
        json_object_object_add(msg.get(), "time", json_object_new_double(now));
        if (resume && (resume_still_since || resume_motion_since)) {
            auto led = resume->led;
            if (resume_motion_since && alignment && imu.orientation)
                if (const auto estimated = position.value(now))
                    led = add(*estimated,
                              rotate(multiply(*alignment, *imu.orientation), cal.emitter));
            const auto prior = transform_point(inverse_pose(*head), led);
            json_object_object_add(msg.get(), "prior", json::value(prior));
        } else if (alignment && imu.orientation && imu.orientation_time &&
                   wire::fresh_input_time(*imu.orientation_time, now) &&
                   now - *imu.orientation_time < .06 && fix && now - fix < .8) {
            if (const auto p = position.value(now)) {
                const auto emitter = rotate(multiply(*alignment, *imu.orientation), cal.emitter);
                const auto prior = transform_point(inverse_pose(*head), add(*p, emitter));
                json_object_object_add(msg.get(), "prior", json::value(prior));
            }
        }
        if (!reports.empty()) {
            const auto &r = reports.back().report;
            auto a = imu.body_vector(r.acceleration);
            if (norm(a) > 8.8 && norm(a) < 10.8 && norm(r.angular_velocity) < 1.5) {
                const auto up = unit(a);
                json_object *arr = json_object_new_array();
                json_object_array_add(arr,
                                      json::value(Vector3{(*head)[4], (*head)[5], (*head)[6]}));
                json_object_array_add(arr, json::value(up));
                json_object_object_add(msg.get(), "up", arr);
            }
        }
        camera->send(msg.get());
        notified = now;
    }
    if (source_ready && now - last_imu > .08) {
        if (now - last_imu > 5)
            throw std::runtime_error("Controller IMU stopped.");
        if (!reports.empty() && mono - reports.back().received < .15 && now >= imu_retry) {
            device->resume_imu();
            imu_retry = now + .5;
        }
    }
    const auto action = guide.update(input.buttons, input.raw_received, now);
    if (action == AlignmentAction::Restart)
        acquire();
    else if (action == AlignmentAction::Heading)
        recenter();
    std::optional<BodyPoseMessage> result;
    if (alignment && imu.orientation && imu.orientation_time &&
        wire::fresh_input_time(*imu.orientation_time, now) && fix &&
        fix <= *imu.orientation_time + .02) {
        auto p = position.value(now);
        if (p)
            result = BodyPoseMessage{fix, now, *p, *imu.orientation_time,
                                     multiply(*alignment, *imu.orientation)};
    }
    if (result)
        last_valid_pose = result;
    else if (last_valid_pose && wire::fresh_input_time(last_valid_pose->orientation_time, now))
        result = last_valid_pose;
    const bool wanted = result && input.enabled && input.received && mono - *input.received < .15;
    bool claim = wanted && filter && mono >= filter_retry;
    if (claim && !input_gate) {
        input_gate = connect_gate("input");
        if (!input_gate) {
            filter_retry = mono + .75;
            claim = false;
        }
    }
    const bool filtered = claim && input.filter_active && input.filter_confirmed &&
                          mono - *input.filter_confirmed < .75;
    const bool was_active = input.native_active;
    input.native_active = filtered && confirm_input_gate(input.filter_index.value_or(-1), mono);
    if (claim && !input.native_active &&
        (was_active || (input.filter_requested && mono - filter_attempt >= .45))) {
        filter_retry = mono + .75;
        claim = false;
    }
    if (!claim)
        confirm_input_gate(-1, mono);
    if (filter) {
        try {
            if (claim != input.filter_requested || input.typing != input.filter_typing ||
                mono - last_filter >= .15) {
                if (claim != input.filter_requested || input.typing != input.filter_typing) {
                    clear_filter_confirmation(mono);
                    if (claim && !input.filter_requested)
                        filter_attempt = mono;
                }
                FilterRequest req;
                req.enabled = claim;
                req.typing = input.typing;
                req.details = claim || input.typing;
                if (input.input) {
                    req.touches = unsigned(input.input->scroll_touch) |
                                  (unsigned(input.input->pad_touch) << 1);
                    req.held = input.buttons != 0 ||
                               std::max(input.input->trigger, input.input->grip) > .1;
                }
                filter->send(req);
                input.filter_requested = claim;
                input.filter_typing = input.typing;
                last_filter = mono;
            }
            for (auto &event : filter->poll()) {
                if (auto *ack = std::get_if<FilterState>(&event); ack && ack->serial == serial &&
                                                                  ack->time >= filter_epoch &&
                                                                  ack->time <= mono + .02) {
                    input.filter_active = ack->active;
                    input.filter_index =
                        ack->active ? std::optional<int>(ack->index) : std::nullopt;
                    input.filter_confirmed = ack->time;
                    input.keyboard_health = ack->keyboard_health;
                    input.error.reset();
                } else if (auto *err = std::get_if<FilterError>(&event))
                    throw std::runtime_error(err->message);
            }
        } catch (const std::exception &e) {
            input.error = e.what();
            clear_filter_confirmation(mono);
            input.native_active = false;
            confirm_input_gate(-1, mono);
            filter_retry = mono + .75;
        }
    }
    if (!input.native_active)
        input.activation_confirmed = false;
    if (now - sent >= .009) {
        auto controls = input.input.value_or(InputControls{});
        if (input.native_active && !input.activation_confirmed) {
            const auto index = runtime.primary_dashboard_device();
            input.activation_confirmed =
                index && runtime.serial(*index) == "controllerutils-sc26-input-" + serial;
        }
        controls.activate = !input.activation_confirmed;
        auto driver_pose = result;
        if (driver_pose)
            driver_pose->time = driver_pose->orientation_time;
        optical::Packet packet{};
        if (serial.size() >= sizeof(packet.serial))
            throw std::runtime_error("Invalid controller serial");
        serial.copy(packet.serial, serial.size());
        packet.input = bridge_input(started, now, driver_pose, controls, input.native_active);
        const ssize_t n = send(driver.get(), &packet, sizeof(packet), MSG_NOSIGNAL);
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            throw std::runtime_error("Optical driver disconnected.");
        sent = now;
    }
    update_beam(mono);
    if (!result && showing_resume(now)) {
        result = resume->pose;
        result->position_time = now;
        result->orientation_time = last_imu;
        if (resume_motion_since && alignment && imu.orientation)
            if (const auto estimated = position.value(now)) {
                result->position = *estimated;
                result->orientation = multiply(*alignment, *imu.orientation);
            }
    }
    if (!source_ready)
        return {};
    if (resume && result)
        status = "Showing the last known gamepad position.";
    else
        status = result
                     ? (now - fix > .15 ? "Using motion sensors between stereo LED fixes."
                                        : "Tracking with Frame cameras and gamepad motion sensors.")
                     : (lit ? (resume ? "Looking for the gamepad where it was left."
                                      : "Looking for the gamepad and your hands.")
                            : "Starting Frame cameras.");
    if (!result || result->orientation_time <= last_rendered_orientation ||
        result->time < last_rendered_fix)
        return {};
    last_rendered_orientation = result->orientation_time;
    last_rendered_fix = result->time;
    return result;
}
json::Value OpticalSession::presentation() {
    const double now = raw_time();
    if (now - last_presentation < .05)
        return {};
    last_presentation = now;
    if (auto hold = alignment_hold_feedback(guide, now))
        return hold;
    if (lit && guide.started)
        return tracking_feedback(now, "palm",
                                 "Restarting tracking. Hold the gamepad normally where the "
                                 "cameras can see it.");
    if (!fix && !showing_resume(now))
        return tracking_feedback(now, lit && !resume ? "palm" : "connecting", status);
    if (guide.completed && now - *guide.completed < 2)
        return tracking_feedback(now, "ready", "Gamepad aligned.", 1);
    return json::parse(R"({"op":"tracking_feedback","active":false})");
}
void OpticalSession::close() {
    if (closed)
        return;
    closed = true;
    driver.reset();
    input_gate.reset();
    beam_gate.reset();
    std::exception_ptr failure;
    try {
        if (camera)
            camera->close("{\"op\":\"stop\"}\n", 10);
    } catch (...) {
        failure = std::current_exception();
    }
    try {
        if (filter)
            filter->close();
    } catch (...) {
        failure = std::current_exception();
    }
    try {
        device->restore();
    } catch (...) {
        failure = std::current_exception();
    }
    if (failure)
        std::rethrow_exception(failure);
}
} // namespace ct
