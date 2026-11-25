#include "shortcuts.h"
#include "compatibility.h"
#include "module.h"
#include "platform/linux/resource.h"
#include <cmath>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <sys/uio.h>
#include <unistd.h>

namespace ct {
bool validate_progress_table(const steamvr::Module &module, const uintptr_t *table,
                             size_t target_size, const char *target_signature) {
    if (!module.type(table, 25, "16CHmdSystemLatest", -24) ||
        !module.contains(reinterpret_cast<const void *>(table[24]), 8, true))
        return false;
    uint32_t words[2];
    std::memcpy(words, reinterpret_cast<const void *>(table[24]), sizeof(words));
    if (words[0] != 0x9100c000 || (words[1] & 0xfc000000) != 0x14000000)
        return false;
    int64_t delta = words[1] & 0x03ffffff;
    if (delta & 0x02000000)
        delta -= 0x04000000;
    delta *= 4;
    const auto instruction = table[24] + 4;
    if ((delta < 0 && instruction < uintptr_t(-delta)) ||
        (delta >= 0 && uintptr_t(delta) > UINTPTR_MAX - instruction))
        return false;
    const auto target =
        delta < 0 ? instruction - uintptr_t(-delta) : instruction + uintptr_t(delta);
    return module.method(target, target_size, target_signature);
}
OpenVrShortcutBackend::~OpenVrShortcutBackend() {
    if (handle_ && mailbox_->undoc2(handle_) != 0)
        std::cerr << "SteamVR shortcut mailbox did not close\n";
}
void OpenVrShortcutBackend::send_room_view(const char *message) {
    if (!handle_) {
        mailbox_ = static_cast<vr::IVRMailbox *>(runtime_.interface("IVRMailbox_001"));
        const auto name = "controllerutils." + std::to_string(getpid());
        uint64_t handle = 0;
        const int error = mailbox_->undoc1(name.c_str(), &handle);
        if (error || !handle) {
            if (handle)
                mailbox_->undoc2(handle);
            throw std::runtime_error("Could not open the SteamVR shortcut mailbox.");
        }
        handle_ = handle;
    }
    if (mailbox_->undoc3(handle_, "vrcompositor_systemlayer", message) != 0)
        throw std::runtime_error("SteamVR did not accept the shortcut. Try again.");
}
void OpenVrShortcutBackend::recenter() {
    const auto chaperone =
        static_cast<vr::IVRChaperone *>(runtime_.interface(vr::IVRChaperone_Version));
    chaperone->ResetZeroPose(vr::TrackingUniverseSeated);
    chaperone->ResetZeroPose(vr::TrackingUniverseStanding);
}
void OpenVrShortcutBackend::toggle_room_view() {
    const auto apps =
        static_cast<vr::IVRApplications *>(runtime_.interface(vr::IVRApplications_Version));
    if (!apps->GetCurrentSceneProcessId()) {
        // The Frame's home background and in-game passthrough have independent states.
        const auto settings =
            static_cast<vr::IVRSettings *>(runtime_.interface(vr::IVRSettings_Version));
        vr::EVRSettingsError error = vr::VRSettingsError_None;
        const bool enabled =
            settings->GetBool(vr::k_pch_Camera_Section, "enableConstructRoomView", &error);
        if (error == vr::VRSettingsError_None) {
            send_room_view(enabled ? R"({"type":"set_camera_roomview_enabled","enabled":false})"
                                   : R"({"type":"set_camera_roomview_enabled","enabled":true})");
            return;
        }
        if (error != vr::VRSettingsError_UnsetSettingHasNoDefault)
            throw std::runtime_error("Could not read the SteamVR passthrough setting.");
    }
    send_room_view(R"({"type":"camera_room_view_toggle"})");
}
void OpenVrShortcutBackend::prepare_events() {
    if (client_)
        return;
    void *client = nullptr;
    try {
        client = runtime_.interface("IVRClientInternal_XXX");
    } catch (const std::exception &) {
        throw std::runtime_error("SteamVR shortcut events unavailable");
    }
    void *table_address = nullptr;
    iovec local{&table_address, sizeof(table_address)}, remote{client, sizeof(table_address)};
    if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != sizeof(table_address))
        throw std::runtime_error("SteamVR shortcut event object unavailable");
    const auto table = static_cast<const uintptr_t *>(table_address);
    try {
        const auto module = steamvr::Module::loaded(table, steamvr::compatibility::VrClient.path);
        if (!validate_progress_table(
                module, table, 180,
                "c166aa2b8ac8ab97ea0593ef162c3c967b85779c2ed209d2a8bedaca48dd55a0"))
            throw std::runtime_error("invalid event method");
    } catch (const std::runtime_error &) {
        throw std::runtime_error("SteamVR shortcut event interface changed");
    }
    post_ = reinterpret_cast<decltype(post_)>(table[24]);
    client_ = client;
}
void OpenVrShortcutBackend::prepare_progress() {
    if (progress_paths_[0])
        return;
    prepare_events();
    const std::array<const char *, 4> names{"/user/head", "/input/system",
                                            "/actions/quickrecenter/in/recenter",
                                            "/actions/controllerutils/in/passthrough"};
    std::array<uint64_t, 4> handles{};
    for (size_t i = 0; i < names.size(); ++i)
        handles[i] = runtime_.path_handle(names[i]);
    progress_paths_ = handles;
}
void OpenVrShortcutBackend::post_progress(const ShortcutProgress &event) {
    if (!progress_paths_[0])
        throw std::logic_error("shortcut progress is not prepared");
    vr::VREvent_t packet{};
    packet.eventType = 1705;
    packet.data.progressUpdate.pathDevice = progress_paths_[0];
    packet.data.progressUpdate.pathInputSource = progress_paths_[1];
    packet.data.progressUpdate.pathProgressAction =
        progress_paths_[event.action == ShortcutAction::Recenter ? 2 : 3];
    packet.data.progressUpdate.fProgress = float(event.amount);
    post_event(packet);
}
void OpenVrShortcutBackend::post_event(const vr::VREvent_t &event) {
    static_assert(sizeof(vr::VREvent_t) == 60 && offsetof(vr::VREvent_t, data) == 12);
    prepare_events();
    post_(client_, &event, 0);
}
NativeShortcuts::NativeShortcuts(ShortcutBackend &backend, std::function<double()> monotonic)
    : backend_(backend), monotonic_(monotonic ? std::move(monotonic) : monotonic_time) {}
NativeShortcuts::~NativeShortcuts() {
    try {
        close();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
    }
}
void NativeShortcuts::send_action(ShortcutAction action) {
    if (closed_)
        throw std::logic_error("shortcuts are closed");
    if (action == ShortcutAction::Screenshot) {
        vr::VREvent_t event{};
        event.eventType = vr::VREvent_ScreenshotTriggered;
        event.trackedDeviceIndex = vr::k_unTrackedDeviceIndexInvalid;
        backend_.post_event(event);
        return;
    }
    if (action == ShortcutAction::Passthrough)
        backend_.toggle_room_view();
    else if (action == ShortcutAction::Recenter)
        backend_.recenter();
    else
        throw std::invalid_argument("unsupported Frame shortcut");
}
void NativeShortcuts::progress(std::optional<ShortcutAction> action, double amount) {
    if ((action && *action != ShortcutAction::Recenter && *action != ShortcutAction::Passthrough) ||
        !std::isfinite(amount) || amount < 0 || amount > 1 || (!action && amount != 0))
        throw std::invalid_argument("invalid native shortcut progress");
    if (!action && !progress_action_)
        return;
    if (!progress_ready_) {
        backend_.prepare_progress();
        progress_ready_ = true;
    }
    const double now = monotonic_();
    if (action == progress_action_ && amount != 0 && now - progress_last_ < .05)
        return;
    if (progress_action_ && action != progress_action_)
        backend_.post_progress({*progress_action_, 0});
    if (action)
        backend_.post_progress({*action, amount});
    progress_action_ = action;
    progress_last_ = now;
}
void NativeShortcuts::close() {
    if (!closed_) {
        progress(std::nullopt);
        closed_ = true;
    }
}
} // namespace ct
