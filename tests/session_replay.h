#pragma once
#include "filter_fixture.h"
#include "fake_controller.h"
#include "controller/report.h"
#include "steamvr/runtime.h"
#include "steamvr/steam_ui.h"
#include "platform/linux/child.h"
#include <memory>

namespace session_replay {
namespace j = ct::json;
struct ChildState {
    std::vector<j::Value> pending;
    bool stopped = false, confirm = true;
};
class SteamUi final : public ct::SteamUiSession {
    std::shared_ptr<ChildState> state_;
    double &now_;
    json_object *&events_;

  public:
    SteamUi(std::shared_ptr<ChildState> state, double &now, json_object *&events)
        : state_(std::move(state)), now_(now), events_(events) {}
    bool running() const override {
        return !state_->stopped;
    }
    void send(const ct::FilterRequest &request) override {
        auto record = json_object_new_array();
        json_object_array_add(record, json_object_new_string("filter"));
        json_object_array_add(record, ct::test::filter_request_json(request).release());
        json_object_array_add(events_, record);
        if (state_->confirm) {
            auto reply = j::parse(R"({"op":"input_filter_state","serial":"TEST1234"})");
            json_object_object_add(reply.get(), "active", json_object_new_boolean(request.enabled));
            json_object_object_add(reply.get(), "index",
                                   json_object_new_int(request.enabled ? 0 : -1));
            json_object_object_add(reply.get(), "monotonic_time", json_object_new_double(now_));
            state_->pending.push_back(std::move(reply));
        }
    }
    std::vector<ct::FilterEvent> poll() override {
        std::vector<ct::FilterEvent> result;
        for (const auto &event : std::exchange(state_->pending, {}))
            if (auto value = ct::test::filter_event(event.get()))
                result.push_back(std::move(*value));
        return result;
    }
    void close() override {
        state_->stopped = true;
    }
};
class Runtime final : public ct::RuntimeView {
  public:
    bool awake = true;
    ct::RigidPose head{1, 0, 0, 0, 0, 1, 0, 1.6, 0, 0, 1, 0};
    std::optional<unsigned> dashboard_pointer;
    bool cursor_available = false, cursor_active = false;
    std::string serial(uint32_t index) override {
        return index == 0 ? "HEADSET" : "controllerutils-sc26-input-TEST1234";
    }
    std::optional<ct::RigidPose> head_pose(int = 1) override {
        return awake ? std::optional<ct::RigidPose>(head) : std::nullopt;
    }
    std::optional<unsigned> primary_dashboard_device() override {
        return dashboard_pointer;
    }
    bool set_menu_cursor(ct::MenuCursor mode) override {
        cursor_active = mode == ct::MenuCursor::Touchpad && cursor_available;
        return cursor_active;
    }
};
inline ct::ControllerInput input(json_object *value) {
    ct::ControllerInput result;
    result.received = j::number(j::field(value, "received"));
    auto &r = result.report;
    r.sensor_tick = j::unsigned_integer(j::field(value, "sensor_tick"));
    r.acceleration = j::numbers<3>(j::field(value, "acceleration"));
    r.angular_velocity = j::numbers<3>(j::field(value, "angular_velocity"));
    r.buttons = uint32_t(j::integer(j::field(value, "buttons")));
    r.triggers = j::numbers<2>(j::field(value, "triggers"));
    r.right_stick = j::numbers<2>(j::field(value, "right_stick"));
    r.left_stick = j::numbers<2>(j::field(value, "left_stick"));
    r.left_pad = j::numbers<3>(j::field(value, "left_pad"));
    r.right_pad = j::numbers<3>(j::field(value, "right_pad"));
    return result;
}
} // namespace session_replay
