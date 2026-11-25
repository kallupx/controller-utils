#include "bluetooth.h"
#include <array>
#include <cctype>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <string>
#include <string_view>
#include <sys/ioctl.h>

namespace ct {
namespace {
using Message = std::unique_ptr<sd_bus_message, decltype(&sd_bus_message_unref)>;
constexpr char Bluez[] = "org.bluez";
constexpr char Characteristic[] = "org.bluez.GattCharacteristic1";

std::string device_component(int hid) {
    std::array<char, 32> address{};
    if (ioctl(hid, HIDIOCGRAWUNIQ(address.size()), address.data()) < 0 || address[17])
        return {};
    std::string path = "/dev_";
    for (unsigned i = 0; i < 17; ++i) {
        const auto c = static_cast<unsigned char>(address[i]);
        if ((i % 3 == 2) ? c != ':' : !std::isxdigit(c))
            return {};
        path += c == ':' ? '_' : char(std::toupper(c));
    }
    return path + "/";
}

std::string report_path(sd_bus *bus, const std::string &device, std::string_view uuid) {
    sd_bus_message *raw = nullptr;
    if (sd_bus_call_method(bus, Bluez, "/", "org.freedesktop.DBus.ObjectManager",
                           "GetManagedObjects", nullptr, &raw, "") < 0)
        return {};
    Message reply(raw, sd_bus_message_unref);
    if (sd_bus_message_enter_container(raw, 'a', "{oa{sa{sv}}}") <= 0)
        return {};
    std::string match;
    while (sd_bus_message_enter_container(raw, 'e', "oa{sa{sv}}") > 0) {
        const char *object = nullptr;
        if (sd_bus_message_read(raw, "o", &object) <= 0)
            return {};
        std::string_view path(object);
        if (path.find(device) == path.npos) {
            if (sd_bus_message_skip(raw, "a{sa{sv}}") < 0)
                return {};
        } else {
            if (sd_bus_message_enter_container(raw, 'a', "{sa{sv}}") <= 0)
                return {};
            while (sd_bus_message_enter_container(raw, 'e', "sa{sv}") > 0) {
                const char *iface = nullptr;
                if (sd_bus_message_read(raw, "s", &iface) <= 0)
                    return {};
                if (std::string_view(iface) == Characteristic) {
                    if (sd_bus_message_enter_container(raw, 'a', "{sv}") <= 0)
                        return {};
                    while (sd_bus_message_enter_container(raw, 'e', "sv") > 0) {
                        const char *key = nullptr;
                        if (sd_bus_message_read(raw, "s", &key) <= 0)
                            return {};
                        if (std::string_view(key) == "UUID") {
                            const char *value = nullptr;
                            if (sd_bus_message_read(raw, "v", "s", &value) <= 0)
                                return {};
                            if (value == uuid) {
                                if (!match.empty())
                                    return {};
                                match = path;
                            }
                        } else if (sd_bus_message_skip(raw, "v") < 0)
                            return {};
                        if (sd_bus_message_exit_container(raw) < 0)
                            return {};
                    }
                    if (sd_bus_message_exit_container(raw) < 0)
                        return {};
                } else if (sd_bus_message_skip(raw, "a{sv}") < 0)
                    return {};
                if (sd_bus_message_exit_container(raw) < 0)
                    return {};
            }
            if (sd_bus_message_exit_container(raw) < 0)
                return {};
        }
        if (sd_bus_message_exit_container(raw) < 0)
            return {};
    }
    return match;
}
} // namespace

BluetoothReports acquire_bluetooth_reports(int hid, uint8_t report_id) {
    if (report_id != 0x45 && report_id != 0x47)
        return {};
    const auto device = device_component(hid);
    sd_bus *raw = nullptr;
    if (device.empty() || sd_bus_open_system(&raw) < 0)
        return {};
    BluetoothReports result;
    result.bus.reset(raw);
    if (sd_bus_set_method_call_timeout(raw, 150000) < 0)
        return {};
    const auto path = report_path(raw, device,
                                  report_id == 0x45 ? "100f6c7a-1735-4313-b402-38567131e5f3"
                                                    : "100f6c7c-1735-4313-b402-38567131e5f3");
    sd_bus_message *response = nullptr;
    if (path.empty() || sd_bus_call_method(raw, Bluez, path.c_str(), Characteristic,
                                           "AcquireNotify", nullptr, &response, "a{sv}", 0) < 0)
        return {};
    Message reply(response, sd_bus_message_unref);
    int fd = -1;
    uint16_t mtu = 0;
    if (sd_bus_message_read(response, "hq", &fd, &mtu) <= 0 || mtu < 48)
        return {};
    result.socket.reset(fcntl(fd, F_DUPFD_CLOEXEC, 3));
    if (!result.socket)
        return {};
    result.id = report_id;
    return result;
}
} // namespace ct
