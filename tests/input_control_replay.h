#pragma once
#include "filter_fixture.h"
#include "interaction/input_control.h"
#include <memory>
#include <stdexcept>
#include <type_traits>

namespace input_replay {
namespace j = ct::json;
inline void put(json_object *object, const char *key, json_object *value) {
    json_object_object_add(object, key, value);
}
inline void put(json_object *object, const char *key, bool value) {
    put(object, key, json_object_new_boolean(value));
}
template <class T>
inline void optional(json_object *object, const char *key, const std::optional<T> &value) {
    if constexpr (std::is_integral_v<T>)
        put(object, key, value ? json_object_new_int64(*value) : nullptr);
    else
        put(object, key, value ? json_object_new_double(*value) : nullptr);
}
class Backend final : public ct::InputBackend {
  public:
    bool present = true, running = true, accept_release = true;
    std::string filter_failure, helper_failure, release_failure;
    std::optional<unsigned> device = 3;
    j::Value events{json_object_new_array()};
    std::vector<ct::FilterEvent> pending;
    bool has_filter() const override {
        return present;
    }
    bool filter_running() const override {
        return running;
    }
    void send_filter(const ct::FilterRequest &request) override {
        if (!filter_failure.empty())
            throw std::runtime_error(filter_failure);
        auto event = json_object_new_array();
        json_object_array_add(event, json_object_new_string("filter"));
        json_object_array_add(event, ct::test::filter_request_json(request).release());
        json_object_array_add(events.get(), event);
    }
    std::vector<ct::FilterEvent> poll_filter() override {
        return std::exchange(pending, {});
    }
    void helper_command(uint8_t command) override {
        if (!helper_failure.empty())
            throw std::runtime_error(helper_failure);
        auto event = json_object_new_array();
        json_object_array_add(event, json_object_new_string("helper"));
        json_object_array_add(event, json_object_new_int(command));
        json_object_array_add(events.get(), event);
    }
    bool release_controls() override {
        if (!release_failure.empty())
            throw std::runtime_error(release_failure);
        auto event = json_object_new_array();
        json_object_array_add(event, json_object_new_string("release_native"));
        json_object_array_add(events.get(), event);
        return accept_release;
    }
    std::optional<unsigned> pointer_device(const std::string &serial) override {
        if (serial != "controllerutils-sc26-input-TEST1234")
            throw std::runtime_error("wrong pointer identity");
        return device;
    }
};
inline j::Value snapshot(ct::InputControl &control, Backend &backend, std::optional<bool> result,
                         const std::string &error) {
    j::Value output(json_object_new_object());
    const auto &s = control.state;
    put(output.get(), "result", result ? json_object_new_boolean(*result) : nullptr);
    put(output.get(), "error", error.empty() ? nullptr : json_object_new_string(error.c_str()));
    put(output.get(), "events", json_object_get(backend.events.get()));
    auto state = json_object_new_object();
    put(output.get(), "state", state);
    put(state, "input_error", s.error ? json_object_new_string(s.error->c_str()) : nullptr);
    put(state, "keyboard_health", json_object_new_string(s.keyboard_health.c_str()));
    put(state, "input_enabled", s.enabled);
    put(state, "keyboard_typing", s.typing);
    put(state, "input_buttons", json_object_new_int64(s.buttons));
    optional(state, "input_received", s.received);
    optional(state, "input_raw_received", s.raw_received);
    put(state, "filter_requested", s.filter_requested);
    put(state, "filter_active", s.filter_active);
    put(state, "filter_typing", s.filter_typing);
    optional(state, "filter_index", s.filter_index);
    optional(state, "filter_confirmed", s.filter_confirmed);
    optional(state, "filter_epoch", s.filter_epoch);
    optional(state, "gate_index", s.gate_index);
    optional(state, "gate_confirmed", s.gate_confirmed);
    optional(state, "gate_confirmed_index", s.gate_confirmed_index);
    optional(state, "gate_epoch", s.gate_epoch);
    put(state, "last_filter_heartbeat", json_object_new_double(s.last_filter_heartbeat));
    put(state, "last_gate_heartbeat", json_object_new_double(s.last_gate_heartbeat));
    put(state, "native_controls_active", s.native_active);
    put(state, "beam_hidden", s.beam_hidden);
    optional(state, "beam_device_index", s.beam_device);
    put(state, "last_beam_heartbeat", json_object_new_double(s.last_beam_heartbeat));
    return output;
}
inline j::Value replay(json_object *request) {
    Backend backend;
    double now = 100;
    ct::InputControl control(backend, "TEST1234", [&] { return now; }, [&] { return now + 1000; });
    j::Value output(json_object_new_array());
    auto steps = j::field(request, "steps");
    for (size_t i = 0; i < j::array_size(steps); ++i) {
        auto step = j::item(steps, i);
        now = j::number(j::field(step, "now"));
        backend.events.reset(json_object_new_array());
        std::optional<bool> result;
        std::string error;
        try {
            if (auto options = j::find(step, "backend")) {
                if (auto v = j::find(options, "present"))
                    backend.present = j::boolean(v);
                if (auto v = j::find(options, "running"))
                    backend.running = j::boolean(v);
                if (auto v = j::find(options, "accept_release"))
                    backend.accept_release = j::boolean(v);
                if (auto v = j::find(options, "filter_failure"))
                    backend.filter_failure = j::string(v);
                if (auto v = j::find(options, "helper_failure"))
                    backend.helper_failure = j::string(v);
                if (auto v = j::find(options, "release_failure"))
                    backend.release_failure = j::string(v);
            }
            if (auto value = j::find(step, "gate_ready"))
                control.state.gate_ready = j::boolean(value);
            if (auto value = j::find(step, "beam_ready"))
                control.state.beam_ready = j::boolean(value);
            if (auto value = j::find(step, "native_active"))
                control.state.native_active = j::boolean(value);
            if (auto value = j::find(step, "events")) {
                for (size_t n = 0; n < j::array_size(value); ++n)
                    if (auto event = ct::test::filter_event(j::item(value, n)))
                        backend.pending.push_back(*event);
            }
            if (auto value = j::find(step, "ack"))
                control.gate_ack(int(j::integer(j::field(value, "index"))),
                                 j::number(j::field(value, "time")));
            if (auto value = j::find(step, "controls")) {
                std::optional<ct::ControllerInput> input;
                if (auto report = j::find(value, "input")) {
                    input.emplace();
                    input->received = j::number(j::field(report, "received"));
                    input->report.buttons = uint32_t(j::integer(j::field(report, "buttons")));
                    input->report.triggers = j::numbers<2>(j::field(report, "triggers"));
                    input->report.right_stick = j::numbers<2>(j::field(report, "right_stick"));
                    input->report.left_pad = j::numbers<3>(j::field(report, "left_pad"));
                    input->report.right_pad = j::numbers<3>(j::field(report, "right_pad"));
                }
                control.controls(input, j::boolean(j::field(value, "enabled")),
                                 j::find(value, "typing") && j::boolean(j::field(value, "typing")));
            }
            if (auto value = j::find(step, "poll"))
                result = control.poll(now, j::boolean(value));
            if (auto value = j::find(step, "fail"))
                control.fail(j::string(value));
            if (auto value = j::find(step, "release"))
                control.release(j::boolean(value));
            if (auto value = j::find(step, "beam"); value && j::boolean(value))
                control.update_beam(now);
        } catch (const std::exception &failure) {
            error = failure.what();
        }
        json_object_array_add(output.get(), snapshot(control, backend, result, error).release());
    }
    return output;
}
} // namespace input_replay
