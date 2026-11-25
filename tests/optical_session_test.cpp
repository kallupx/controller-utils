#include "session_replay.h"
#include "tracking/optical_session.h"
#include <cassert>
#include <fcntl.h>

namespace {
int64_t micros = 100000000;
int pair_fd[2]{}, gate_fd[2]{}, beam_fd[2]{};
class Camera final : public ct::ChildChannel {
  public:
    std::vector<ct::json::Value> events;
    bool lit = false, stopped = false, guided = false, checking_resume = false, resumed = false;
    unsigned restarts = 0;
    unsigned ready_requests = 0;
    std::optional<int> exit_status() override {
        return {};
    }
    void send(const std::string &bytes) override {
        auto msg = ct::json::parse(bytes);
        const auto op = ct::json::string(ct::json::field(msg.get(), "op"));
        if (op == "lit")
            lit = true;
        if (op == "request_ready")
            ++ready_requests;
        if (op == "reacquire")
            ++restarts;
        if (op == "check_resume")
            checking_resume = true;
        if (op == "resume")
            resumed = true;
        guided |= json_object_object_get_ex(msg.get(), "prior", nullptr);
    }
    std::vector<ct::json::Value> poll() override {
        return std::exchange(events, {});
    }
    std::vector<ct::json::Value> close(const std::string &msg, double) override {
        stopped = msg.find("stop") != std::string::npos;
        return {};
    }
    void emit(const char *op, double time, const ct::Vector3 &point = {.1, -.2, -.5}) {
        auto msg = ct::json::parse(
            R"({"transform":[1,0,0,0.1,0,1,0,-0.2,0,0,1,-0.5],"point":[0.1,-0.2,-0.5],"origin":[0,0,0],"direction":[0.236962,-0.360397,-0.902188]})");
        json_object_object_add(msg.get(), "op", json_object_new_string(op));
        json_object_object_add(msg.get(), "time", json_object_new_double(time));
        json_object_object_add(msg.get(), "point", ct::json::value(point));
        events.push_back(std::move(msg));
    }
};
} // namespace
extern "C" int __real_clock_gettime(clockid_t, timespec *);
extern "C" int __wrap_clock_gettime(clockid_t id, timespec *out) {
    if (id != CLOCK_MONOTONIC && id != CLOCK_MONOTONIC_RAW)
        return __real_clock_gettime(id, out);
    *out = {micros / 1000000, (micros % 1000000) * 1000};
    return 0;
}
extern "C" int __wrap_connect(int fd, const sockaddr *addr, socklen_t) {
    assert(addr->sa_family == AF_UNIX);
    const auto &socket = *reinterpret_cast<const sockaddr_un *>(addr);
    if (std::strstr(socket.sun_path + 1, ".beam.")) {
        if (dup3(beam_fd[0], fd, O_CLOEXEC) < 0)
            return -1;
        close(std::exchange(beam_fd[0], -1));
        return 0;
    }
    const bool gate = std::strstr(socket.sun_path + 1, ".input.");
    return dup3(gate ? gate_fd[0] : pair_fd[0], fd, O_CLOEXEC) < 0 ? -1 : 0;
}
int main() {
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair_fd) == 0);
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, gate_fd) == 0);
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, beam_fd) == 0);
    session_replay::Runtime runtime;
    runtime.dashboard_pointer = 3;
    auto hid = std::make_shared<ct::test::FakeState>();
    auto device =
        std::make_shared<ct::ControllerDevice>(std::make_unique<ct::test::FakeTransport>(hid));
    ct::TrackingCalibration cal{
        "HEADSET", {1, "TEST1234", {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}}, {}}, {0, .01, -.02}, false};
    Camera *camera{};
    std::optional<ct::TrackingCalibration> saved;
    auto filter = std::make_shared<session_replay::ChildState>();
    auto log = ct::json::parse("[]");
    auto *events = log.get();
    double stamp = 0;
    const auto make_camera =
        [&](const std::vector<std::string> &args) -> std::unique_ptr<ct::ChildChannel> {
        assert(args.front() == "/usr/bin/python3" &&
               args.at(2) == "/test/optical/frame_camera_runtime.py" && args.back() == "HEADSET");
        auto child = std::make_unique<Camera>();
        camera = child.get();
        return child;
    };
    const auto make_filter = [&](const std::string &, bool) {
        return std::make_unique<session_replay::SteamUi>(filter, stamp, events);
    };
    ct::OpticalSession session(runtime, device, cal, {}, "/test", make_camera, make_filter,
                               [&](const auto &value) { saved = value; });
    ct::optical::Packet packet{};
    ct::ControllerInput report{};
    report.report.acceleration = {0, 9.80665, 0};
    report.report.angular_velocity[1] = .004;
    bool ui = false, gate_ready = false, stalled = false;
    auto step = [&] {
        micros += 5000;
        if (!stalled)
            report.report.sensor_tick += 5000;
        report.received = stamp = ct::monotonic_time();
        session.controls(report, ui, false);
        auto pose = session.step({report});
        ct::optical::Packet next{};
        while (recv(pair_fd[1], &next, sizeof(next), 0) == sizeof(next))
            packet = next;
        int32_t index;
        if (gate_ready)
            while (recv(gate_fd[1], &index, sizeof(index), 0) == sizeof(index)) {
                assert(index == 0);
                assert(send(gate_fd[1], &index, sizeof(index), 0) == sizeof(index));
            }
        return pose;
    };
    micros += 6000000;
    assert(!step() && !hid->raw && hid->brightness == 25);
    camera->events.push_back(
        ct::json::parse(R"({"op":"waiting","message":"Waiting for Frame to wake."})"));
    camera->emit("light", ct::raw_time());
    assert(!step() && !hid->raw && hid->brightness == 25 && camera->ready_requests == 1);
    assert(!step() && session.status == "Waiting for Frame to wake.");
    camera->emit("ready", ct::raw_time());
    camera->emit("ready", ct::raw_time());
    assert(!step() && hid->raw && hid->brightness == 0);
    camera->emit("light", ct::raw_time());
    for (int i = 0; i < 220; ++i)
        assert(!step());
    assert(camera->lit && hid->raw);
    assert(saved && saved->gyro_calibrated &&
           std::abs(saved->body.gyro_bias_report_rad_s[1] - .004) < 1e-9);
    camera->emit("seed", ct::raw_time() - .25);
    assert(!step() && !session.acquired());
    camera->emit("led", ct::raw_time() - .075);
    auto pose = step();
    assert(session.acquired());
    assert(pose && ct::distance(pose->position, {.1, 1.39, -.48}) < .0001);
    assert(pose->position_time == ct::raw_time() && pose->time < pose->position_time - .07);
    micros += 1000;
    assert(!session.step({}));
    for (int i = 0; i < 10; ++i)
        assert(step());
    assert((packet.input.flags & 2) && !(packet.input.flags & 1));
    ui = true;
    for (int i = 0; i < 4; ++i)
        assert(step());
    assert((packet.input.flags & 2) && !(packet.input.flags & 1));
    gate_ready = true;
    for (int i = 0; i < 4; ++i)
        assert(step());
    assert((packet.input.flags & 3) == 3);
    report.report.buttons = 0x00200000;
    for (int i = 0; i < 4; ++i)
        assert(step());
    int32_t target;
    assert(recv(beam_fd[1], &target, sizeof(target), 0) == sizeof(target) && target == 3);
    assert((packet.input.flags & 3) == 3 && (packet.input.buttons & 16u));
    const auto pressure = [&](double force) {
        report.report.right_pad[2] = force;
        for (int i = 0; i < 3; ++i)
            assert(step());
        return bool(packet.input.buttons & 32u);
    };
    assert(!pressure(.09));
    assert(pressure(.14));
    assert(pressure(.09));
    assert(!pressure(.06));
    report.report.buttons = 0;
    assert(step());
    assert(recv(beam_fd[1], &target, sizeof(target), 0) == 0);
    assert((packet.input.flags & 3) == 3);
    assert(camera->guided);
    const auto before_bearing = step();
    assert(before_bearing);
    for (int i = 0; i < 24; ++i) {
        if (i % 6 == 0)
            camera->emit("bearing", ct::raw_time() - .01);
        const auto next = step();
        assert(next && next->time == pose->time);
    }
    const auto after_bearing = step();
    assert(after_bearing && after_bearing->position[0] > before_bearing->position[0] + .003);
    camera->emit("led", ct::raw_time() - .01);
    const auto before_loss = step();
    assert(before_loss && before_loss->time > pose->time);
    for (int i = 0; i < 40; ++i)
        assert(step());
    report.report.acceleration[0] = 2.8;
    report.report.angular_velocity[1] = .25;
    for (int i = 0; i < 56; ++i)
        assert(step());
    const auto moving = step();
    assert(moving && moving->position[0] > before_loss->position[0] + .045);
    assert(std::abs(ct::dot(moving->orientation, pose->orientation)) < .9999);
    report.report.acceleration[0] = -3.8;
    for (int i = 0; i < 96; ++i)
        assert(step());
    const auto reversed = step();
    assert(reversed && reversed->position[0] < moving->position[0] - .04);
    report.report.acceleration[0] = -6;
    for (int i = 0; i < 180; ++i)
        assert(step());
    const auto still = step();
    assert(still && ct::distance(still->position, pose->position) <= .350001);
    assert((packet.input.flags & 3) == 3);
    assert(packet.input.pose_time - moving->time > 1.);
    report.report.acceleration[0] = 0;
    micros += 80000;
    report.report.sensor_tick += 80000;
    assert(step());
    for (int i = 0; i < 6; ++i)
        assert(step());
    const double returned = ct::raw_time() - .01;
    camera->emit("led", returned);
    auto recovered = step();
    assert(recovered && recovered->time == returned);
    assert(ct::distance(recovered->position, still->position) < .02);
    for (int i = 0; i < 31; ++i)
        recovered = step();
    assert(recovered && ct::distance(recovered->position, pose->position) < .03);
    session.rebase(ct::rigid_pose({1, 0, 0, 0}, {1, 0, 0}));
    assert(step() && session.acquired() && camera->restarts == 0);
    const auto command_count = hid->commands.size();
    stalled = true;
    for (int i = 0; i < 25; ++i)
        step();
    assert(hid->commands.size() == command_count + 1 && session.acquired());
    stalled = false;
    for (int i = 0; i < 10; ++i)
        step();

    auto last_filter_enabled = [&] {
        return ct::json::boolean(ct::json::field(
            ct::json::item(ct::json::item(events, ct::json::array_size(events) - 1), 1),
            "enabled"));
    };
    gate_ready = false;
    for (int i = 0; i < 110; ++i)
        step();
    assert(!(packet.input.flags & 1) && !last_filter_enabled());
    gate_ready = true;
    for (int i = 0; i < 175; ++i)
        step();
    assert((packet.input.flags & 1) && last_filter_enabled());
    filter->pending.push_back(
        ct::json::parse(R"({"op":"input_filter_error","error":"Steam UI restarted"})"));
    for (int i = 0; i < 4; ++i)
        step();
    assert(!(packet.input.flags & 1) && !last_filter_enabled() && !filter->stopped);
    for (int i = 0; i < 175; ++i)
        step();
    assert(packet.input.flags & 1);
    ui = false;
    assert(step() && step());
    assert(!(packet.input.flags & 1));

    runtime.head = ct::rigid_pose({std::cos(.27), 0, std::sin(.27), 0}, {0, 1.6, 0});
    report.report.angular_velocity = {};
    const auto before_heading = step();
    report.report.buttons = 0x4040;
    for (int i = 0; i < 240; ++i)
        assert(step());
    const auto ready = session.presentation();
    assert(ct::json::string(ct::json::field(ready.get(), "state")) == "restart");
    assert(before_heading && camera->restarts == 0);
    report.report.buttons = 0;
    const auto aligned = step();
    assert(aligned && aligned->time == before_heading->time && camera->restarts == 0);
    const auto back = ct::rotate(aligned->orientation, {0, 0, 1});
    assert(std::abs(std::atan2(back[0], back[2]) - .54) < 1e-6);

    report.report.buttons = 0x4040;
    for (int i = 0; i < 1000; ++i)
        assert(step());
    const auto old_seed = ct::raw_time() - .02;
    assert(step() && camera->restarts == 1 && session.guide.started && !session.acquired());
    camera->emit("seed", old_seed);
    for (int i = 0; i < 40; ++i)
        assert(step());
    assert(camera->restarts == 1 && session.guide.started);
    report.report.buttons = 0;
    const double fresh_led = ct::raw_time() - .03;
    camera->emit("led", fresh_led);
    const auto during_restart = step();
    assert(during_restart && during_restart->time == fresh_led && session.guide.started);
    camera->emit("seed", ct::raw_time() - .12);
    assert(step() && !session.acquired());
    camera->emit("led", ct::raw_time() - .01);
    assert(step() && session.acquired() && camera->restarts == 1);

    const auto previous = step();
    runtime.head[3] += 2.5;
    session.acquire();
    for (int i = 0; i < 50; ++i)
        assert(step());
    assert(previous && !session.acquired());
    camera->emit("seed", ct::raw_time() - .2);
    assert(step() && !session.acquired());
    camera->emit("led", ct::raw_time() - .01);
    const auto reanchored = step();
    assert(reanchored && session.acquired() &&
           reanchored->position[0] - previous->position[0] > 2.2);
    const auto checkpoint = session.checkpoint();
    assert(checkpoint);

    const auto before_burst = step();
    assert(before_burst);
    report.report.sensor_tick += 42000;
    micros += 5000;
    assert(!step() && session.acquired() && (packet.input.flags & 2));
    assert(packet.input.pose_time == before_burst->orientation_time);
    micros += 50000;
    const auto clock_recovered = step();
    assert(clock_recovered && session.acquired() && camera->restarts == 2);

    camera->emit("disconnected", ct::raw_time());
    try {
        step();
        assert(false);
    } catch (const ct::CameraDisconnected &) {
    }
    session.close();
    assert(camera->stopped && !hid->raw && hid->brightness == 25);

    runtime.head = ct::rigid_pose({std::cos(.35), 0, -std::sin(.35), 0}, {2.5, 1.6, 0});
    const auto wake = [&](const ct::Vector3 &movement, const ct::Vector3 &gravity, bool same_device,
                          bool picked_up = false, bool timeout = false) {
        auto anchor = *checkpoint;
        if (!same_device)
            anchor.controller_serial = "ANOTHER";
        micros += 60000000;
        ct::OpticalSession waking(runtime, device, *saved, {}, "/test", make_camera, make_filter,
                                  {}, anchor);
        report.report.acceleration = gravity;
        const auto tick = [&] {
            micros += 5000;
            report.report.sensor_tick += 5000;
            report.received = ct::monotonic_time();
            const auto result = waking.step({report});
            ct::optical::Packet next{};
            while (recv(pair_fd[1], &next, sizeof(next), 0) == sizeof(next))
                packet = next;
            return result;
        };
        camera->emit("ready", ct::raw_time());
        assert(!tick() && camera->checking_resume == same_device);
        camera->emit("light", ct::raw_time());
        std::optional<ct::BodyPoseMessage> preview;
        for (int i = 0; i < 220; ++i)
            preview = tick();
        const bool expect_preview = same_device && gravity[0] == 0;
        assert(bool(preview) == expect_preview && !waking.acquired() && !(packet.input.flags & 2));
        assert(camera->guided == expect_preview);
        if (preview) {
            assert(preview->time == anchor.pose.time && preview->position == anchor.pose.position);
            assert(!ct::json::boolean(ct::json::field(waking.presentation().get(), "active")));
        }
        if (picked_up) {
            report.report.angular_velocity[1] = 1;
            report.report.acceleration[1] += 2.6;
            for (int i = 0; i < 24; ++i) {
                preview = tick();
                assert(preview && !waking.acquired() && !(packet.input.flags & 2));
            }
            assert(!waking.checkpoint());
            assert(std::abs(ct::dot(preview->orientation, anchor.pose.orientation)) < .999);
            assert(ct::distance(preview->position, anchor.pose.position) > .006);
            report.report.angular_velocity = {};
            report.report.acceleration = gravity;
            if (timeout) {
                for (int i = 0; i < 180; ++i)
                    preview = tick();
                assert(!preview && !waking.checkpoint());
            }
        }
        const auto point =
            ct::transform_point(ct::inverse_pose(runtime.head), ct::add(anchor.led, movement));
        camera->emit("led", ct::raw_time() - .01, point);
        const bool expect_resume = expect_preview && !timeout && ct::norm(movement) < .15;
        assert(bool(tick()) == expect_resume && !camera->resumed && !waking.acquired());
        camera->emit("led", ct::raw_time() - .01, point);
        const auto restored = tick();
        assert(camera->resumed == expect_resume && bool(restored) == expect_resume);
        if (restored) {
            assert(ct::distance(restored->position, reanchored->position) < .03);
            assert(std::abs(ct::dot(restored->orientation, preview->orientation)) > .9999);
        } else {
            assert(!waking.checkpoint());
            camera->emit("seed", ct::raw_time() - .01);
            assert(tick() && waking.acquired());
        }
    };
    wake({}, {0, 9.80665, 0}, true);
    wake({.4, 0, 0}, {0, 9.80665, 0}, true);
    wake({}, {9.80665, 0, 0}, true);
    wake({}, {0, 9.80665, 0}, false);
    wake({}, {0, 9.80665, 0}, true, true);
    wake({.4, 0, 0}, {0, 9.80665, 0}, true, true);
    wake({}, {0, 9.80665, 0}, true, true, true);
    close(pair_fd[0]);
    close(pair_fd[1]);
    close(gate_fd[0]);
    close(gate_fd[1]);
    close(beam_fd[1]);

    ct::InertialPosition drift;
    drift.sample(1, {}, 0);
    assert(drift.observe(1, {}, 1));
    for (int i = 1; i <= 400; ++i)
        drift.sample(1 + i * .01, {.22, 0, 0}, 0);
    assert(ct::norm(*drift.value(5)) < .05);
}
