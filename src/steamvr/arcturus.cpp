#include "arcturus.h"
#include "compatibility.h"
#include "module.h"
#include "runtime.h"
#include "platform/json.h"
#include "platform/linux/resource.h"
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <string_view>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <systemd/sd-bus.h>

namespace ct {
namespace {
constexpr auto CameraBus = "com.arcturusvision.CameraPreview";
constexpr auto CameraPath = "/com/arcturusvision/CameraPreview";
constexpr auto CameraAction = "set-color-passthrough";
constexpr auto CameraMissing = "Connect the Arcturus Vision Camera to use this shortcut.";
constexpr auto CameraLayoutChanged = "Camera switching is unavailable on this SteamVR build.";
constexpr auto ServiceMissing = "Install and start Camera Control in the Arcturus Vision app.";

bool camera_connected(int descriptor) {
    constexpr auto SensorConnected = _IOR('V', 193, uint32_t);
    uint32_t connected = 0;
    return ioctl(descriptor, SensorConnected, &connected) == 0 && connected == 1;
}

FileDescriptor find_camera() {
    std::error_code error;
    auto node = std::filesystem::directory_iterator("/sys/class/video4linux", error);
    for (; !error && node != std::filesystem::directory_iterator(); node.increment(error)) {
        const auto name = node->path().filename().string();
        if (name.compare(0, 10, "v4l-subdev") != 0)
            continue;
        std::string sensor;
        std::getline(std::ifstream(node->path() / "name"), sensor);
        if (sensor.compare(0, 10, "arcimx616 ") != 0)
            continue;
        FileDescriptor device(
            open(("/dev/" + name).c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW));
        if (device && camera_connected(device.get()))
            return device;
    }
    throw std::runtime_error(CameraMissing);
}

void check_recorder() {
    FileDescriptor lock(
        open("/tmp/arcturus_camera_recorder.lock", O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW));
    if (!lock && errno == ENOENT)
        return;
    struct stat info{};
    if (!lock || fstat(lock.get(), &info) || !S_ISREG(info.st_mode))
        throw std::runtime_error("Could not check whether the Arcturus camera is recording.");
    if (flock(lock.get(), LOCK_SH | LOCK_NB))
        throw std::runtime_error("Finish recording before switching passthrough cameras.");
}

void request_camera_switch(sd_bus *bus, const std::string &owner, int sensor, bool color) {
    check_recorder();
    if (!camera_connected(sensor))
        throw std::runtime_error(CameraMissing);
    pollfd parent{STDIN_FILENO, 0, 0};
    if (poll(&parent, 1, 0) != 0)
        return;
    if (sd_bus_call_method(bus, owner.c_str(), CameraPath, "org.gtk.Actions", "Activate", nullptr,
                           nullptr, "sava{sv}", CameraAction, 1, "b", int(color), 0) < 0)
        throw std::runtime_error("Camera Control did not confirm the request. Try again.");
}

bool uses_color_camera(Runtime &runtime) {
    auto camera = runtime.interface("IVRCameraPassthroughInternal_001");
    const uintptr_t *table = nullptr;
    iovec local{&table, sizeof(table)}, remote{camera, sizeof(table)};
    if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != sizeof(table) || !table)
        throw std::runtime_error(CameraLayoutChanged);
    const auto module = steamvr::Module::loaded(table, steamvr::compatibility::VrClient.path);
    // shortcut: review the private getter when its SteamVR signature changes.
    if (!module.type(table, 10, "28CVRCameraPassthroughInternal") ||
        !module.method(table[9], 160,
                       "00f425021a99219d99db7cb31322c33cfe0710ad5426cf8205be10f432721a92"))
        throw std::runtime_error(CameraLayoutChanged);
    std::array<uint8_t, 5> config{255, 255, 255, 255, 255};
    const auto get = reinterpret_cast<bool (*)(void *, uint8_t *)>(table[9]);
    get(camera, config.data());
    if (std::any_of(config.begin(), config.end(), [](uint8_t byte) { return byte > 1; }))
        throw std::runtime_error(CameraLayoutChanged);
    if (!config[0])
        throw std::runtime_error("Open passthrough and try the camera shortcut again.");
    return config[2] != 0;
}

std::string camera_service(sd_bus *bus) {
    sd_bus_message *raw = nullptr;
    const auto found =
        sd_bus_call_method(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                           "org.freedesktop.DBus", "GetNameOwner", nullptr, &raw, "s", CameraBus);
    std::unique_ptr<sd_bus_message, decltype(&sd_bus_message_unref)> reply(raw,
                                                                           sd_bus_message_unref);
    const char *name = nullptr;
    if (found < 0 || sd_bus_message_read(reply.get(), "s", &name) <= 0 || !name || name[0] != ':')
        throw std::runtime_error(ServiceMissing);
    const std::string owner(name);
    raw = nullptr;
    const auto described = sd_bus_call_method(bus, owner.c_str(), CameraPath, "org.gtk.Actions",
                                              "Describe", nullptr, &raw, "s", CameraAction);
    reply.reset(raw);
    int enabled = 0;
    const char *parameter = nullptr;
    if (described < 0 || sd_bus_message_enter_container(reply.get(), 'r', "bgav") <= 0 ||
        sd_bus_message_read(reply.get(), "bg", &enabled, &parameter) <= 0 || !enabled ||
        !parameter || std::string_view(parameter) != "b")
        throw std::runtime_error(ServiceMissing);
    return owner;
}

void switch_camera() {
    const auto camera = find_camera();
    sd_bus *raw = nullptr;
    const auto opened = sd_bus_open_user(&raw);
    std::unique_ptr<sd_bus, decltype(&sd_bus_unref)> bus(raw, sd_bus_unref);
    if (opened < 0 || sd_bus_set_method_call_timeout(bus.get(), 350000) < 0)
        throw std::runtime_error(ServiceMissing);
    const auto owner = camera_service(bus.get());
    Runtime runtime;
    request_camera_switch(bus.get(), owner, camera.get(), !uses_color_camera(runtime));
}
} // namespace

int camera_switch_worker() {
    alarm(2);
    try {
        switch_camera();
        return 0;
    } catch (const std::exception &error) {
        json::Value result(json_object_new_object());
        json_object_object_add(result.get(), "error", json_object_new_string(error.what()));
        std::cout << json::encode(result.get()) << '\n';
        return 1;
    }
}
} // namespace ct
