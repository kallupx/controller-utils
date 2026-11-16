#include "host.h"
#include "platform/linux/files.h"
#include "update/transaction.h"
#include "update/repair.h"
#include "update_public_key.h"
#include "steamvr/startup_recovery.h"
#include <spawn.h>
#include <pwd.h>
#include <unistd.h>
#include <thread>
extern char **environ;
#include <cerrno>
#include <exception>
#include <iostream>
#include <system_error>
#include <sys/wait.h>

namespace ct {
NativeApplicationHost::NativeApplicationHost(Runtime &runtime, ControllerReader &reader,
                                             const ApplicationLock &lock,
                                             ui::Presentation &presentation,
                                             std::filesystem::path root, std::filesystem::path data,
                                             ChildFactory children, SteamUiFactory steam_ui,
                                             std::function<void()> repair_verifier)
    : runtime_(runtime), reader_(reader), lock_(lock), presentation_(presentation),
      root_(std::move(root)), data_(std::move(data)),
      children_(children
                    ? std::move(children)
                    : ChildFactory([directory = root_](const std::vector<std::string> &command) {
                          return std::make_unique<ChildProcess>(command, directory);
                      })),
      steam_ui_(steam_ui ? std::move(steam_ui)
                         : SteamUiFactory([](const std::string &serial, bool keyboard) {
                               return std::make_unique<SteamUiClient>(serial, keyboard);
                           })),
      repair_verifier_(std::move(repair_verifier)),
      registration_(
          *static_cast<vr::IVRApplications *>(runtime_.interface(vr::IVRApplications_Version))),
      updates_(root_, data_, [this] {
          if (!backend_)
              throw std::runtime_error("This release needs a native compatibility update");
          return update::capture_requirements({root_}, *backend_);
      }) {
    if (!lock_.primary())
        throw std::logic_error("Native application requires its exclusive lifetime lock");
    try {
        backend_ = installed_backend();
    } catch (const std::exception &error) {
        backend_problem_ = "Native tracking is unavailable. Check for a compatible app update.";
        log("Controller Utils tracking backend: " + std::string(error.what()));
    }
    try {
        const auto user = getpwuid(getuid());
        if (!user || !user->pw_name)
            throw std::runtime_error("Cannot identify tracking user");
        const bool frame_user = std::string(user->pw_name) == "steamos";
        const bool missing = frame_user && !std::filesystem::exists(data_ / "startup-recovery");
        const auto state = startup_recovery(data_);
        recovery_mode_ = missing || state.disabled();
        if (recovery_mode_)
            backend_problem_ =
                missing || state.failed()
                    ? "Tracking recovery is unavailable. Retry or check for an update."
                    : "Tracking disabled after repeated SteamVR startup failures.";
    } catch (const std::exception &error) {
        log("Controller Utils recovery: " + std::string(error.what()));
        recovery_mode_ = true;
        backend_problem_ = "Tracking recovery is unavailable. Retry or check for an update.";
    }
    registration_.identify();
    start_permission_action(false);
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
    if (update_confirmation_) {
        update_confirmation_->poll();
        if (const auto status = update_confirmation_->exit_status()) {
            if (*status != 0)
                log("Update startup confirmation failed. The previous version will be restored "
                    "when SteamVR restarts.");
            update_confirmation_.reset();
        }
    }
    std::vector<json::Value> result;
    for (const auto &event : presentation_.events())
        result.push_back(ui::event_json(event));
    return result;
}
void NativeApplicationHost::save(const Settings &options) {
    save_settings(data_ / "settings.json", options);
}
void NativeApplicationHost::autostart(bool enabled) {
    registration_.autostart(enabled);
}
std::unique_ptr<TrackingSessionView> NativeApplicationHost::track(const Settings &options) {
    if (!backend_)
        throw std::runtime_error(*backend_problem_);
    auto controller = reader_.device();
    if (!controller)
        throw std::runtime_error("Connect a Steam Controller to start tracking.");
    const auto calibration =
        controller_calibration(data_, runtime_.serial(0), controller->serial());
    return std::make_unique<TrackingSession>(
        runtime_, controller, calibration, options, children_, steam_ui_, std::function<double()>{},
        std::function<double()>{}, std::function<void(double)>{},
        [data = data_](const TrackingCalibration &value) {
            save_controller_calibration(data, value);
        });
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
    else
        throw std::invalid_argument("unsupported Frame shortcut");
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
bool NativeApplicationHost::recovery_active() const {
    return recovery_mode_;
}
std::string NativeApplicationHost::recovery_message() const {
    return backend_problem_.value_or("Tracking is in recovery mode.");
}
void NativeApplicationHost::open_uninstaller() {
    const auto script = (root_ / "uninstall.sh").string();
    char program[] = "/usr/bin/konsole", hold[] = "--hold", execute[] = "-e", shell[] = "/bin/sh";
    char *args[] = {program, hold, execute, shell, const_cast<char *>(script.c_str()), nullptr};
    pid_t child = 0;
    const int error = posix_spawn(&child, program, nullptr, nullptr, args, environ);
    if (error)
        throw std::system_error(error, std::generic_category(), "Open uninstaller");
    std::thread([child] {
        int status;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
    }).detach();
}
void NativeApplicationHost::retry_recovery() {
    auto recovery = startup_recovery(data_);
    if (!recovery_mode_)
        throw std::runtime_error("Recovery is not active");
    recovery.retry();
    pid_t child = 0;
    char program[] = "/usr/bin/systemctl";
    char arg1[] = "--user", arg2[] = "--no-block", arg3[] = "restart", arg4[] = "steamvr.service";
    char *args[] = {program, arg1, arg2, arg3, arg4, nullptr};
    const int error = posix_spawn(&child, program, nullptr, nullptr, args, environ);
    if (error) {
        recovery.disable();
        throw std::system_error(error, std::generic_category(), "Restart SteamVR");
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR)
            continue;
        recovery.disable();
        throw std::system_error(errno, std::generic_category(), "Wait for SteamVR restart");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        recovery.disable();
        throw std::runtime_error("Could not restart SteamVR");
    }
}
void NativeApplicationHost::check_updates() {
    updates_.check(true);
}
json::Value NativeApplicationHost::updates(bool automatic) {
    return updates_.poll(automatic).to_json();
}
void NativeApplicationHost::confirm_update() {
    if (repair_)
        throw std::system_error(EBUSY, std::generic_category(),
                                "Waiting for helper permission maintenance");
    update::confirm(root_, data_, lock_);
    if (update::channel(root_) == update::GitHubChannel)
        update_confirmation_ =
            children_({"/usr/bin/sudo", "-n", "/var/lib/controller-utils/controller-utils-updater",
                       "--confirm"});
}
void NativeApplicationHost::verify_permission_updater() {
    if (repair_verifier_)
        repair_verifier_();
    else {
        ProtectedDirectory native("/var/lib/controller-utils");
        (void)update::verify_repair_updater(native, UpdatePublicKey);
    }
}
void NativeApplicationHost::start_permission_action(bool inspection) {
    if (closed_ || repair_)
        return;
    bool updater_verified = false;
    try {
        verify_permission_updater();
        updater_verified = true;
        const auto verify = [this] { verify_permission_updater(); };
        repair_ = inspection ? update::start_inspection_action(children_, verify)
                             : update::start_repair_action(children_, verify);
        repair_started_ = monotonic_time();
        repair_inspection_ = inspection;
        inspection_result_.reset();
        repair_status_ =
            inspection ? "Checking helper permissions." : "Restoring helper permissions.";
    } catch (const std::exception &failure) {
        repair_status_ = updater_verified ? "Permission maintenance is unavailable."
                                          : "A verified reinstall is required.";
        log(std::string("Permission repair: ") + failure.what());
    }
}
void NativeApplicationHost::request_permission_repair() {
    start_permission_action(false);
}
void NativeApplicationHost::request_permission_inspection() {
    start_permission_action(true);
}
std::string NativeApplicationHost::permission_repair_status() {
    try {
        if (repair_) {
            if (monotonic_time() - repair_started_ > 20)
                throw std::runtime_error("Permission maintenance timed out");
            for (const auto &message : repair_->poll()) {
                if (repair_inspection_) {
                    if (json::string(json::field(message.get(), "op")) != "repair-status" ||
                        inspection_result_)
                        throw std::runtime_error("Invalid permission inspection response");
                    const auto value = json::string(json::field(message.get(), "status"));
                    if (value == "intact")
                        inspection_result_ = "Helper permissions are intact.";
                    else if (value == "missing")
                        inspection_result_ = "Helper permissions are missing. Repair is available.";
                    else if (value == "conflict")
                        inspection_result_ = "A system policy conflicts. Review is required.";
                    else if (value == "busy")
                        inspection_result_ = "Another maintenance operation is running.";
                    else if (value == "interrupted")
                        inspection_result_ = "A previous repair was interrupted.";
                    else if (value == "reinstall-required")
                        inspection_result_ = "A verified reinstall is required.";
                    else
                        throw std::runtime_error("Invalid permission inspection status");
                } else
                    throw std::runtime_error("Unexpected permission repair response");
            }
            if (const auto status = repair_->exit_status()) {
                if (WIFEXITED(*status) && WEXITSTATUS(*status) == 0)
                    repair_status_ =
                        repair_inspection_
                            ? inspection_result_.value_or("Permission inspection failed.")
                            : "Helper permissions are ready.";
                else
                    repair_status_ = "Permission maintenance failed. Review the installation.";
                repair_.reset();
            }
        }
    } catch (const std::exception &failure) {
        repair_status_ = "Permission inspection failed.";
        log(std::string("Permission inspection: ") + failure.what());
        repair_.reset();
    }
    return repair_status_;
}
json::Value NativeApplicationHost::permission_status() {
    auto value = json::parse("{}");
    const auto message = permission_repair_status();
    json_object_object_add(value.get(), "op", json_object_new_string("repair_status"));
    json_object_object_add(value.get(), "message", json_object_new_string(message.c_str()));
    json_object_object_add(value.get(), "busy", json_object_new_boolean(bool(repair_)));
    return value;
}
void NativeApplicationHost::log(const std::string &message) {
    std::cerr << message << '\n';
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
    attempt([&] { updates_.close(); });
    attempt([&] {
        if (update_confirmation_)
            update_confirmation_->close("", 1);
        update_confirmation_.reset();
    });
    repair_.reset();
    if (failure)
        std::rethrow_exception(failure);
}
} // namespace ct
