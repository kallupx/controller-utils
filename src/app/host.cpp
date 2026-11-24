#include "host.h"
#include "tracking/optical_session.h"
#include "platform/linux/files.h"
#include <chrono>
#include <exception>
#include <iostream>

namespace ct {
NativeApplicationHost::NativeApplicationHost(Runtime &runtime, ControllerReader &reader,
                                             const ApplicationLock &lock,
                                             ui::Presentation &presentation,
                                             std::filesystem::path root, std::filesystem::path data,
                                             ChildFactory children, SteamUiFactory steam_ui)
    : runtime_(runtime), reader_(reader), lock_(lock), presentation_(presentation),
      root_(std::move(root)), data_(std::move(data)), debug_(data_),
      version_(json::string(
          json::field(json::parse(read_file(root_ / "release.json", 65536)).get(), "version"))),
      children_(children
                    ? std::move(children)
                    : ChildFactory([directory = root_](const std::vector<std::string> &command) {
                          return std::make_unique<ChildProcess>(command, directory);
                      })),
      steam_ui_(steam_ui ? std::move(steam_ui)
                         : SteamUiFactory([](const std::string &serial, bool keyboard) {
                               return std::make_unique<SteamUiClient>(serial, keyboard);
                           })),
      registration_(
          *static_cast<vr::IVRApplications *>(runtime_.interface(vr::IVRApplications_Version))) {
    if (!lock_.primary())
        throw std::logic_error("Controller Utils requires its exclusive lifetime lock");
    registration_.identify();
    if (debug_.recording())
        debug_.event("App started version=" + version_);
}
NativeApplicationHost::~NativeApplicationHost() {
    try {
        close();
    } catch (const std::exception &error) {
        log("Controller Utils host shutdown: " + std::string(error.what()));
    }
}
bool NativeApplicationHost::running() {
    return !closed_ && presentation_.running();
}
void NativeApplicationHost::send(json::Value message) {
    if (closed_)
        throw std::logic_error("Renderer is closed");
    auto command = ui::command(message.get());
    if (!command)
        throw std::invalid_argument("Unsupported Controller Utils UI command");
    presentation_.send(std::move(*command));
}
std::vector<json::Value> NativeApplicationHost::events() {
    if (closed_)
        return {};
    std::vector<json::Value> result;
    for (const auto &event : presentation_.events())
        result.push_back(ui::event_json(event));
    return result;
}
void NativeApplicationHost::save(const Settings &options) {
    save_settings(data_ / "settings.json", options);
    if (debug_.recording())
        debug_.event("Settings tracking=" + std::to_string(options.tracking_enabled) +
                     " brightness=" + std::to_string(options.brightness) +
                     " smoothing_ms=" + std::to_string(options.smoothing_ms) +
                     " updates=" + std::to_string(options.automatic_updates));
}
void NativeApplicationHost::autostart(bool enabled) {
    registration_.autostart(enabled);
}
std::unique_ptr<TrackingSessionView>
NativeApplicationHost::track(const Settings &options, const std::optional<TrackingResume> &resume) {
    auto controller = reader_.device();
    if (!controller)
        throw std::runtime_error("Connect a Steam Controller to start tracking.");
    const auto calibration =
        controller_calibration(data_, runtime_.serial(0), controller->serial());
    return std::make_unique<OpticalSession>(
        runtime_, controller, calibration, options, root_, children_, steam_ui_,
        [data = data_](const auto &value) { save_controller_calibration(data, value); }, resume);
}
NativeShortcuts &NativeApplicationHost::shortcuts() {
    if (!shortcuts_) {
        auto backend = std::make_unique<OpenVrShortcutBackend>(runtime_);
        auto shortcuts = std::make_unique<NativeShortcuts>(*backend);
        shortcut_backend_ = std::move(backend);
        shortcuts_ = std::move(shortcuts);
    }
    return *shortcuts_;
}
void NativeApplicationHost::shortcut(const std::string &action) {
    if (action == "dashboard") {
        if (!system_input_ || system_tap_ == UINT32_MAX)
            throw std::runtime_error("Steam button input is unavailable");
        FilterRequest request;
        request.enabled = request.system_only = true;
        request.tap = ++system_tap_;
        system_input_->send(request);
    } else if (action == "recenter")
        shortcuts().send_action(ShortcutAction::Recenter);
    else if (action == "passthrough")
        shortcuts().send_action(ShortcutAction::Passthrough);
    else if (action == "screenshot")
        shortcuts().send_action(ShortcutAction::Screenshot);
    else if (action == "camera_switch") {
        if (!camera_switch_)
            camera_switch_ =
                children_({(root_ / "controller-utils").string(), "--camera-switch-worker"});
        if (!camera_switch_)
            throw std::runtime_error("Camera shortcut could not start.");
    } else
        throw std::invalid_argument("unsupported Frame shortcut");
}
std::optional<std::string> NativeApplicationHost::camera_switch_error(bool enabled) {
    if (!enabled)
        camera_switch_.reset();
    if (!camera_switch_)
        return {};
    try {
        const auto status = camera_switch_->exit_status();
        if (!status)
            return {};
        auto child = std::move(camera_switch_);
        if (*status == 0)
            return {};
        auto messages = child->poll();
        if (messages.size() == 1)
            return json::string(json::field(messages.front().get(), "error"));
    } catch (const std::exception &error) {
        camera_switch_.reset();
        log(error.what());
    }
    return "Camera shortcut did not complete. Try again.";
}
bool NativeApplicationHost::system_input(bool enabled) {
    const auto controller = reader_.device();
    const auto serial = enabled && controller ? controller->serial() : std::string();
    if (serial != system_serial_ || (system_input_ && !system_input_->running())) {
        auto old = std::move(system_input_);
        system_serial_.clear();
        system_confirmed_.reset();
        system_tap_ = 0;
        if (old)
            old->close();
    }
    if (serial.empty())
        return false;
    if (!system_input_) {
        system_input_ = steam_ui_(serial, false);
        if (!system_input_)
            throw std::runtime_error("Steam button input could not start");
        system_serial_ = serial;
    }
    FilterRequest request;
    request.enabled = request.system_only = true;
    request.tap = system_tap_;
    system_input_->send(request);
    for (const auto &event : system_input_->poll()) {
        if (const auto state = std::get_if<FilterState>(&event)) {
            if (state->active)
                system_confirmed_ = state->time;
            else
                system_confirmed_.reset();
        } else if (const auto error = std::get_if<FilterError>(&event)) {
            system_confirmed_.reset();
            throw std::runtime_error(error->message);
        }
    }
    const auto age = system_confirmed_ ? monotonic_time() - *system_confirmed_ : 1e30;
    return age >= -.02 && age <= .5;
}
void NativeApplicationHost::shortcut_progress(std::optional<double> amount) {
    if (amount)
        shortcuts().progress(ShortcutAction::Recenter, *amount);
    else if (shortcuts_)
        shortcuts_->progress(std::nullopt);
}
void NativeApplicationHost::request_keyboard() {
    if (!keyboard_) {
        keyboard_ = steam_ui_("keyboard-shortcut", true);
        if (!keyboard_)
            throw std::runtime_error("Keyboard shortcut helper could not start");
    }
}
std::optional<std::string> NativeApplicationHost::keyboard_error() {
    if (!keyboard_)
        return std::nullopt;
    if (keyboard_->running())
        return std::nullopt;
    auto request = std::move(keyboard_);
    try {
        request->close();
        for (const auto &event : request->poll()) {
            if (const auto error = std::get_if<FilterError>(&event))
                return error->message;
            const auto &state = std::get<FilterState>(event);
            if (!state.keyboard_overlay.empty()) {
                auto overlay = runtime_.overlay;
                vr::VROverlayHandle_t window = vr::k_ulOverlayHandleInvalid;
                if (!overlay || overlay->FindOverlay(state.keyboard_overlay.c_str(), &window) ||
                    !window)
                    throw std::runtime_error("The focused window is no longer available");
                const auto error = overlay->ShowKeyboardForOverlay(
                    window, vr::k_EGamepadTextInputModeNormal,
                    vr::k_EGamepadTextInputLineModeSingleLine,
                    vr::KeyboardFlag_Minimal | vr::KeyboardFlag_HideDoneKey |
                        vr::KeyboardFlag_ShowArrowKeys,
                    "", 1024, "", 0);
                if (error)
                    throw std::runtime_error("Could not open the window keyboard: " +
                                             std::to_string(error));
                overlay->SetKeyboardPositionForOverlay(window, {});
            }
        }
    } catch (const std::exception &error) {
        return error.what();
    }
    return std::nullopt;
}
json::Value NativeApplicationHost::app_info() {
    auto value = json::parse(R"({"op":"app_info"})");
    json_object_object_add(value.get(), "version", json_object_new_string(version_.c_str()));
    json_object_object_add(value.get(), "debug", debug_.info().release());
    return value;
}
void NativeApplicationHost::debug_action(bool save) {
    const bool starting = !save && !debug_.recording();
    debug_.action(save, children_);
    if (starting && debug_.recording()) {
        const auto version = runtime_.system->GetRuntimeVersion();
        debug_.event("SteamVR runtime=" + std::string(version ? version : "unknown"));
    }
}
void NativeApplicationHost::debug_state(bool awake, bool connected, bool session, bool acquired,
                                        const std::string &status) {
    if (debug_.recording())
        debug_.state("Tracking head_pose=" + std::to_string(awake) + " controller=" +
                     std::to_string(connected) + " session=" + std::to_string(session) +
                     " aligned=" + std::to_string(acquired) + " " + status);
}
void NativeApplicationHost::update(bool apply) {
    const auto script = root_ / "frame_update.py";
    if (update_job_ || !std::filesystem::is_regular_file(script))
        return;
    try {
        update_job_ =
            children_({"/usr/bin/systemd-run", "--user", "--quiet", "--collect", "--no-block",
                       "--expand-environment=no", "--unit=controller-utils-frame-update",
                       "/usr/bin/python3", "-B", script.string(), apply ? "apply" : "check"});
        update_started_ = monotonic_time();
        next_update_check_ = update_started_ + 12 * 60 * 60;
        update_error_.clear();
    } catch (const std::exception &error) {
        log(error.what());
        update_error_ = "Couldn’t start the update check.";
        next_update_check_ = monotonic_time() + 60;
    }
}
json::Value NativeApplicationHost::update_info(bool automatic, bool idle) {
    if (!std::filesystem::is_regular_file(root_ / "frame_update.py"))
        return {};
    const auto now = monotonic_time();
    if (!idle)
        update_idle_.reset();
    else if (!update_idle_)
        update_idle_ = now;
    if (update_job_)
        if (const auto status = update_job_->exit_status()) {
            if (*status != 0)
                update_error_ = "Couldn’t start the update. Try again later.";
            update_job_.reset();
        }
    std::string state, message = "Check for a new Controller Utils release.";
    try {
        const auto path = data_ / "frame-update.json";
        if (std::filesystem::is_regular_file(path)) {
            const auto value = json::parse(read_file(path, 4096), 4096);
            state = json::string(json::field(value.get(), "state"));
            message = json::string(json::field(value.get(), "message"));
            if (message.size() > 180)
                message = "Update status is unavailable.";
            if (state == "busy" && std::filesystem::file_time_type::clock::now() -
                                           std::filesystem::last_write_time(path) >
                                       std::chrono::minutes(12)) {
                state.clear();
                message = "The update stopped. Check for updates to retry.";
            }
        }
    } catch (const std::exception &error) {
        log(error.what());
    }
    const bool starting = update_job_ || (update_started_ && now - update_started_ < 2);
    if (automatic && !starting && state != "busy") {
        if (state == "ready" && update_idle_ && now - *update_idle_ >= 30)
            update(true);
        else if (state != "ready" && now >= next_update_check_)
            update(false);
    }
    const bool busy = starting || update_job_ || state == "busy";
    if (!update_error_.empty())
        message = update_error_;
    else if (starting || update_job_)
        message = "Starting update…";
    json::Value value(json_object_new_object());
    json_object_object_add(value.get(), "automatic", json_object_new_boolean(automatic));
    json_object_object_add(value.get(), "busy", json_object_new_boolean(busy));
    json_object_object_add(value.get(), "ready", json_object_new_boolean(state == "ready"));
    json_object_object_add(value.get(), "message", json_object_new_string(message.c_str()));
    return value;
}
void NativeApplicationHost::log(const std::string &message) {
    std::cerr << message << '\n';
    if (debug_.recording() && !message.empty() && message.front() != '{')
        debug_.event("App error: " + message);
}
void NativeApplicationHost::close_keyboard() {
    auto request = std::move(keyboard_);
    if (request)
        request->close();
}
void NativeApplicationHost::close() {
    if (closed_)
        return;
    closed_ = true;
    debug_.event("App stopping");
    camera_switch_.reset();
    std::exception_ptr failure;
    const auto attempt = [&](const auto &operation) {
        try {
            operation();
        } catch (...) {
            if (!failure)
                failure = std::current_exception();
        }
    };
    attempt([&] { close_keyboard(); });
    attempt([&] {
        if (system_input_)
            system_input_->close();
        system_input_.reset();
    });
    presentation_.close();
    auto shortcuts = std::move(shortcuts_);
    attempt([&] {
        if (shortcuts)
            shortcuts->close();
    });
    if (failure)
        std::rethrow_exception(failure);
}
} // namespace ct
