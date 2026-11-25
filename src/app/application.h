#pragma once
#include "settings.h"
#include "interaction/gestures.h"
#include "tracking/session_view.h"
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
    virtual std::unique_ptr<TrackingSessionView>
    track(const Settings &options, const std::optional<TrackingResume> &resume) = 0;
    virtual void shortcut(const std::string &action) = 0;
    virtual std::optional<std::string> camera_switch_error(bool) {
        return {};
    }
    virtual bool system_input(bool enabled) = 0;
    virtual void shortcut_progress(std::optional<double> amount) = 0;
    virtual void request_keyboard() = 0;
    virtual std::optional<std::string> keyboard_error() = 0;
    virtual json::Value app_info() = 0;
    virtual json::Value update_info(bool, bool) {
        return {};
    }
    virtual void update(bool) {}
    virtual void debug_action(bool) {}
    virtual void debug_state(bool, bool, bool, bool, const std::string &) {}
    virtual void log(const std::string &message) = 0;
};

class Application {
    ApplicationHost &host_;
    Settings options_;
    std::function<double()> monotonic_, raw_;
    std::unique_ptr<TrackingSessionView> session_;
    std::optional<TrackingResume> tracking_resume_;
    SteamShortcuts gestures_;
    KeyboardMode keyboard_;
    std::optional<BindingCapture> capture_;
    std::optional<ControllerInput> latest_input_;
    std::optional<std::string> input_status_, tracking_problem_;
    std::optional<std::string> shortcut_status_, camera_status_, animation_status_,
        keyboard_status_, system_status_;
    std::string status_;
    bool running_ = true, show_settings_, ui_enabled_ = false;
    bool was_connected_ = false, last_keyboard_typing_ = false;
    bool shortcut_animation_error_ = false;
    double retry_ = 0, last_status_ = 0, last_keyboard_log_ = 0;
    bool action(json_object *event);
    void send_op(const char *op);
    void close_session();

  public:
    Application(ApplicationHost &host, const Settings &options, bool background = false,
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
