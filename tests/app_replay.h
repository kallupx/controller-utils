#pragma once
#include "app/application.h"
#include "platform/linux/error.h"
#include "session_replay.h"
#include <system_error>

namespace app_replay {
namespace j = ct::json;
class Host : public ct::ApplicationHost {
    class Session final : public ct::TrackingSessionView {
        Host &host_;
        ct::TrackingInputState input_;

      public:
        explicit Session(Host &host) : host_(host) {
            status = "native fixture";
        }
        bool acquired() const override {
            return host_.acquisition_complete;
        }
        std::optional<ct::TrackingResume> checkpoint() const override {
            return host_.checkpoint;
        }
        const ct::TrackingInputState &input_state() const override {
            return input_;
        }
        void controls(const std::optional<ct::ControllerInput> &input, bool enabled,
                      bool typing) override {
            auto value = json_object_new_array();
            json_object_array_add(value, input ? host_.input_json(*input) : nullptr);
            json_object_array_add(value, json_object_new_boolean(enabled));
            json_object_array_add(value, json_object_new_boolean(typing));
            host_.record("controls", value);
        }
        void update_options(const ct::Settings &options) override {
            host_.record("options", options.to_json().release());
        }
        std::optional<ct::BodyPoseMessage> step(const std::vector<ct::ControllerInput> &) override {
            host_.record("step");
            if (auto error = j::find(host_.current, "input_error"))
                input_.error = j::string(error);
            else
                input_.error.reset();
            if (auto health = j::find(host_.current, "keyboard_health"))
                input_.keyboard_health = j::string(health);
            else
                input_.keyboard_health = "off";
            if (auto text = j::find(host_.current, "status"))
                status = j::string(text);
            return ct::BodyPoseMessage{1099.99, 1100., {0, 0, -.4}, 1100., {1, 0, 0, 0}};
        }
        void acquire() override {
            host_.record("acquire");
        }
        void recenter() override {
            host_.record("recenter");
        }
        void rebase(const ct::RigidPose &delta) override {
            host_.record("rebase", j::value(delta));
        }
        j::Value presentation() override {
            auto value = j::find(host_.current, "presentation");
            return j::Value(json_object_get(value));
        }
        void close() override {
            host_.record("close");
        }
    };

  public:
    double now = 100;
    int index = -1;
    json_object *current = nullptr;
    j::Value trace{json_object_new_array()};
    bool acquisition_complete = true;
    bool keyboard_pending = false;
    bool progress_available = false;
    std::optional<ct::TrackingResume> checkpoint, requested_resume;
    static json_object *input_json(const ct::ControllerInput &input) {
        const auto &r = input.report;
        auto result = json_object_new_object();
        json_object_object_add(result, "received", json_object_new_double(input.received));
        json_object_object_add(result, "buttons", json_object_new_int64(r.buttons));
        json_object_object_add(result, "triggers", j::value(r.triggers));
        json_object_object_add(result, "left_pad", j::value(r.left_pad));
        json_object_object_add(result, "right_pad", j::value(r.right_pad));
        json_object_object_add(result, "left_stick", j::value(r.left_stick));
        json_object_object_add(result, "right_stick", j::value(r.right_stick));
        json_object_object_add(result, "sensor_tick", json_object_new_uint64(r.sensor_tick));
        json_object_object_add(result, "acceleration", j::value(r.acceleration));
        json_object_object_add(result, "angular_velocity", j::value(r.angular_velocity));
        return result;
    }
    void record(const char *name, json_object *value = nullptr) {
        auto row = json_object_new_array();
        json_object_array_add(row, json_object_new_int(index));
        json_object_array_add(row, json_object_new_string(name));
        json_object_array_add(row, value);
        json_object_array_add(trace.get(), row);
        auto failures = current ? j::find(current, "fail") : nullptr;
        if (auto error = failures ? j::find(failures, name) : nullptr) {
            const auto text = j::string(j::field(error, "message"));
            const auto kind = j::string(j::field(error, "kind"));
            if (kind == "camera_disconnected")
                throw ct::CameraDisconnected();
            if (kind == "connection")
                throw ct::ConnectionError(text);
            if (kind == "value")
                throw std::invalid_argument(text);
            if (kind == "os")
                throw std::system_error(std::error_code(), text);
            throw std::runtime_error(text);
        }
    }
    void send(j::Value value) override {
        record("send", value.release());
    }
    std::vector<j::Value> events() override {
        std::vector<j::Value> result;
        if (auto events = j::find(current, "events"))
            for (size_t i = 0; i < j::array_size(events); ++i)
                result.emplace_back(json_object_get(j::item(events, i)));
        return result;
    }
    void save(const ct::Settings &options) override {
        record("save", options.to_json().release());
    }
    void autostart(bool enabled) override {
        record("autostart", json_object_new_boolean(enabled));
    }
    std::unique_ptr<ct::TrackingSessionView>
    track(const ct::Settings &, const std::optional<ct::TrackingResume> &resume) override {
        requested_resume = resume;
        record("track");
        return std::make_unique<Session>(*this);
    }
    void shortcut(const std::string &action) override {
        progress_available = true;
        record("shortcut", json_object_new_string(action.c_str()));
    }
    bool system_input(bool enabled) override {
        return enabled;
    }
    void shortcut_progress(std::optional<double> amount) override {
        if (!amount && !progress_available)
            return;
        progress_available = true;
        record("progress", j::value(amount));
    }
    void request_keyboard() override {
        if (!keyboard_pending) {
            record("keyboard_request");
            keyboard_pending = true;
        }
    }
    std::optional<std::string> keyboard_error() override {
        if (keyboard_pending) {
            if (auto error = j::find(current, "keyboard_error")) {
                keyboard_pending = false;
                return j::string(error);
            }
            if (j::find(current, "keyboard_done"))
                keyboard_pending = false;
        }
        return std::nullopt;
    }
    j::Value app_info() override {
        return j::parse(R"({"op":"app_info","version":"0.1.0"})");
    }
    void log(const std::string &text) override {
        try {
            auto parsed = j::parse(text);
            record("log", parsed.release());
        } catch (const std::invalid_argument &) {
            record("log", json_object_new_string(text.c_str()));
        }
    }
};

inline j::Value replay(json_object *request) {
    Host host;
    auto options = ct::Settings::from_json(j::field(request, "options"));
    ct::Application app(
        host, options, j::boolean(j::field(request, "background")), [&] { return host.now; },
        [&] { return host.now + 1000; });
    auto steps = j::field(request, "steps");
    for (size_t i = 0; i < j::array_size(steps); ++i) {
        host.index = int(i);
        host.current = j::item(steps, i);
        host.now = j::number(j::field(host.current, "now"));
        if (auto show = j::find(host.current, "show"); show && j::boolean(show))
            app.show_settings();
        std::vector<ct::ControllerInput> reports;
        if (auto input = j::find(host.current, "report"))
            reports.push_back(session_replay::input(input));
        try {
            app.step(j::boolean(j::field(host.current, "awake")),
                     j::boolean(j::field(host.current, "connected")), reports);
            if (app.running())
                host.record("tick", json_object_new_double(app.cadence()));
        } catch (const std::exception &error) {
            host.record("exception", json_object_new_string(error.what()));
            break;
        }
        if (!app.running())
            break;
    }
    try {
        app.close();
    } catch (const std::exception &error) {
        host.record("exception", json_object_new_string(error.what()));
    }
    return std::move(host.trace);
}
} // namespace app_replay
