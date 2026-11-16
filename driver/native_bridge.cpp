#include <openvr_driver.h>
#include "steamvr/queue_abi.h"
#include "steamvr/protocol.h"
#include "tracked_input.h"
#include "tracker_capability.h"
#include "native_queue_startup.h"
#include "compatibility_profile.h"
#include "steamvr/startup_recovery.h"
#include <pwd.h>
#include <unistd.h>
#include <algorithm>
#include <ctime>
#include <memory>
#include <mutex>
#include <thread>

class NativeBridge final : public vr::IServerTrackedDeviceProvider {
    ct::steamvr::BlockQueue *queue = nullptr;
    ct::steamvr::Paths *paths = nullptr;
    vr::IVRDriverLog *log = nullptr;
    uint64_t input = 0, output = 0, native_data = 0, native_event = 0, pose = 0;
    uint64_t property_paths[3]{};
    ct::wire::Status status;
    ct::wire::Command saved_event{};
    std::string serial;
    double last_imu = 0, last_status = 0, last_disconnect = 0;
    double last_app_input = 0;
    std::unique_ptr<ct::TrackedInput> tracked_input;
    ct::wire::AppInput app_input{};
    std::unique_ptr<ct::StartupRecovery> recovery;
    bool recovery_started = false, recovery_healthy_requested = false;
    double recovery_start = 0;
    std::shared_ptr<std::mutex> recovery_lock = std::make_shared<std::mutex>();
    bool (*isolation_available)(double);
    bool (*initialize_queues)(ct::steamvr::BlockQueue *);

    static double now() {
        timespec time{};
        clock_gettime(CLOCK_MONOTONIC_RAW, &time);
        return double(time.tv_sec) + double(time.tv_nsec) * 1e-9;
    }
    void report(int error) {
        status.error = error;
        if (log && error) {
            char message[128];
            std::snprintf(message, sizeof(message), "Controller Utils native kind=%u error=%d",
                          status.last_kind, error);
            log->Log(message);
        }
    }
    void publish_status() {
        uint64_t block = 0;
        void *buffer = nullptr;
        if (queue && output && !queue->AcquireWriteOnlyBlock(output, &block, &buffer)) {
            if (buffer)
                std::memcpy(buffer, &status, sizeof(status));
            queue->ReleaseWriteOnlyBlock(output, block);
        }
    }
    int send_event(const ct::wire::Command &command) {
        uint64_t block = 0;
        void *buffer = nullptr;
        int rc = queue->AcquireWriteOnlyBlock(native_event, &block, &buffer);
        if (rc)
            return rc;
        if (!buffer) {
            queue->ReleaseWriteOnlyBlock(native_event, block);
            return -10;
        }
        std::memcpy(buffer, command.data, ct::wire::EventSize);
        uint64_t onboard_size = command.onboard_size, default_size = command.default_size;
        ct::steamvr::PathWrite properties[3]{};
        properties[0].buffer = &onboard_size;
        properties[1].buffer = &default_size;
        for (unsigned i = 0; i < 2; ++i) {
            properties[i].size = 8;
            properties[i].tag = 3;
        }
        properties[2].buffer = const_cast<char *>(serial.c_str());
        properties[2].size = uint32_t(serial.size() + 1);
        properties[2].tag = 5;
        for (unsigned i = 0; i < 3; ++i) {
            properties[i].path = property_paths[i];
            properties[i].post_events = true;
        }
        rc = paths->WritePathBatch(block, properties, 3);
        for (auto &property : properties)
            if (property.error)
                rc = -100 - property.error;
        if (rc) {
            // A failed metadata write must never publish a connect event with partial metadata.
            uint32_t disconnected = 0;
            std::memcpy(static_cast<unsigned char *>(buffer) + 4, &disconnected, 4);
        }
        int released = queue->ReleaseWriteOnlyBlock(native_event, block);
        return rc ? rc : released;
    }
    void disconnect(const char *reason) {
        if (tracked_input)
            tracked_input->yield();
        if (!status.connected)
            return;
        if (log) {
            char message[128];
            std::snprintf(message, sizeof(message), "Controller Utils native disconnect reason=%s",
                          reason);
            log->Log(message);
        }
        uint32_t disconnected = 0;
        std::memcpy(saved_event.data + 4, &disconnected, 4);
        int rc = send_event(saved_event);
        report(rc);
        if (!rc) {
            status.connected = 0;
            last_imu = 0;
            last_app_input = 0;
            app_input = {};
            if (log)
                log->Log("Controller Utils native disconnected");
        }
    }
    void handle_event(const ct::wire::Command &command, double time) {
        std::string validated_serial;
        if (!ct::wire::event(command, validated_serial)) {
            report(-1);
            return;
        }
        ct::wire::EventHeader header;
        std::memcpy(&header, command.data, sizeof(header));
        if (!header.connected) {
            disconnect("app request");
            return;
        }
        if (status.connected) {
            report(-4);
            return;
        }
        if (!isolation_available(time)) {
            report(-8);
            return;
        }
        int rc = 0;
        if (!native_data)
            rc = queue->Connect(&native_data, "/xrservice/controller/data");
        if (!rc && !native_event)
            rc = queue->Connect(&native_event, "/xrservice/controller/event");
        status.native_ready = !rc && native_data && native_event;
        if (!status.native_ready) {
            report(rc ? rc : -2);
            return;
        }
        if (!pose) {
            rc = queue->Create(&pose, "/xrservice/controller_16/pose", 144, 512, 4, 1);
            if (rc) {
                pose = 0;
                report(rc);
                return;
            }
        }
        serial = validated_serial;
        rc = send_event(command);
        report(rc);
        if (rc)
            return;
        saved_event = command;
        status.connected = 1;
        status.last_input = status.session_start = time;
        status.imu_count = 0;
        last_imu = 0;
        last_app_input = 0;
        if (log)
            log->Log("Controller Utils native connect published for device 16");
    }
    void handle_imu(const ct::wire::Command &command, double time) {
        if (!status.connected || !ct::wire::imu(command, time, last_imu)) {
            ++status.dropped;
            return;
        }
        ct::wire::Imu value;
        std::memcpy(&value, command.data, sizeof(value));
        if (value.time < status.session_start) {
            ++status.dropped;
            return;
        }
        uint64_t block = 0;
        void *buffer = nullptr;
        int rc = queue->AcquireWriteOnlyBlock(native_data, &block, &buffer);
        if (!rc) {
            if (buffer)
                std::memcpy(buffer, command.data, sizeof(ct::wire::Imu));
            const int released = queue->ReleaseWriteOnlyBlock(native_data, block);
            rc = buffer ? released : -10;
        }
        if (rc) {
            ++status.dropped;
            return;
        }
        last_imu = value.time;
        status.last_input = time;
        ++status.imu_count;
    }
    void handle_app_input(const ct::wire::Command &command, double time) {
        if (!status.connected ||
            !ct::wire::app_input(command, time, last_app_input, status.session_start)) {
            report(-6);
            return;
        }
        try {
            if (!tracked_input->register_device(
                    serial.substr(std::strlen("controllerutils-sc26-")))) {
                report(-7);
                return;
            }
        } catch (const std::exception &) {
            report(-7);
            return;
        }
        std::memcpy(&app_input, command.data, sizeof(app_input));
        last_app_input = app_input.time;
        report(0);
    }
    void update_input(double time) {
        if (!tracked_input)
            return;
        if (!status.connected || !last_app_input) {
            tracked_input->yield();
            return;
        }
        vr::DriverPose_t body{};
        body.qWorldFromDriverRotation.w = body.qDriverFromHeadRotation.w = 1;
        body.qRotation = {app_input.orientation[0], app_input.orientation[1],
                          app_input.orientation[2], app_input.orientation[3]};
        std::copy(app_input.position, app_input.position + 3, body.vecPosition);
        body.deviceIsConnected = true;
        body.poseIsValid = bool(app_input.flags & 2u);
        body.result =
            body.poseIsValid ? vr::TrackingResult_Running_OK : vr::TrackingResult_Uninitialized;
        ct::TrackedInputState controls;
        controls.time = app_input.time;
        controls.enabled = bool(app_input.flags & 1u);
        controls.trigger = app_input.trigger;
        controls.grip = app_input.grip;
        controls.stick_x = app_input.stick_x;
        controls.stick_y = app_input.stick_y;
        controls.back = app_input.buttons & 1u;
        controls.bumper = app_input.buttons & 2u;
        controls.stick_touch = app_input.buttons & 4u;
        controls.stick_click = app_input.buttons & 8u;
        controls.pad_x = app_input.pad_x;
        controls.pad_y = app_input.pad_y;
        controls.pad_touch = app_input.buttons & 16u;
        controls.pad_click = app_input.buttons & 32u;
        controls.scroll_x = app_input.scroll_x;
        controls.scroll_y = app_input.scroll_y;
        controls.scroll_touch = app_input.buttons & 64u;
        controls.scroll_click = app_input.buttons & 128u;
        tracked_input->update(body, app_input.pose_time, controls, time);
    }

  public:
    explicit NativeBridge(
        bool (*capability)(double) = ct::tracker_ready,
        bool (*startup)(ct::steamvr::BlockQueue *) = ct::queue_startup::initialize_compatible)
        : isolation_available(capability), initialize_queues(startup) {}
    vr::EVRInitError Init(vr::IVRDriverContext *context) override {
        try {
            const auto user = getpwuid(getuid());
            if (!user || !user->pw_name)
                throw std::runtime_error("Cannot identify tracking user");
            if (!std::strcmp(user->pw_name, "steamos")) {
                if (getuid() != 1000 || !user->pw_dir)
                    throw std::runtime_error("Cannot locate tracking recovery directory");
                recovery = std::make_unique<ct::StartupRecovery>(
                    ct::startup_recovery(std::filesystem::path(user->pw_dir) /
                                         ".local/share/controller-utils/app-data"));
                if (!recovery->begin())
                    return vr::VRInitError_None;
                recovery_started = true;
                recovery_healthy_requested = false;
                recovery_start = now();
            }
        } catch (const std::exception &error) {
            std::fprintf(stderr, "Controller Utils recovery: %s\n", error.what());
            if (recovery) {
                try {
                    recovery->fail();
                } catch (const std::exception &failure) {
                    std::fprintf(stderr, "Controller Utils recovery: %s\n", failure.what());
                }
            }
            return vr::VRInitError_None;
        }
        struct StartupCompletion {
            ct::StartupRecovery *recovery;
            bool *started;
            bool completed = false;
            ~StartupCompletion() {
                if (!completed && recovery && *started) {
                    try {
                        recovery->clean();
                    } catch (const std::exception &) {
                    }
                    *started = false;
                }
            }
        } startup_completion{recovery.get(), &recovery_started};
        vr::EVRInitError error = vr::VRInitError_None;
        log = static_cast<vr::IVRDriverLog *>(
            context->GetGenericInterface(vr::IVRDriverLog_Version, &error));
        queue = static_cast<ct::steamvr::BlockQueue *>(
            context->GetGenericInterface("IVRBlockQueue_005", &error));
        if (!queue || error)
            return vr::VRInitError_Init_InterfaceNotFound;
        paths =
            static_cast<ct::steamvr::Paths *>(context->GetGenericInterface("IVRPaths_002", &error));
        if (!paths || error)
            return vr::VRInitError_Init_InterfaceNotFound;
        auto *host = static_cast<vr::IVRServerDriverHost *>(
            context->GetGenericInterface(vr::IVRServerDriverHost_Version, &error));
        if (!host || error)
            return vr::VRInitError_Init_InterfaceNotFound;
        auto *input_api = static_cast<vr::IVRDriverInput *>(
            context->GetGenericInterface(vr::IVRDriverInput_Version, &error));
        if (!input_api || error)
            return vr::VRInitError_Init_InterfaceNotFound;
        auto *properties = static_cast<vr::IVRProperties *>(
            context->GetGenericInterface(vr::IVRProperties_Version, &error));
        if (!properties || error)
            return vr::VRInitError_Init_InterfaceNotFound;
        tracked_input = std::make_unique<ct::TrackedInput>(host, input_api, properties);
        const char *names[] = {"/controllerOnboardConfigData/blockDataSize",
                               "/controllerDefaultConfigData/blockDataSize",
                               "/controllerConfigData/deviceSerialNumber"};
        for (unsigned i = 0; i < 3; ++i)
            if (paths->StringToHandle(&property_paths[i], names[i]))
                return vr::VRInitError_Driver_Failed;
        if (!initialize_queues(queue)) {
            if (log)
                log->Log("Controller Utils native queue startup needs a compatibility update");
            Cleanup();
            return vr::VRInitError_Driver_Failed;
        }
        int rc = queue->Create(&input, "/controller-utils/native/input_v1",
                               sizeof(ct::wire::Command), 512, 64, 1);
        if (!rc)
            rc = queue->Create(&output, "/controller-utils/native/status_v1",
                               sizeof(ct::wire::Status), 64, 4, 0);
        if (rc) {
            Cleanup();
            return vr::VRInitError_Driver_Failed;
        }
        publish_status();
        if (log)
            log->Log("Controller Utils native adapter ready; waiting for explicit input");
        startup_completion.completed = true;
        return vr::VRInitError_None;
    }
    void Cleanup() override {
        if (recovery && recovery_started) {
            try {
                std::lock_guard<std::mutex> lock(*recovery_lock);
                recovery->clean();
            } catch (const std::exception &error) {
                std::fprintf(stderr, "Controller Utils recovery: %s\n", error.what());
            }
            recovery_started = false;
        }
        disconnect("driver cleanup");
        for (auto *handle : {&pose, &output, &input}) {
            if (queue && *handle)
                queue->Destroy(*handle);
            *handle = 0;
        }
        // These IDs are borrowed from the stock driver in this same vrserver.
        // ct::steamvr::BlockQueue Destroy removes the process-wide owner and reader entries,
        // Connect does not give each caller a separately owned reference.
        native_data = native_event = 0;
        status = ct::wire::Status{};
        serial.clear();
        last_imu = last_status = last_disconnect = last_app_input = 0;
    }
    const char *const *GetInterfaceVersions() override {
        return vr::k_InterfaceVersions;
    }
    void RunFrame() override {
        run_frame(now());
    }
    void run_frame(double time) {
        if (recovery && recovery_started && !recovery_healthy_requested &&
            time - recovery_start >= 30) {
            recovery_healthy_requested = true;
            try {
                auto state = *recovery;
                auto lock = recovery_lock;
                std::thread([state, lock] {
                    try {
                        std::lock_guard<std::mutex> held(*lock);
                        state.healthy();
                    } catch (const std::exception &error) {
                        std::fprintf(stderr, "Controller Utils recovery: %s\n", error.what());
                    }
                }).detach();
            } catch (const std::exception &error) {
                std::fprintf(stderr, "Controller Utils recovery: %s\n", error.what());
            }
        }
        if (!queue || !input)
            return;
        if (status.connected && !isolation_available(time)) {
            if (log) {
                char message[128];
                std::snprintf(message, sizeof(message),
                              "Controller Utils isolation check failed: %s",
                              ct::tracker_failure_reason);
                log->Log(message);
            }
            disconnect("isolation unavailable");
            report(-8);
        }
        for (unsigned i = 0; i < 32; ++i) {
            uint64_t block = 0;
            void *buffer = nullptr;
            if (queue->AcquireReadOnlyBlock(input, &block, &buffer, 2))
                break;
            ct::wire::Command command{};
            if (buffer) {
                std::memcpy(&command, buffer, 24);
                if (command.size <= ct::wire::EventSize)
                    std::memcpy(command.data, static_cast<unsigned char *>(buffer) + 24,
                                command.size);
            }
            int rc = queue->ReleaseReadOnlyBlock(input, block);
            ++status.input_count;
            status.last_kind = command.kind;
            if (rc || command.magic != ct::wire::Magic || command.size > ct::wire::EventSize) {
                report(-1);
                continue;
            }
            if (command.kind == 4)
                handle_event(command, time);
            else if (command.kind == 2)
                handle_imu(command, time);
            else if (command.kind == 3)
                handle_app_input(command, time);
            else
                report(-1);
        }
        // Stale menu input yields in update_input, it must not tear down live IMU tracking.
        if (status.connected &&
            (time - status.last_input > .5 ||
             (!last_app_input && time - status.session_start > 30)) &&
            time - last_disconnect > .1) {
            last_disconnect = time;
            disconnect(time - status.last_input > .5 ? "IMU timeout" : "bench timeout");
        }
        update_input(time);
        status.pose_controller_id = 0;
        if (pose && status.connected) {
            uint64_t block = 0;
            void *buffer = nullptr;
            if (!queue->AcquireReadOnlyBlock(pose, &block, &buffer, 0)) {
                if (buffer) {
                    std::memcpy(&status.pose_controller_id, buffer, 4);
                    std::memcpy(&status.last_pose, static_cast<unsigned char *>(buffer) + 8, 8);
                    if (!std::isfinite(status.last_pose)) {
                        status.last_pose = 0;
                        status.pose_controller_id = 0;
                    } else if (std::abs(time - status.last_pose) > .5)
                        status.pose_controller_id = 0;
                }
                queue->ReleaseReadOnlyBlock(pose, block);
            }
        }
        if (time - last_status > .1) {
            publish_status();
            last_status = time;
        }
    }
    bool ShouldBlockStandbyMode() override {
        return false;
    }
    void EnterStandby() override {
        disconnect("SteamVR standby");
    }
    void LeaveStandby() override {}
};

extern "C" __attribute__((visibility("default"))) void *HmdDriverFactory(const char *name,
                                                                         int *error) {
    static NativeBridge provider;
    if (name && !std::strcmp(name, vr::IServerTrackedDeviceProvider_Version))
        return &provider;
    if (error)
        *error = vr::VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
