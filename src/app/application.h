#pragma once
#include "settings.h"
#include "interaction/gestures.h"
#include "tracking/session.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ct {
class ApplicationHost {
  public:
    virtual ~ApplicationHost() = default;
    virtual void send(json::Value message) = 0;
    virtual std::vector<json::Value> events() = 0;
    virtual void save(const Settings &options) = 0;
    virtual void autostart(bool enabled) = 0;
    virtual std::unique_ptr<TrackingSessionView> track(const Settings &options) = 0;
    virtual void shortcut(const std::string &action) = 0;
    virtual std::optional<std::string> camera_switch_error(bool) {
        return {};
    }
    virtual bool system_input(bool enabled) = 0;
    virtual void shortcut_progress(std::optional<double> amount) = 0;
    virtual void request_keyboard() = 0;
    virtual std::optional<std::string> keyboard_error() = 0;
    virtual void check_updates() = 0;
    virtual bool recovery_active() const {
        return false;
    }
    virtual std::string recovery_message() const {
        return "Tracking disabled after repeated SteamVR startup failures.";
    }
    virtual void open_uninstaller() {
        throw std::runtime_error("Uninstaller is unavailable");
    }
    virtual void retry_recovery() {
        throw std::runtime_error("Recovery is unavailable");
    }
    virtual json::Value updates(bool automatic) = 0;
    virtual void request_permission_inspection() = 0;
    virtual void request_permission_repair() = 0;
    virtual json::Value permission_status() = 0;
    virtual void confirm_update() = 0;
    virtual void log(const std::string &message) = 0;
};

class Application {
    ApplicationHost &host_;
    Settings options_;
    std::function<double()> monotonic_, raw_;
    std::unique_ptr<TrackingSessionView> session_;
    SteamShortcuts gestures_;
    KeyboardMode keyboard_;
    std::optional<BindingCapture> capture_;
    std::optional<ControllerInput> latest_input_;
    std::optional<std::string> runtime_problem_, input_status_, tracking_problem_, recovery_error_;
    std::optional<std::string> shortcut_status_, camera_status_, animation_status_,
        keyboard_status_, system_status_;
    std::string status_;
    bool running_ = true, show_settings_, ui_enabled_ = false;
    bool was_connected_ = false, last_keyboard_typing_ = false;
    bool shortcut_animation_error_ = false, update_confirmed_ = false;
    double startup_, retry_ = 0, last_status_ = 0, last_keyboard_log_ = 0;
    bool action(json_object *event);
    void send_op(const char *op);
    void close_session();

  public:
    Application(ApplicationHost &host, const Settings &options,
                std::optional<std::string> runtime_problem = std::nullopt, bool background = false,
                std::function<double()> monotonic = {}, std::function<double()> raw = {});
    Application(const Application &) = delete;
    Application &operator=(const Application &) = delete;
    ~Application();
    void step(bool awake, bool connected, const std::vector<ControllerInput> &reports);
    void show_settings() {
        show_settings_ = true;
    }
    void stop() {
        running_ = false;
    }
    bool running() const {
        return running_;
    }
    double cadence() const {
        return session_ ? .004 : .02;
    }
    const Settings &options() const {
        return options_;
    }
    void close();
};

bool retryable(const std::exception &error);
} // namespace ct
