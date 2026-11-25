#pragma once
#include "report.h"
#include "bluetooth.h"
#include "platform/linux/resource.h"
#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace ct {
enum class ControllerConnection { Puck, Bluetooth };

class HidTransport {
  public:
    virtual ~HidTransport() = default;
    virtual void send(const std::array<uint8_t, 64> &report) = 0;
    virtual std::array<uint8_t, 64> feature() = 0;
    virtual std::optional<std::vector<uint8_t>> receive(double timeout) = 0;
    virtual double now() = 0;
    virtual void wait(double seconds) = 0;
    virtual bool menu_reports(bool) {
        return false;
    }
    virtual void close() noexcept = 0;
};

class LinuxHidTransport final : public HidTransport {
    FileDescriptor descriptor_;
    BluetoothReports menu_;
    uint8_t hid_report_ = 0;
    double menu_retry_ = 0, menu_started_ = 0;
    bool bluetooth_ = false;

  public:
    static std::filesystem::path discover(ControllerConnection connection,
                                          const std::filesystem::path &sysfs = "/sys/class/hidraw",
                                          const std::filesystem::path &devices = "/dev");
    explicit LinuxHidTransport(ControllerConnection connection);
    void send(const std::array<uint8_t, 64> &report) override;
    std::array<uint8_t, 64> feature() override;
    std::optional<std::vector<uint8_t>> receive(double timeout) override;
    double now() override;
    void wait(double seconds) override;
    bool menu_reports(bool enabled) override;
    void close() noexcept override;
};

class ControllerDevice {
    std::unique_ptr<HidTransport> transport_;
    bool closed_ = false;
    std::optional<std::string> serial_;
    std::array<uint8_t, 64> query(uint8_t opcode, const std::vector<uint8_t> &payload);
    std::vector<ControllerInput> collect(double duration);

  public:
    struct Cleanup {
        std::optional<uint16_t> ir_returned_to_read_value;
        bool device_disconnected = false;
        std::optional<bool> imu_disabled_behavior_verified;
    } cleanup;
    bool changed_imu = false;
    std::optional<uint16_t> old_brightness;
    explicit ControllerDevice(ControllerConnection connection);
    explicit ControllerDevice(std::unique_ptr<HidTransport> transport);
    ControllerDevice(const ControllerDevice &) = delete;
    ControllerDevice &operator=(const ControllerDevice &) = delete;
    ~ControllerDevice();
    std::string serial();
    std::optional<ControllerInput> read(double timeout);
    bool menu_reports(bool enabled) {
        return !closed_ && transport_->menu_reports(enabled);
    }
    void enable_imu();
    void resume_imu();
    void enable_ir(unsigned brightness = 100);
    void restore();
    void close();
};
} // namespace ct
