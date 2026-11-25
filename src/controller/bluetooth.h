#pragma once
#include "platform/linux/resource.h"
#include <cstdint>
#include <memory>
#include <systemd/sd-bus.h>

namespace ct {
struct BluetoothReports {
    std::unique_ptr<sd_bus, decltype(&sd_bus_unref)> bus{nullptr, sd_bus_unref};
    FileDescriptor socket;
    uint8_t id = 0;
};

// Triton sends state through its Valve GATT service while this notification socket is open.
BluetoothReports acquire_bluetooth_reports(int hid, uint8_t report_id);
} // namespace ct
