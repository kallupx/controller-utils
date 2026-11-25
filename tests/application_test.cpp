#include "app_replay.h"
#include "controller/reader.h"
#include "platform/linux/error.h"
#include <cassert>
#include <iostream>

namespace {
namespace j = ct::json;
size_t count(const app_replay::Host &host, const char *kind) {
    size_t result = 0;
    for (size_t i = 0; i < j::array_size(host.trace.get()); ++i)
        result += j::string(j::item(j::item(host.trace.get(), i), 1)) == kind;
    return result;
}
template <class Function> void rejects(Function function) {
    bool rejected = false;
    try {
        function();
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert(rejected);
}
void event_boundary(const std::string &event) {
    app_replay::Host host;
    auto current = j::parse("{}");
    host.current = current.get();
    ct::Application app(host, {}, true, [&] { return host.now; }, [&] { return host.now + 1000; });
    app.step(true, true, {});
    assert(count(host, "track") == 1);
    current = j::parse("{\"events\":[" + event + "]}");
    host.current = current.get();
    rejects([&] { app.step(true, true, {}); });
    app.close();
    app.close();
    assert(count(host, "close") == 1);
    assert(!app.running());
}
} // namespace
int main() {
    {
        app_replay::Host host;
        auto current = j::parse("{}");
        host.current = current.get();
        host.checkpoint = ct::TrackingResume{"HEADSET", "CONTROLLER", {0, 1, -.4}, {0, 1, 0}, {}};
        ct::Application app(
            host, {}, true, [&] { return host.now; }, [&] { return host.now + 1000; });
        app.step(true, true, {});
        assert(!host.requested_resume);
        host.now += .25;
        app.step(false, false, {});
        assert(j::encode(host.trace.get()).find("Waiting for Frame's head tracking") !=
               std::string::npos);
        host.now += 60;
        app.step(true, true, {});
        assert(host.requested_resume && host.requested_resume->led == host.checkpoint->led);
        app.step(true, false, {});
        app.step(false, false, {});
        app.step(true, true, {});
        assert(host.requested_resume && host.requested_resume->led == host.checkpoint->led);
        app.step(false, false, {});
        current = j::parse(R"({"events":[{"op":"action","action":"acquire"}]})");
        host.current = current.get();
        app.step(true, true, {});
        assert(!host.requested_resume);
    }
    for (bool enabled : {false, true}) {
        app_replay::Host host;
        auto current = j::parse("{}");
        host.current = current.get();
        ct::Settings options;
        options.steam_shortcuts = enabled;
        ct::Application app(
            host, options, true, [&] { return host.now; }, [&] { return host.now + 1000; });
        ct::ControllerInput input;
        for (uint32_t buttons : {0u, 0x10000u, 0x10200u, 0x10200u, 0x10000u, 0u}) {
            host.now += .01;
            input.received = host.now;
            input.report.buttons = buttons;
            app.step(true, true, {input});
        }
        assert(count(host, "shortcut") == unsigned(enabled));
        for (size_t i = 0; i < j::array_size(host.trace.get()); ++i) {
            const auto row = j::item(host.trace.get(), i);
            if (j::string(j::item(row, 1)) == "shortcut")
                assert(j::string(j::item(row, 2)) == "screenshot");
        }
        assert(count(host, "keyboard_request") == 0);
        app.close();
    }
    {
        class CameraHost final : public app_replay::Host {
          public:
            std::optional<std::string> error;
            std::optional<std::string> camera_switch_error(bool enabled) override {
                auto result = std::exchange(error, std::nullopt);
                return enabled ? result : std::nullopt;
            }
        } host;
        auto current = j::parse("{}");
        host.current = current.get();
        host.acquisition_complete = false;
        ct::Application app(
            host, {}, true, [&] { return host.now; }, [&] { return host.now + 1000; });
        const auto step = [&](uint32_t buttons) {
            host.now += .05;
            ct::ControllerInput input;
            input.received = host.now;
            input.report.buttons = buttons;
            app.step(true, true, {input});
        };
        const auto attention = [&] {
            for (size_t i = j::array_size(host.trace.get()); i > 0; --i) {
                const auto row = j::item(host.trace.get(), i - 1);
                if (j::string(j::item(row, 1)) != "send")
                    continue;
                const auto value = j::item(row, 2);
                if (j::string(j::field(value, "op")) == "status")
                    return j::boolean(j::field(value, "status_attention"));
            }
            throw std::logic_error("status was not published");
        };
        constexpr uint32_t Camera = 0x10000 | ct::CameraSwitchButton;
        step(0);
        step(Camera);
        assert(count(host, "shortcut") == 0);
        host.acquisition_complete = true;
        step(Camera);
        assert(count(host, "shortcut") == 0);
        step(0);
        step(Camera);
        assert(count(host, "shortcut") == 1);

        host.error = "The passthrough camera did not switch.";
        host.now += .2;
        step(0);
        assert(attention());
        current = j::parse(R"({"events":[{"op":"action","action":"acquire"}]})");
        host.current = current.get();
        step(Camera);
        assert(count(host, "acquire") == 1 && count(host, "shortcut") == 1);

        current = j::parse(R"({"events":[{"op":"action","action":"steam_shortcuts"}]})");
        host.current = current.get();
        host.now += .2;
        step(0);
        assert(!app.options().steam_shortcuts && !attention());
    }
    {
        app_replay::Host host;
        auto current = j::parse("{}");
        host.current = current.get();
        ct::Application app(
            host, {}, true, [&] { return host.now; }, [&] { return host.now + 1000; });
        for (bool enabled : {true, false, true}) {
            current =
                j::parse(std::string("{\"events\":[{\"op\":\"pointer_enabled\",\"enabled\":") +
                         (enabled ? "true" : "false") + "}]}");
            host.current = current.get();
            host.now += .01;
            ct::ControllerInput input;
            input.received = host.now;
            app.step(true, true, {input});
            assert(count(host, "track") == 1 && count(host, "close") == 0);
            bool observed = false;
            for (size_t i = j::array_size(host.trace.get()); i > 0; --i) {
                auto row = j::item(host.trace.get(), i - 1);
                if (j::string(j::item(row, 1)) != "controls")
                    continue;
                assert(j::boolean(j::item(j::item(row, 2), 1)) == enabled);
                observed = true;
                break;
            }
            assert(observed);
        }
        app.close();
    }
    {
        app_replay::Host host;
        auto current = j::parse("{}");
        host.current = current.get();
        ct::Application app(
            host, {}, true, [&] { return host.now; }, [&] { return host.now + 1000; });
        ct::ControllerInput input;
        input.received = host.now;
        app.step(true, true, {input});
        std::vector<ct::ControllerInput> batch;
        for (uint32_t buttons : {0x10000u, 0x10004u, 0x10000u, 0u}) {
            input.received += .01;
            input.report.buttons = buttons;
            batch.push_back(input);
        }
        host.now = input.received;
        app.step(true, true, batch);
        assert(count(host, "keyboard_request") == 1 && count(host, "shortcut") == 0);
        app.close();
    }
    {
        app_replay::Host host;
        auto current = j::parse(
            R"({"fail":{"progress":{"kind":"runtime","message":"animation unavailable"}}})");
        host.current = current.get();
        ct::Settings options;
        options.steam_shortcuts = true;
        ct::Application app(
            host, options, true, [&] { return host.now; }, [&] { return host.now + 1000; });
        ct::ControllerInput input;
        input.received = host.now;
        app.step(true, true, {input});
        input.report.buttons = 0x10000;
        for (int i = 0; i < 13; ++i) {
            host.now += .1;
            input.received = host.now;
            app.step(true, true, {input});
        }
        input.report.buttons = 0;
        host.now += .1;
        input.received = host.now;
        app.step(true, true, {input});
        assert(count(host, "progress") == 1);
        assert(count(host, "shortcut") == 1);
        assert(count(host, "track") == 1);
        bool problem_displayed = false;
        for (size_t i = 0; i < j::array_size(host.trace.get()); ++i) {
            auto row = j::item(host.trace.get(), i);
            if (j::string(j::item(row, 1)) != "send")
                continue;
            auto value = j::item(row, 2);
            if (j::string(j::field(value, "op")) == "status") {
                const auto text = j::string(j::field(value, "message"));
                problem_displayed |=
                    text.find("Shortcut animation unavailable") != std::string::npos;
            }
        }
        assert(problem_displayed);
        assert(count(host, "controls") > 0);
        app.close();
    }
    {
        app_replay::Host host;
        auto current =
            j::parse(R"({"fail":{"track":{"kind":"runtime","message":"camera offline"}}})");
        host.current = current.get();
        ct::Application app(
            host, {}, true, [&] { return host.now; }, [&] { return host.now + 1000; });
        app.step(true, true, {});
        bool problem_displayed = false;
        for (size_t i = 0; i < j::array_size(host.trace.get()); ++i) {
            auto row = j::item(host.trace.get(), i);
            if (j::string(j::item(row, 1)) != "send")
                continue;
            auto value = j::item(row, 2);
            if (j::string(j::field(value, "op")) == "status") {
                const auto text = j::string(j::field(value, "message"));
                problem_displayed |= text.find("camera offline") != std::string::npos;
            }
        }
        assert(problem_displayed);
        app.close();
    }
    assert(ct::retryable(ct::ConnectionError("disconnected")));
    assert(ct::retryable(ct::TimeoutError("late")));
    assert(ct::retryable(std::system_error(EIO, std::generic_category())));
    for (auto message : {"Headset asleep", "Camera worker stopped.", "Controller IMU stopped."})
        assert(ct::retryable(std::runtime_error(message)));
    assert(!ct::retryable(std::runtime_error("invalid camera calibration")));
    assert(!ct::retryable(std::invalid_argument("malformed input")));
    for (
        const auto event :
        {R"({"op":"pointer_enabled","enabled":1})",
         R"({"op":"pointer_enabled","enabled":true,"extra":true})", R"({"op":"pointer_enabled"})",
         R"({"op":"keyboard_state","time":1100,"visible":true,"gaze_valid":false,"gaze_on_keyboard":true})",
         R"({"op":"keyboard_state","time":1100.021,"visible":true,"gaze_valid":true,"gaze_on_keyboard":true})",
         R"({"op":"keyboard_state","time":1100,"visible":1,"gaze_valid":true,"gaze_on_keyboard":true})",
         R"({"op":"keyboard_state","time":1100,"visible":true,"gaze_valid":true})",
         R"({"op":"origin_changed","transform":[1,0]})"})
        event_boundary(event);
    for (unsigned cycle = 0; cycle < 100; ++cycle) {
        app_replay::Host host;
        auto current = j::parse("{}");
        host.current = current.get();
        ct::Application app(
            host, {}, true, [&] { return host.now; }, [&] { return host.now + 1000; });
        app.step(true, false, {});
        assert(count(host, "track") == 0 && app.cadence() == .02);
        app.step(true, true, {});
        assert(count(host, "track") == 1 && app.cadence() == .004);
        app.step(false, true, {});
        assert(count(host, "close") == 1 && app.cadence() == .02);
        app.step(true, true, {});
        assert(count(host, "track") == 2);
        app.show_settings();
        app.step(true, true, {});
        app.stop();
        const auto records = j::array_size(host.trace.get());
        app.step(true, true, {});
        assert(j::array_size(host.trace.get()) == records);
        app.close();
        app.close();
        assert(count(host, "close") == 2);
    }
    {
        app_replay::Host host;
        auto current = j::parse("{}");
        host.current = current.get();
        ct::Application app(
            host, {}, true, [&] { return host.now; }, [&] { return host.now + 1000; });
        app.step(true, true, {});
        rejects([&] { app.step(true, true, std::vector<ct::ControllerInput>(257)); });
        current = j::parse(
            R"({"fail":{"step":{"kind":"connection","message":"input lost"},"close":{"kind":"runtime","message":"restore failed"}}})");
        host.current = current.get();
        app.step(true, true, {});
        assert(count(host, "close") == 1 && count(host, "track") == 1);
        current = j::parse("{}");
        host.current = current.get();
        host.now += 4.99;
        app.step(true, true, {});
        assert(count(host, "track") == 1);
        host.now += .02;
        app.step(true, true, {});
        assert(count(host, "track") == 2);
        current = j::parse(R"({"fail":{"step":{"kind":"camera_disconnected","message":""}}})");
        host.current = current.get();
        app.step(true, true, {});
        assert(count(host, "close") == 2);
        current = j::parse("{}");
        host.current = current.get();
        app.step(true, true, {});
        assert(count(host, "track") == 3);
        current = j::parse(
            R"({"fail":{"step":{"kind":"camera_disconnected","message":""},"close":{"kind":"runtime","message":"restore failed"}}})");
        host.current = current.get();
        app.step(true, true, {});
        app.step(true, true, {});
        assert(count(host, "track") == 3 && count(host, "close") == 3);
        app.close();
        assert(count(host, "close") == 3);
    }
    std::cout << "application: event boundaries, failure cleanup, retries and 100 lifecycle cycles "
                 "passed\n";
}
