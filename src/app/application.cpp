#include "application.h"
#include "platform/linux/error.h"
#include "platform/linux/resource.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <system_error>

namespace ct {
namespace {
void put(json_object *value, const char *name, bool flag) {
    json_object_object_add(value, name, json_object_new_boolean(flag));
}
void put(json_object *value, const char *name, const std::string &text) {
    json_object_object_add(value, name, json_object_new_string(text.c_str()));
}
json::Value message(const char *op) {
    json::Value value(json_object_new_object());
    put(value.get(), "op", std::string(op));
    return value;
}
void exact_fields(json_object *event, std::initializer_list<const char *> names,
                  const char *error) {
    if (json_object_object_length(event) != int(names.size()))
        throw std::invalid_argument(error);
    for (const auto name : names) {
        json_object *field = nullptr;
        if (!json_object_object_get_ex(event, name, &field))
            throw std::invalid_argument(error);
    }
}
std::string ui_text(std::string text, size_t limit) {
    std::replace(text.begin(), text.end(), ';', '.');
    for (size_t at = 0; (at = text.find("\xe2\x80\x94", at)) != std::string::npos;)
        text.replace(at, 3, ",");
    size_t characters = 0, end = 0;
    while (end < text.size()) {
        if ((uint8_t(text[end]) & 0xc0) != 0x80 && characters++ == limit)
            break;
        ++end;
    }
    text.resize(end);
    return text;
}
json::Value controls_message(const ControllerInput &input, double offset) {
    auto value = message("controls");
    const auto &r = input.report;
    json_object_object_add(value.get(), "time", json_object_new_double(input.received + offset));
    json_object_object_add(value.get(), "buttons", json_object_new_int64(r.buttons));
    json_object_object_add(value.get(), "triggers", json::value(r.triggers));
    json_object_object_add(value.get(), "left_pad", json::value(r.left_pad));
    json_object_object_add(value.get(), "right_pad", json::value(r.right_pad));
    json_object_object_add(value.get(), "left_stick", json::value(r.left_stick));
    json_object_object_add(value.get(), "right_stick", json::value(r.right_stick));
    return value;
}
json::Value pose_message(const BodyPoseMessage &pose) {
    auto value = message("native_pose");
    json_object_object_add(value.get(), "time", json_object_new_double(pose.time));
    json_object_object_add(value.get(), "position_time",
                           json_object_new_double(pose.position_time));
    json_object_object_add(value.get(), "position", json::value(pose.position));
    json_object_object_add(value.get(), "orientation_time",
                           json_object_new_double(pose.orientation_time));
    json_object_object_add(value.get(), "orientation", json::value(pose.orientation));
    return value;
}
bool shortcut_failure(const std::exception &error) {
    return dynamic_cast<const std::runtime_error *>(&error) != nullptr;
}
} // namespace

bool retryable(const std::exception &error) {
    if (dynamic_cast<const ConnectionError *>(&error) ||
        dynamic_cast<const TimeoutError *>(&error) ||
        dynamic_cast<const std::system_error *>(&error))
        return true;
    const std::string text(error.what());
    for (const auto prefix : {"Headset asleep", "Put the Frame on", "connect one Steam Controller",
                              "Put Frame on", "Camera worker stopped", "Controller IMU stopped",
                              "sensor clock reset or long input gap"})
        if (text.rfind(prefix, 0) == 0)
            return true;
    return false;
}

Application::Application(ApplicationHost &host, const Settings &options, bool background,
                         std::function<double()> monotonic, std::function<double()> raw)
    : host_(host), options_(options), monotonic_(monotonic ? std::move(monotonic) : monotonic_time),
      raw_(raw ? std::move(raw) : raw_time),
      status_(options.tracking_enabled ? "Waiting for controller" : "Tracking off"),
      show_settings_(!background) {
    host_.autostart(options_.autostart);
    host_.save(options_);
}
Application::~Application() {
    try {
        close();
    } catch (const std::exception &error) {
        std::cerr << "Controller Utils shutdown: " << error.what() << '\n';
    }
}
void Application::send_op(const char *op) {
    host_.send(message(op));
}
void Application::close_session() {
    auto closing = std::move(session_);
    if (closing)
        closing->close();
}
void Application::close() {
    running_ = false;
    close_session();
}

bool Application::action(json_object *event) {
    const auto name = json::string(json::field(event, "action"));
    if (name == "quit")
        running_ = false;
    else if (name == "toggle") {
        options_.tracking_enabled = !options_.tracking_enabled;
        host_.save(options_);
        retry_ = 0;
    } else if (name == "autostart") {
        host_.autostart(!options_.autostart);
        options_.autostart = !options_.autostart;
        host_.save(options_);
    } else if (name == "automatic_updates") {
        options_.automatic_updates = !options_.automatic_updates;
        host_.save(options_);
    } else if (name == "check_updates" || name == "apply_update") {
        host_.update(name == "apply_update");
    } else if (name == "debug_logging" || name == "save_debug_log") {
        host_.debug_action(name == "save_debug_log");
    } else if (name == "input_toggle")
        send_op("input_toggle");
    else if (name == "pinned") {
        options_.pinned = !options_.pinned;
        host_.save(options_);
    } else if (name == "steam_shortcuts") {
        if (options_.pointer_buttons & 0x10000)
            input_status_ =
                std::string("Choose a different pointer button before enabling Steam shortcuts.");
        else {
            input_status_.reset();
            options_.steam_shortcuts = !options_.steam_shortcuts;
        }
        host_.save(options_);
    } else if (name == "pointer_binding") {
        if (capture_)
            capture_.reset();
        else
            capture_.emplace(monotonic_());
    } else if (name == "recenter" && session_) {
        session_->recenter();
        send_op("hide");
    } else if (name == "acquire" && options_.tracking_enabled) {
        send_op("hide");
        return true;
    } else if (name == "brightness_down" || name == "brightness_up" || name == "smoothing_down" ||
               name == "smoothing_up") {
        const bool brightness = name.rfind("brightness", 0) == 0;
        auto &value = brightness ? options_.brightness : options_.smoothing_ms;
        value = std::clamp(value + (name.find("_up") != std::string::npos ? 10 : -10), 0,
                           brightness ? 100 : 150);
        host_.save(options_);
        if (session_)
            session_->update_options(options_);
    }
    return false;
}

void Application::step(bool awake, bool connected, const std::vector<ControllerInput> &reports) {
    if (!running_)
        return;
    if (reports.size() > 256)
        throw std::invalid_argument("controller report batch exceeded limit");
    if (connected != was_connected_) {
        latest_input_.reset();
        tracking_problem_.reset();
        retry_ = last_status_ = 0;
        was_connected_ = connected;
    }
    const auto sample = reports.empty() ? std::optional<ControllerInput>() : reports.back();
    if (capture_) {
        auto captured = capture_->update(
            sample ? std::optional<uint32_t>(sample->report.buttons) : std::nullopt, monotonic_());
        if (captured) {
            if (*captured) {
                options_.pointer_buttons = *captured;
                if (*captured & 0x10000)
                    options_.steam_shortcuts = false;
                host_.save(options_);
            }
            capture_.reset();
        }
    }
    const bool system_requested =
        awake && connected && !capture_ && !(options_.pointer_buttons & 0x10000);
    bool allow = false;
    try {
        allow = host_.system_input(system_requested);
        if (allow || !system_requested)
            system_status_.reset();
    } catch (const std::exception &error) {
        if (!shortcut_failure(error))
            throw;
        const auto message = "Steam shortcuts unavailable: " + std::string(error.what());
        if (system_status_ != message)
            host_.log(message);
        system_status_ = message;
    }
    std::vector<std::string> shortcut_actions;
    for (const auto &report : reports) {
        const auto actions = gestures_.update(report.report.buttons, report.received,
                                              options_.steam_shortcuts && allow, allow);
        shortcut_actions.insert(shortcut_actions.end(), actions.begin(), actions.end());
    }
    if (reports.empty()) {
        const auto now = monotonic_();
        const bool fresh = latest_input_ && now - latest_input_->received >= -.02 &&
                           now - latest_input_->received <= .15;
        const auto actions =
            gestures_.update(fresh ? latest_input_->report.buttons : 0, now,
                             options_.steam_shortcuts && allow && fresh, allow && fresh, false);
        shortcut_actions.insert(shortcut_actions.end(), actions.begin(), actions.end());
    }
    if (sample) {
        latest_input_ = sample;
        host_.send(controls_message(*sample, raw_() - monotonic_()));
    }
    bool camera_requested = false;
    for (const auto &name : shortcut_actions) {
        if (name == "camera_switch") {
            camera_requested = true;
            continue;
        }
        auto &feature_status = name == "keyboard" ? keyboard_status_ : shortcut_status_;
        try {
            if (name == "keyboard")
                host_.request_keyboard();
            else
                host_.shortcut(name);
            feature_status.reset();
        } catch (const std::exception &error) {
            if (!shortcut_failure(error))
                throw;
            feature_status = error.what();
            host_.log(*feature_status);
        }
    }
    if (!shortcut_animation_error_) {
        try {
            const auto hold = gestures_.progress(monotonic_());
            host_.shortcut_progress(awake ? hold : std::nullopt);
        } catch (const std::exception &error) {
            if (!shortcut_failure(error))
                throw;
            shortcut_animation_error_ = true;
            animation_status_ = "Shortcut animation unavailable: " + std::string(error.what());
            host_.log(*animation_status_);
        }
    }
    if (auto error = host_.keyboard_error()) {
        keyboard_status_ = *error;
        host_.log(*error);
    }
    if (show_settings_) {
        send_op("show_settings");
        show_settings_ = false;
    }
    bool restart = false;
    for (auto &event : host_.events()) {
        const auto op = json::find(event.get(), "op");
        if (!op)
            continue;
        const auto name = json::string(op);
        if (name == "keyboard_state") {
            exact_fields(event.get(), {"op", "time", "visible", "gaze_valid", "gaze_on_keyboard"},
                         "Invalid native keyboard observation.");
            keyboard_.observe({json::number(json::field(event.get(), "time")),
                               json::boolean(json::field(event.get(), "visible")),
                               json::boolean(json::field(event.get(), "gaze_valid")),
                               json::boolean(json::field(event.get(), "gaze_on_keyboard"))},
                              raw_());
        } else if (name == "pointer_enabled") {
            exact_fields(event.get(), {"op", "enabled"}, "invalid pointer activation event");
            ui_enabled_ = json::boolean(json::field(event.get(), "enabled")) && awake && !capture_;
        } else if (name == "origin_changed") {
            if (session_)
                session_->rebase(json::numbers<12>(json::field(event.get(), "transform")));
        } else if (name == "action")
            restart = action(event.get()) || restart;
    }
    if (!running_)
        return;
    const bool camera_ready =
        allow && options_.steam_shortcuts &&
        (!options_.tracking_enabled || (!restart && session_ && session_->acquired()));
    const bool camera_conflict = options_.pointer_buttons == CameraSwitchButton;
    if (!options_.steam_shortcuts || !connected)
        camera_status_.reset();
    if (auto error = host_.camera_switch_error(camera_ready && !camera_conflict)) {
        camera_status_ = *error;
        host_.log(*error);
    }
    if (camera_requested && camera_ready) {
        try {
            if (camera_conflict)
                throw std::runtime_error("Choose a different UI control button to use Steam + L1.");
            host_.shortcut("camera_switch");
            camera_status_.reset();
        } catch (const std::runtime_error &error) {
            camera_status_ = error.what();
            host_.log(*camera_status_);
        }
    }
    try {
        if (restart || !options_.tracking_enabled)
            tracking_resume_.reset();
        if (session_ && (!options_.tracking_enabled || !awake || !connected)) {
            if (options_.tracking_enabled && !restart)
                tracking_resume_ = session_->checkpoint();
            send_op("hide");
            send_op("input_off");
            ui_enabled_ = false;
            close_session();
            tracking_problem_.reset();
        }
        if (restart) {
            retry_ = 0;
            if (session_)
                session_->acquire();
        }
        if (!options_.tracking_enabled)
            status_ = "Tracking disabled.";
        else if (!awake)
            status_ = "Waiting for Frame's head tracking. Put Frame on. If its own passthrough "
                      "also fails, restart Frame.";
        else if (!connected)
            status_ = "Waiting for gamepad. Connect it to Frame by Bluetooth.";
        if (options_.tracking_enabled && awake && connected && !session_ &&
            monotonic_() >= retry_) {
            session_ = host_.track(options_, tracking_resume_);
            if (!session_)
                throw std::runtime_error("tracking session was not created");
            tracking_resume_.reset();
        }
        const auto typing = keyboard_.update(latest_input_,
                                             session_ && options_.tracking_enabled && awake &&
                                                 ui_enabled_ && !capture_,
                                             monotonic_(), raw_());
        if (session_) {
            if (capture_) {
                session_->guide.hold_started.reset();
                session_->guide.last_input.reset();
            }
            session_->controls(latest_input_, ui_enabled_ && !capture_ && !typing, typing);
            auto pose = session_->step(reports);
            tracking_problem_.reset();
            if (pose)
                host_.send(pose_message(*pose));
            if (auto presentation = session_->presentation())
                host_.send(std::move(presentation));
            status_ = session_->status;
        }
    } catch (const std::exception &error) {
        status_ = error.what();
        const bool asleep = status_.rfind("Headset asleep", 0) == 0;
        bool reconnect = dynamic_cast<const CameraDisconnected *>(&error) != nullptr;
        if (session_ && options_.tracking_enabled && !restart)
            tracking_resume_ = session_->checkpoint();
        send_op("hide");
        send_op("input_off");
        ui_enabled_ = false;
        keyboard_.update(std::nullopt, false, monotonic_(), raw_());
        try {
            close_session();
        } catch (const std::exception &cleanup) {
            status_ += " Cleanup: " + std::string(cleanup.what());
            reconnect = false;
        }
        tracking_problem_ = reconnect ? std::nullopt : std::optional(status_);
        retry_ = reconnect ? 0 : monotonic_() + (asleep ? .5 : retryable(error) ? 5 : 15);
        host_.log(status_);
    }
    if (keyboard_.typing != last_keyboard_typing_) {
        auto event = message("keyboard_typing");
        put(event.get(), "active", keyboard_.typing);
        host_.send(std::move(event));
        last_keyboard_typing_ = keyboard_.typing;
    }
    const auto now = monotonic_();
    if (keyboard_.observation && keyboard_.observation->visible && now - last_keyboard_log_ >= 1) {
        auto record = message("keyboard_health");
        put(record.get(), "typing", keyboard_.typing);
        const auto &observation = *keyboard_.observation;
        json_object_object_add(record.get(), "time", json_object_new_double(observation.time));
        put(record.get(), "visible", observation.visible);
        put(record.get(), "gaze_valid", observation.gaze_valid);
        put(record.get(), "gaze_on_keyboard", observation.gaze_on_keyboard);
        host_.log(json::encode(record.get()));
        last_keyboard_log_ = now;
    }
    if (now - last_status_ >= .2) {
        host_.debug_state(awake, connected, bool(session_), session_ && session_->acquired(),
                          status_);
        if (!session_) {
            if (options_.tracking_enabled && connected && awake && now < retry_)
                host_.send(tracking_feedback(raw_(), "error",
                                             "Tracking paused. Keep the gamepad connected. "
                                             "Controller Utils shows the error and retry status."));
            else {
                auto feedback = message("tracking_feedback");
                put(feedback.get(), "active", false);
                host_.send(std::move(feedback));
            }
        }
        auto value = options_.to_json();
        put(value.get(), "op", std::string("status"));
        const std::array<const std::optional<std::string> *, 7> problems{
            &tracking_problem_, &input_status_,     &shortcut_status_, &camera_status_,
            &system_status_,    &animation_status_, &keyboard_status_};
        const auto count = std::count_if(problems.begin(), problems.end(),
                                         [](const auto *problem) { return bool(*problem); });
        std::string display = tracking_problem_.value_or(status_);
        if (count) {
            display.clear();
            for (const auto *problem : problems) {
                if (!*problem)
                    continue;
                if (!display.empty())
                    display += " | ";
                display += ui_text(**problem, count > 1 ? 37 : 240);
            }
        }
        put(value.get(), "message", ui_text(display, 240));
        put(value.get(), "enabled", options_.tracking_enabled);
        put(value.get(), "gamepad_connected", connected);
        const auto *input = session_ ? &session_->input_state() : nullptr;
        put(value.get(), "status_attention",
            count || (input && (input->error ||
                                (keyboard_.typing && input->keyboard_health == "recovering"))));
        put(value.get(), "binding_capture", bool(capture_));
        put(value.get(), "pointer_label", ui_text(binding_label(options_.pointer_buttons), 120));
        if (auto update = host_.update_info(options_.automatic_updates, !awake))
            json_object_object_add(value.get(), "update", update.release());
        host_.send(std::move(value));
        host_.send(host_.app_info());
        last_status_ = now;
    }
}
} // namespace ct
