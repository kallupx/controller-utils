#include "../driver/native_bridge.cpp"
#include "tracked_input_support.h"
#include <cassert>
#include <map>
#include <vector>

class TestQueue final : public ct::steamvr::BlockQueue {
  public:
    std::map<uint64_t, std::vector<unsigned char>> buffers{
        {1000, std::vector<unsigned char>(48)},
        {1001, std::vector<unsigned char>(ct::wire::EventSize)}};
    std::vector<uint32_t> events;
    uint64_t input = 0, output = 0, next = 1;
    unsigned imu_writes = 0, destroyed = 0;
    bool pending = false, absent = false, null_imu_buffer = false;
    int Create(uint64_t *handle, const char *name, uint32_t size, uint32_t, uint32_t,
               uint32_t flag) override {
        if (!std::strcmp(name, "/controller-utils/native/input_v1")) {
            assert(size == sizeof(ct::wire::Command) && flag == 1);
            input = next;
        } else if (!std::strcmp(name, "/controller-utils/native/status_v1")) {
            assert(size == sizeof(ct::wire::Status) && flag == 0);
            output = next;
        } else {
            assert(!std::strcmp(name, "/xrservice/controller_16/pose") && size == 144 && flag == 1);
        }
        *handle = next++;
        buffers[*handle].resize(size);
        return 0;
    }
    int Connect(uint64_t *handle, const char *name) override {
        if (absent)
            return 2;
        if (!std::strcmp(name, "/xrservice/controller/data"))
            *handle = 1000;
        else {
            assert(!std::strcmp(name, "/xrservice/controller/event"));
            *handle = 1001;
        }
        return 0;
    }
    int Destroy(uint64_t handle) override {
        assert(buffers.count(handle));
        assert(handle != 1000 && handle != 1001);
        ++destroyed;
        return 0;
    }
    int AcquireWriteOnlyBlock(uint64_t handle, uint64_t *block, void **buffer) override {
        assert(buffers.count(handle));
        *block = handle;
        *buffer = handle == 1000 && null_imu_buffer ? nullptr : buffers[handle].data();
        return 0;
    }
    int ReleaseWriteOnlyBlock(uint64_t handle, uint64_t block) override {
        assert(handle == block);
        if (handle == 1001) {
            uint32_t connected;
            std::memcpy(&connected, buffers[handle].data() + 4, 4);
            events.push_back(connected);
        }
        if (handle == 1000)
            ++imu_writes;
        return 0;
    }
    int WaitAndAcquireReadOnlyBlock(uint64_t, uint64_t *, void **, int, uint32_t) override {
        assert(false);
        return 0;
    }
    int AcquireReadOnlyBlock(uint64_t handle, uint64_t *block, void **buffer, int type) override {
        if (handle != input)
            return 3;
        assert(type == 2);
        if (!pending)
            return 3;
        pending = false;
        *block = handle;
        *buffer = buffers[handle].data();
        return 0;
    }
    int ReleaseReadOnlyBlock(uint64_t handle, uint64_t block) override {
        assert(handle == block);
        return 0;
    }
    int QueueHasReader(uint64_t, bool *) override {
        assert(false);
        return 0;
    }
    ct::wire::Status status() {
        ct::wire::Status value;
        std::memcpy(&value, buffers[output].data(), sizeof(value));
        return value;
    }
    void submit(NativeBridge &bridge, const ct::wire::Command &command, double time) {
        std::memcpy(buffers[input].data(), &command, sizeof(command));
        pending = true;
        bridge.run_frame(time);
    }
};
class TestPaths final : public ct::steamvr::Paths {
  public:
    unsigned resolved = 0, writes = 0;
    bool fail = false;
    int ReadPathBatch(uint64_t, void *, uint32_t) override {
        assert(false);
        return 0;
    }
    int StringToHandle(uint64_t *path, const char *) override {
        *path = ++resolved;
        return 0;
    }
    int WritePathBatch(uint64_t root, ct::steamvr::PathWrite *properties, uint32_t count) override {
        assert(root == 1001 && count == 3);
        assert(properties[0].path == 1 && properties[1].path == 2 && properties[2].path == 3);
        assert(properties[0].size == 8 && properties[0].tag == 3 && properties[2].tag == 5);
        assert(properties[2].size == std::strlen(static_cast<char *>(properties[2].buffer)) + 1);
        ++writes;
        if (fail)
            properties[0].error = 4;
        return 0;
    }
};
class TestContext final : public vr::IVRDriverContext {
  public:
    struct TestLog final : vr::IVRDriverLog {
        std::vector<std::string> messages;
        void Log(const char *message) override {
            messages.emplace_back(message);
        }
    } log;
    TestQueue queue;
    TestPaths paths;
    Host host;
    Input input;
    Properties properties;
    void *GetGenericInterface(const char *name, vr::EVRInitError *error) override {
        *error = vr::VRInitError_None;
        if (!std::strcmp(name, vr::IVRDriverLog_Version))
            return &log;
        if (!std::strcmp(name, "IVRBlockQueue_005"))
            return &queue;
        if (!std::strcmp(name, "IVRPaths_002"))
            return &paths;
        if (!std::strcmp(name, vr::IVRServerDriverHost_Version))
            return &host;
        if (!std::strcmp(name, vr::IVRDriverInput_Version))
            return &input;
        if (!std::strcmp(name, vr::IVRProperties_Version))
            return &properties;
        *error = vr::VRInitError_Init_InterfaceNotFound;
        return nullptr;
    }
    vr::DriverHandle_t GetDriverHandle() override {
        return 1;
    }
};

ct::wire::Command connection() {
    const char *json =
        R"({"device_class":"controller","device_serial_number":"controllerutils-sc26-TEST1234","model_number":"ibex_ev1","imu":{"plus_x":[1,0,0],"plus_z":[0,0,1],"position":[0,0,0]},"lighthouse_config":{"modelPoints":[[0,0,0]],"modelNormals":[[0,0,1]]}})";
    ct::wire::Command command{};
    command.magic = ct::wire::Magic;
    command.kind = 4;
    command.size = ct::wire::EventSize;
    command.onboard_size = command.default_size = std::strlen(json);
    ct::wire::EventHeader header{ct::wire::ControllerId, 1, 0x47};
    std::memcpy(command.data, &header, sizeof(header));
    std::memcpy(command.data + 16, json, std::strlen(json));
    std::memcpy(command.data + 0x3010, json, std::strlen(json));
    return command;
}
ct::wire::Command sample(double time) {
    ct::wire::Command command{};
    command.magic = ct::wire::Magic;
    command.kind = 2;
    command.size = sizeof(ct::wire::Imu);
    ct::wire::Imu imu{ct::wire::ControllerId, 0, time, {0, 0, 9.80665f}, {0, 0, 0}, {0, 0}};
    std::memcpy(command.data, &imu, sizeof(imu));
    return command;
}

ct::wire::AppInput controls(double session, double time) {
    ct::wire::AppInput value{};
    value.id = ct::wire::ControllerId;
    value.flags = 3;
    value.session_start = session;
    value.time = time;
    value.pose_time = time - .01;
    value.orientation[0] = value.orientation[2] = std::sqrt(.5);
    value.position[0] = .1;
    value.position[1] = .2;
    value.position[2] = -.5;
    return value;
}
ct::wire::Command app_command(const ct::wire::AppInput &value) {
    ct::wire::Command command{};
    command.magic = ct::wire::Magic;
    command.kind = 3;
    command.size = sizeof(value);
    std::memcpy(command.data, &value, sizeof(value));
    return command;
}
struct BridgeFixture {
    TestContext context;
    NativeBridge bridge{[](double) { return true; },
                        [](ct::steamvr::BlockQueue *) { return true; }};
    explicit BridgeFixture(double time) {
        assert(bridge.Init(&context) == vr::VRInitError_None);
        context.queue.submit(bridge, connection(), time);
        assert(context.queue.status().connected);
    }
    ~BridgeFixture() {
        bridge.Cleanup();
    }
    void imu(double time) {
        context.queue.submit(bridge, sample(time), time);
    }
    void ui(ct::wire::AppInput value, double now) {
        context.queue.submit(bridge, app_command(value), now);
    }
    void released() {
        assert(context.input.released());
        assert(!context.host.published.deviceIsConnected && !context.host.published.poseIsValid);
        assert(context.properties.role() == vr::TrackedControllerRole_OptOut);
    }
};

void test_isolation_requires_live_capability_and_yields_when_it_expires() {
    TestContext context;
    static bool ready = false;
    NativeBridge bridge([](double) { return ready; },
                        [](ct::steamvr::BlockQueue *) { return true; });
    assert(bridge.Init(&context) == vr::VRInitError_None);
    context.properties.controller(2, vr::TrackedControllerRole_RightHand);
    context.host.poses[2].bDeviceIsConnected = true;
    auto connect = connection();
    connect.kind = 4;
    context.queue.submit(bridge, connect, 200);
    assert(context.queue.events.empty() && context.queue.status().error == -8);
    ready = true;
    context.queue.submit(bridge, connect, 201);
    assert(context.queue.status().connected);
    context.queue.submit(bridge, sample(201.01), 201.01);
    context.queue.submit(bridge, app_command(controls(201, 201.02)), 201.02);
    assert(context.host.published.deviceIsConnected && context.host.poses[2].bDeviceIsConnected);
    assert(vr::CVRPropertyHelpers(&context.properties)
               .GetInt32Property(110, vr::Prop_ControllerHandSelectionPriority_Int32) == 1000);
    for (unsigned i = 1; i <= 10; ++i)
        context.queue.submit(bridge, sample(201.02 + i * .1), 201.02 + i * .1);
    assert(context.queue.status().connected && context.host.poses[2].bDeviceIsConnected);
    assert(context.input.released() &&
           context.properties.role() == vr::TrackedControllerRole_OptOut);
    ready = false;
    bridge.run_frame(202.2);
    assert(!context.queue.status().connected && context.queue.status().error == -8);
    assert(context.input.released() &&
           context.properties.role() == vr::TrackedControllerRole_OptOut);
    assert(context.host.poses[2].bDeviceIsConnected);
    bridge.Cleanup();
    ct::TrackerCapability capability{0x314c4f5349544343ULL, 1, 2, 1000000000, 12, 1, 456};
    assert(ct::valid_tracker_capability(capability, 500000000, 456));
    assert(!ct::valid_tracker_capability(capability, 499999999, 456));
    assert(!ct::valid_tracker_capability(capability, 1000000000, 456));
    assert(!ct::valid_tracker_capability(capability, 500000000, 457));
    capability.phase = 3;
    assert(!ct::valid_tracker_capability(capability, 500000000, 456));
}

void test_app_input_wire_and_disable() {
    BridgeFixture f(100);
    f.imu(100.01);
    auto value = controls(100, 100.02);
    f.ui(value, value.time);
    assert(f.context.host.adds == 1 &&
           f.context.host.serial == "controllerutils-sc26-input-TEST1234");
    assert(f.context.properties.role() == vr::TrackedControllerRole_RightHand);
    const auto &pose = f.context.host.published;
    assert(pose.deviceIsConnected && pose.poseIsValid &&
           pose.result == vr::TrackingResult_Running_OK);
    assert(std::abs(pose.poseTimeOffset) < 1e-9);
    assert(pose.qRotation.w == value.orientation[0] && pose.qRotation.y == value.orientation[2]);
    assert(pose.qRotation.x == 0 && pose.qRotation.z == 0);
    assert(pose.qWorldFromDriverRotation.w == 1 && pose.qDriverFromHeadRotation.w == 1);
    for (unsigned i = 0; i < 3; ++i)
        assert(pose.vecPosition[i] == value.position[i]);
    value = controls(100, 100.04);
    value.trigger = .8f;
    value.grip = .7f;
    value.stick_x = -.25f;
    value.stick_y = .75f;
    value.buttons = 255;
    value.pad_x = .1f;
    value.pad_y = -.2f;
    value.scroll_x = .3f;
    value.scroll_y = -.4f;
    f.ui(value, value.time);
    for (const char *path :
         {"/input/trigger/click", "/input/trigger/touch", "/input/grip/click", "/input/grip/touch",
          "/input/thumbstick/click", "/input/thumbstick/touch", "/input/b/click",
          "/input/bumper/click", "/input/cursorpad/touch", "/input/cursorpad/click",
          "/input/trackpad/touch", "/input/trackpad/click"})
        assert(f.context.input.values.at(path) == 1);
    assert(f.context.input.values.at("/input/trigger/value") == value.trigger);
    assert(f.context.input.values.at("/input/grip/value") == value.grip);
    assert(f.context.input.values.at("/input/thumbstick/x") == value.stick_x);
    assert(f.context.input.values.at("/input/thumbstick/y") == value.stick_y);
    assert(f.context.input.values.at("/input/cursorpad/x") == value.pad_x);
    assert(f.context.input.values.at("/input/cursorpad/y") == value.pad_y);
    assert(f.context.input.values.at("/input/trackpad/x") == value.scroll_x);
    assert(f.context.input.values.at("/input/trackpad/y") == value.scroll_y);
    value.time = 100.05;
    value.pose_time = 100.06;
    f.ui(value, value.time);
    assert(f.context.host.published.deviceIsConnected);
    assert(std::abs(f.context.host.published.poseTimeOffset) < 1e-9);

    value.time = 100.06;
    value.pose_time = 100.05;
    value.flags = 2;
    f.ui(value, value.time);
    f.released();
    assert(f.context.queue.events == std::vector<uint32_t>{1});
    f.ui(controls(100, 100.08), 100.08);
    assert(f.context.host.published.deviceIsConnected);
    value = controls(100, 100.1);
    value.flags = 1;
    value.pose_time = 0;
    value.trigger = 1;
    f.ui(value, value.time);
    f.released();
    auto off = connection();
    uint32_t disconnected = 0;
    std::memcpy(off.data + 4, &disconnected, sizeof(disconnected));
    f.context.queue.submit(f.bridge, off, 100.2);
    assert((f.context.queue.events == std::vector<uint32_t>{1, 0}));
    assert(!f.context.queue.status().connected);
    value = controls(100, 100.31);
    value.trigger = 1;
    f.ui(value, value.time);
    f.released();
    assert(f.context.queue.status().error == -6 && f.context.host.adds == 1);
}

void test_rejected_app_packets() {
    BridgeFixture f(200);
    f.imu(200.01);
    f.ui(controls(200, 200.02), 200.02);
    auto held = controls(200, 200.04);
    held.trigger = .8f;
    f.ui(held, held.time);
    const auto updates = f.context.input.updates.size();
    auto wrong_session = controls(199, 200.06);
    f.ui(wrong_session, 200.06);
    auto stale = controls(200, 200.001);
    f.ui(stale, 200.17);
    auto replay = held;
    replay.trigger = 0;
    f.ui(replay, 200.17);
    assert(f.context.input.updates.size() == updates);
    assert(f.context.input.values.at("/input/trigger/click") == 1);
    assert(f.context.queue.status().error == -6);
    f.bridge.run_frame(200.21);
    f.released();
    f.imu(200.65);
    assert(f.context.queue.events == std::vector<uint32_t>{1});
    f.bridge.run_frame(201.16);
    assert((f.context.queue.events == std::vector<uint32_t>{1, 0}));

    BridgeFixture no_lease(250);
    no_lease.ui(controls(249, 250.01), 250.01);
    assert(no_lease.context.host.adds == 0);
    no_lease.imu(279.95);
    no_lease.ui(controls(249, 280.01), 280.01);
    no_lease.bridge.run_frame(280.11);
    assert((no_lease.context.queue.events == std::vector<uint32_t>{1, 0}));
    assert(std::find(no_lease.context.log.messages.begin(), no_lease.context.log.messages.end(),
                     "Controller Utils native disconnect reason=bench timeout") !=
           no_lease.context.log.messages.end());
}

void test_interactive_session_leases() {
    BridgeFixture f(300);
    for (unsigned i = 1; i <= 312; ++i) {
        const double time = 300 + i * .1;
        f.imu(time);
        f.ui(controls(300, time), time);
    }
    assert(f.context.queue.events == std::vector<uint32_t>{1});
    assert(f.context.queue.status().connected && f.context.queue.imu_writes == 312);
    assert(f.context.host.published.deviceIsConnected && f.context.host.adds == 1);
    auto held = controls(300, 331.21);
    held.trigger = .8f;
    f.ui(held, held.time);
    assert(f.context.input.values.at("/input/trigger/click") == 1);
    f.imu(331.42);
    f.released();
    assert(f.context.queue.events == std::vector<uint32_t>{1});
    f.imu(331.82);
    assert(f.context.queue.events == std::vector<uint32_t>{1});
    assert(f.context.queue.status().connected && f.context.input.released());
    for (unsigned i = 1; i <= 100; ++i)
        f.imu(331.82 + i * .1);
    assert(f.context.queue.status().connected);
    f.released();
    f.ui(controls(300, 341.84), 341.84);
    held = controls(300, 341.86);
    held.trigger = .8f;
    f.ui(held, held.time);
    assert(f.context.input.values.at("/input/trigger/click") == 1);
    assert(f.context.queue.events == std::vector<uint32_t>{1});

    BridgeFixture no_imu(400);
    no_imu.imu(400.01);
    for (unsigned i = 1; i <= 6; ++i) {
        const double time = 400 + i * .1;
        no_imu.ui(controls(400, time), time);
    }
    assert((no_imu.context.queue.events == std::vector<uint32_t>{1, 0}));
    no_imu.released();
    assert(std::find(no_imu.context.log.messages.begin(), no_imu.context.log.messages.end(),
                     "Controller Utils native disconnect reason=IMU timeout") !=
           no_imu.context.log.messages.end());
}

void test_imu_session_and_null_buffer() {
    BridgeFixture old_sample(600);
    old_sample.context.queue.submit(old_sample.bridge, sample(599.99), 600.01);
    old_sample.bridge.run_frame(600.12);
    assert(old_sample.context.queue.imu_writes == 0);
    assert(old_sample.context.queue.status().imu_count == 0 &&
           old_sample.context.queue.status().dropped == 1);
    old_sample.imu(600.2);
    old_sample.bridge.run_frame(600.32);
    assert(old_sample.context.queue.imu_writes == 1 &&
           old_sample.context.queue.status().imu_count == 1);

    BridgeFixture null_buffer(700);
    null_buffer.context.queue.null_imu_buffer = true;
    null_buffer.imu(700.4);
    assert(null_buffer.context.queue.status().imu_count == 0 &&
           null_buffer.context.queue.status().dropped == 1);
    assert(null_buffer.context.queue.status().last_input == 700);
    null_buffer.bridge.run_frame(700.51);
    assert((null_buffer.context.queue.events == std::vector<uint32_t>{1, 0}));
}

int main() {
    {
        TestContext context;
        bool initialized = false;
        NativeBridge bridge(ct::tracker_ready, +[](ct::steamvr::BlockQueue *) { return false; });
        assert(bridge.Init(&context) == vr::VRInitError_Driver_Failed);
        assert(context.queue.destroyed == 0 && context.queue.events.empty());
        NativeBridge ready(
            ct::tracker_ready, +[](ct::steamvr::BlockQueue *queue) { return queue != nullptr; });
        initialized = ready.Init(&context) == vr::VRInitError_None;
        assert(initialized);
        ready.Cleanup();
    }
    std::string serial;
    auto valid = connection();
    assert(ct::wire::event(valid, serial) && serial == "controllerutils-sc26-TEST1234");
    auto wrong_profile = valid;
    auto *model = std::strstr(reinterpret_cast<char *>(wrong_profile.data + 16), "ibex_ev1");
    assert(model);
    *model = 'I';
    assert(!ct::wire::event(wrong_profile, serial));
    auto bad = valid;
    bad.kind = 1;
    assert(!ct::wire::event(bad, serial));
    bad = valid;
    bad.onboard_size = ct::wire::EventSize;
    assert(!ct::wire::event(bad, serial));
    bad = valid;
    bad.data[0] = 17;
    assert(!ct::wire::event(bad, serial));
    bad = valid;
    bad.data[16] = '!';
    assert(!ct::wire::event(bad, serial));
    auto imu = sample(10);
    assert(ct::wire::imu(imu, 10.01, 9));
    assert(!ct::wire::imu(imu, 11, 9) && !ct::wire::imu(imu, 10.01, 10));
    ct::wire::Command ui{};
    ui.magic = ct::wire::Magic;
    ui.kind = 3;
    ui.size = sizeof(ct::wire::AppInput);
    ct::wire::AppInput input{16,   3,   10, 10.1, 10.05, {1, 0, 0, 0}, {.1, .2, -.5}, .5, 0,
                             -.25, .75, .1, -.2,  .3,    -.4,          255,           0};
    std::memcpy(ui.data, &input, sizeof(input));
    assert(ct::wire::app_input(ui, 10.11, 0, 10));
    assert(!ct::wire::app_input(ui, 10.07, 0, 10));
    assert(!ct::wire::app_input(ui, 10.3, 0, 10));
    assert(!ct::wire::app_input(ui, 10.11, 10.1, 10));
    assert(!ct::wire::app_input(ui, 10.11, 0, 9));
    ui.size = 112;
    assert(!ct::wire::app_input(ui, 10.11, 0, 10));
    ui.size = sizeof(input);
    for (auto field : {&ct::wire::AppInput::pad_x, &ct::wire::AppInput::pad_y,
                       &ct::wire::AppInput::scroll_x, &ct::wire::AppInput::scroll_y}) {
        auto invalid = input;
        invalid.*field = 1.01f;
        std::memcpy(ui.data, &invalid, sizeof(invalid));
        assert(!ct::wire::app_input(ui, 10.11, 0, 10));
    }
    auto invalid = input;
    invalid.buttons = 256;
    std::memcpy(ui.data, &invalid, sizeof(invalid));
    assert(!ct::wire::app_input(ui, 10.11, 0, 10));
    input.pose_time = 10.11;
    std::memcpy(ui.data, &input, sizeof(input));
    assert(ct::wire::app_input(ui, 10.1, 0, 10));
    input.pose_time = 10.121;
    std::memcpy(ui.data, &input, sizeof(input));
    assert(!ct::wire::app_input(ui, 10.1, 0, 10));
    input.pose_time = 9;
    std::memcpy(ui.data, &input, sizeof(input));
    assert(!ct::wire::app_input(ui, 10.11, 0, 10));
    input.flags = 0;
    input.pose_time = 0;
    std::memcpy(ui.data, &input, sizeof(input));
    assert(ct::wire::app_input(ui, 10.11, 0, 10));

    TestContext context;
    NativeBridge bridge{[](double) { return true; },
                        [](ct::steamvr::BlockQueue *) { return true; }};
    assert(bridge.Init(&context) == vr::VRInitError_None);
    assert(context.queue.status().version == 3);
    context.queue.submit(bridge, valid, 10);
    assert(context.queue.events == std::vector<uint32_t>{1} && context.queue.status().connected);
    context.queue.submit(bridge, sample(10.1), 10.11);
    assert(context.queue.imu_writes == 1);
    context.queue.submit(bridge, sample(10.1), 10.2);
    assert(context.queue.imu_writes == 1);
    bridge.run_frame(10.7);
    assert((context.queue.events == std::vector<uint32_t>{1, 0}) &&
           !context.queue.status().connected);
    context.queue.submit(bridge, valid, 20);
    context.queue.submit(bridge, sample(49.9), 49.91);
    bridge.run_frame(50.1);
    assert((context.queue.events == std::vector<uint32_t>{1, 0, 1, 0}));
    bridge.Cleanup();
    bridge.Cleanup();
    assert(context.queue.destroyed == 3);

    TestContext missing;
    missing.queue.absent = true;
    assert(bridge.Init(&missing) == vr::VRInitError_None);
    missing.queue.submit(bridge, valid, 60);
    assert(missing.queue.events.empty() && missing.queue.status().error == 2);
    bridge.Cleanup();

    TestContext metadata;
    metadata.paths.fail = true;
    assert(bridge.Init(&metadata) == vr::VRInitError_None);
    metadata.queue.submit(bridge, valid, 70);
    assert(metadata.queue.events == std::vector<uint32_t>{0} && !metadata.queue.status().connected);
    bridge.Cleanup();
    test_app_input_wire_and_disable();
    test_rejected_app_packets();
    test_interactive_session_leases();
    test_isolation_requires_live_capability_and_yields_when_it_expires();
    test_imu_session_and_null_buffer();
    std::puts("native bridge: wire input, pose, session leases, rejection, handover, bench cap and "
              "cleanup passed");
}
