#pragma once
#include "filter_fixture.h"
#include "fake_controller.h"
#include "fake_native_queue.h"
#include "tracking/session.h"
#include <memory>

namespace session_replay {
namespace j = ct::json;
struct ChildState {
    std::vector<j::Value> pending;
    bool stopped = false, confirm = true, restore = true;
    std::string exit_error, close_error;
};
class Channel final : public ct::ChildChannel {
    std::shared_ptr<ChildState> state_;
    std::string kind_;
    double &now_;
    json_object *&events_;
    void event(json_object *value) {
        state_->pending.emplace_back(value);
    }

  public:
    Channel(std::shared_ptr<ChildState> state, std::string kind, double &now, json_object *&events)
        : state_(std::move(state)), kind_(std::move(kind)), now_(now), events_(events) {}
    std::optional<int> exit_status() override {
        if (!state_->exit_error.empty())
            throw std::runtime_error(state_->exit_error);
        return state_->stopped ? std::optional<int>(0) : std::nullopt;
    }
    void send(const std::string &bytes) override {
        auto record = json_object_new_array();
        json_object_array_add(record, json_object_new_string(kind_.c_str()));
        if (kind_ == "filter") {
            auto request = j::parse(bytes);
            json_object_array_add(record, json_object_get(request.get()));
            if (state_->confirm) {
                auto reply = json_object_new_object();
                const auto enabled = j::boolean(j::field(request.get(), "enabled"));
                json_object_object_add(reply, "op", json_object_new_string("input_filter_state"));
                json_object_object_add(reply, "active", json_object_new_boolean(enabled));
                json_object_object_add(reply, "index", json_object_new_int(enabled ? 0 : -1));
                json_object_object_add(reply, "serial", json_object_new_string("TEST1234"));
                json_object_object_add(reply, "monotonic_time", json_object_new_double(now_));
                event(reply);
            }
        } else {
            assert(bytes.size() == 1);
            const auto command = uint8_t(bytes[0]);
            json_object_array_add(record, json_object_new_int(command));
            if (kind_ == "helper" && state_->confirm &&
                (command == '-' || (command >= 'A' && command <= 'P'))) {
                auto reply = json_object_new_object();
                json_object_object_add(reply, "op", json_object_new_string("input_gate_state"));
                json_object_object_add(reply, "index",
                                       json_object_new_int(command == '-' ? -1 : command - 'A'));
                json_object_object_add(reply, "monotonic_time", json_object_new_double(now_));
                event(reply);
            }
        }
        json_object_array_add(events_, record);
    }
    std::vector<j::Value> poll() override {
        return std::exchange(state_->pending, {});
    }
    std::vector<j::Value> close(const std::string &, double = 12) override {
        state_->stopped = true;
        if (!state_->close_error.empty())
            throw std::runtime_error(state_->close_error);
        std::vector<j::Value> result;
        if (state_->restore && kind_ != "filter") {
            auto reply = json_object_new_object();
            json_object_object_add(reply, "op", json_object_new_string("native_helper_restored"));
            result.emplace_back(reply);
        }
        return result;
    }
};
class SteamUi final : public ct::SteamUiSession {
    mutable Channel channel_;

  public:
    SteamUi(std::shared_ptr<ChildState> state, double &now, json_object *&events)
        : channel_(std::move(state), "filter", now, events) {}
    bool running() const override {
        return !channel_.exit_status();
    }
    void send(const ct::FilterRequest &request) override {
        channel_.send(j::encode(ct::test::filter_request_json(request).get()));
    }
    std::vector<ct::FilterEvent> poll() override {
        std::vector<ct::FilterEvent> result;
        for (const auto &event : channel_.poll())
            if (auto value = ct::test::filter_event(event.get()))
                result.push_back(std::move(*value));
        return result;
    }
    void close() override {
        channel_.close("{}");
    }
};
class Runtime final : public ct::RuntimeView {
    ct::QueueContext &queues_;

  public:
    bool awake = true;
    ct::RigidPose head{1, 0, 0, 0, 0, 1, 0, 1.6, 0, 0, 1, 0};
    explicit Runtime(ct::QueueContext &queues) : queues_(queues) {}
    std::vector<ct::RuntimeDevice> devices() override {
        return {{3, 2, true, 200}};
    }
    std::string serial(uint32_t index) override {
        return index == 0 ? "HEADSET" : "controllerutils-sc26-input-TEST1234";
    }
    std::optional<ct::RigidPose> head_pose(int = 1) override {
        return awake ? std::optional<ct::RigidPose>(head) : std::nullopt;
    }
    ct::QueueContext &queues() override {
        return queues_;
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
inline json_object *pose_json(const std::optional<ct::BodyPoseMessage> &pose) {
    if (!pose)
        return nullptr;
    auto result = json_object_new_object();
    json_object_object_add(result, "op", json_object_new_string("native_pose"));
    json_object_object_add(result, "time", json_object_new_double(pose->time));
    json_object_object_add(result, "position_time", json_object_new_double(pose->position_time));
    json_object_object_add(result, "position", j::value(pose->position));
    json_object_object_add(result, "orientation_time",
                           json_object_new_double(pose->orientation_time));
    json_object_object_add(result, "orientation", j::value(pose->orientation));
    return result;
}
inline j::Value replay(json_object *request) {
    ct::test::Queue queue;
    ct::QueueContext queues(queue);
    Runtime runtime(queues);
    auto hid = std::make_shared<ct::test::FakeState>();
    auto controller =
        std::make_shared<ct::ControllerDevice>(std::make_unique<ct::test::FakeTransport>(hid));
    double now = 100;
    queue.advancing_status = true;
    queue.connection_time = [&] { return now + 1000; };
    auto helper = std::make_shared<ChildState>(), filter = std::make_shared<ChildState>();
    helper->pending.push_back(j::parse(R"({"op":"native_helper_ready","isolated":true})"));
    helper->pending.push_back(j::parse(R"({"op":"input_gate_ready"})"));
    helper->pending.push_back(j::parse(R"({"op":"beam_gate_ready"})"));
    j::Value events(json_object_new_array());
    auto events_pointer = events.get();
    auto factory =
        [&](const std::vector<std::string> &command) -> std::unique_ptr<ct::ChildChannel> {
        for (const auto &word : command)
            assert(word.find("python") == std::string::npos);
        assert(command[0] == "/usr/bin/sudo" && command.back() == "--private");
        return std::make_unique<Channel>(helper, "helper", now, events_pointer);
    };
    auto calibration = ct::TrackingCalibration::from_json(j::field(request, "calibration"));
    ct::TrackingSession session(
        runtime, controller, calibration, {}, factory,
        [&](const std::string &serial, bool keyboard) -> std::unique_ptr<ct::SteamUiSession> {
            assert(serial == "TEST1234" && !keyboard);
            return std::make_unique<SteamUi>(filter, now, events_pointer);
        },
        [&] { return now; }, [&] { return now + 1000; }, [&](double t) { now += t; });
    j::Value output(json_object_new_array());
    auto steps = j::field(request, "steps");
    for (size_t i = 0; i < j::array_size(steps); ++i) {
        auto step = j::item(steps, i);
        now = j::number(j::field(step, "now"));
        events.reset(json_object_new_array());
        events_pointer = events.get();
        if (auto value = j::find(step, "awake"))
            runtime.awake = j::boolean(value);
        if (auto value = j::find(step, "head"))
            runtime.head = j::numbers<12>(value);
        for (const auto &[name, state] :
             std::vector<std::pair<const char *, std::shared_ptr<ChildState>>>{
                 {"helper_events", helper}, {"filter_events", filter}})
            if (auto values = j::find(step, name))
                for (size_t n = 0; n < j::array_size(values); ++n)
                    state->pending.emplace_back(json_object_get(j::item(values, n)));
        if (auto value = j::find(step, "pose")) {
            ct::wire::PoseBlock pose{};
            pose.controller_id = 16;
            pose.timestamp = j::number(j::field(value, "timestamp"));
            const auto q = j::numbers<4>(j::field(value, "quaternion"));
            const auto p = j::numbers<3>(j::field(value, "position"));
            std::copy(q.begin(), q.end(), pose.orientation);
            std::copy(p.begin(), p.end(), pose.position);
            queue.packets[3].resize(sizeof(pose));
            std::memcpy(queue.packets[3].data(), &pose, sizeof(pose));
        } else
            queue.packets.erase(3);
        std::string error;
        std::optional<ct::BodyPoseMessage> pose;
        j::Value presentation;
        try {
            if (auto op = j::find(step, "action")) {
                const auto name = j::string(op);
                if (name == "recenter")
                    session.recenter();
                else if (name == "acquire")
                    session.acquire();
                else if (name == "close")
                    session.close();
                else
                    throw std::invalid_argument("unknown test action");
            }
            std::optional<ct::ControllerInput> report;
            if (auto value = j::find(step, "report"))
                report = input(value);
            session.controls(report, j::boolean(j::field(step, "enabled")),
                             j::boolean(j::field(step, "typing")));
            pose = session.step(report ? std::vector<ct::ControllerInput>{*report}
                                       : std::vector<ct::ControllerInput>{});
            presentation = session.presentation();
        } catch (const std::exception &failure) {
            error = failure.what();
        }
        auto result = json_object_new_object();
        json_object_object_add(result, "error",
                               error.empty() ? nullptr : json_object_new_string(error.c_str()));
        json_object_object_add(result, "pose", pose_json(pose));
        json_object_object_add(result, "feedback", presentation.release());
        json_object_object_add(result, "status", json_object_new_string(session.status.c_str()));
        json_object_object_add(result, "ready", json_object_new_boolean(session.initial.ready));
        json_object_object_add(result, "events", json_object_get(events.get()));
        json_object_object_add(
            result, "imu_commands",
            json_object_new_int64(
                std::count_if(hid->commands.begin(), hid->commands.end(), [](const auto &command) {
                    return command[1] == 0x87 && command[3] == 0x30 && command[4] == 0x18;
                })));
        json_object_object_add(result, "brightness", json_object_new_int(hid->brightness));
        json_object_array_add(output.get(), result);
    }
    events.reset(json_object_new_array());
    events_pointer = events.get();
    session.close();
    return output;
}
} // namespace session_replay
