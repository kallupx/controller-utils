#pragma once
#include "tracking/math.h"
#include "touchpad_cursor.h"
#include <optional>
#include <stdexcept>
#include <string>
#include <openvr.h>

namespace ct {
class RuntimeView {
  public:
    virtual ~RuntimeView() = default;
    virtual std::string serial(uint32_t index) = 0;
    virtual std::optional<RigidPose> head_pose(int origin = 1) = 0;
    virtual std::optional<unsigned> primary_dashboard_device() {
        return std::nullopt;
    }
    virtual bool set_menu_cursor(MenuCursor) {
        return false;
    }
};
class Runtime final : public RuntimeView {
    void *library_ = nullptr;
    uint32_t (*initialize_)(vr::EVRInitError *, vr::EVRApplicationType) = nullptr;
    void (*shutdown_)() = nullptr;
    void *(*interface_)(const char *, vr::EVRInitError *) = nullptr;
    bool initialized_ = false;
    TouchpadCursor cursor_;

  public:
    vr::IVRSystem *system = nullptr;
    vr::IVROverlay *overlay = nullptr;
    explicit Runtime(bool overlay = false,
                     const std::string &library = "/opt/steamvr/bin/linuxarm64/libopenvr_api.so");
    Runtime(const Runtime &) = delete;
    Runtime &operator=(const Runtime &) = delete;
    ~Runtime() override;
    void *interface(const char *version) {
        if (!initialized_)
            throw std::logic_error("OpenVR runtime is closed");
        vr::EVRInitError error = vr::VRInitError_None;
        auto value = interface_(version, &error);
        if (!value || error)
            throw std::runtime_error(std::string(version) + ": " + std::to_string(error));
        return value;
    }
    std::string serial(uint32_t index) override;
    std::optional<RigidPose> head_pose(int origin = 1) override;
    std::optional<unsigned> primary_dashboard_device() override;
    bool set_menu_cursor(MenuCursor mode) override;
    uint64_t path_handle(const char *name);
    void close();
};
} // namespace ct
