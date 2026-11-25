#include "device.h"
#include "platform/linux/error.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <exception>
#include <fstream>
#include <iostream>
#include <linux/hidraw.h>
#include <poll.h>
#include <set>
#include <stdexcept>
#include <system_error>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <fcntl.h>

namespace ct {
std::filesystem::path LinuxHidTransport::discover(ControllerConnection connection,
                                                  const std::filesystem::path &sysfs,
                                                  const std::filesystem::path &devices) {
    std::vector<std::filesystem::path> candidates;
    std::error_code error;
    for (const auto &node : std::filesystem::directory_iterator(sysfs, error)) {
        const auto name = node.path().filename().string();
        if (name.rfind("hidraw", 0) != 0)
            continue;
        std::ifstream input(node.path() / "device/uevent");
        if (!input)
            continue;
        std::string line, id, physical;
        size_t bytes = 0;
        bool valid = true;
        while (std::getline(input, line)) {
            bytes += line.size();
            if (bytes > 16384) {
                valid = false;
                break;
            }
            const auto equal = line.find('=');
            if (equal == std::string::npos) {
                valid = false;
                break;
            }
            if (line.substr(0, equal) == "HID_ID")
                id = line.substr(equal + 1);
            else if (line.substr(0, equal) == "HID_PHYS")
                physical = line.substr(equal + 1);
        }
        if (!valid)
            continue;
        const bool puck = id == "0003:000028DE:00001304" && physical.size() >= 7 &&
                          physical.substr(physical.size() - 7) == "/input2";
        const bool bluetooth = id == "0005:000028DE:00001303";
        if (connection == ControllerConnection::Puck ? puck : bluetooth)
            candidates.push_back(devices / name);
    }
    if (candidates.size() != 1)
        throw std::runtime_error(std::string("connect one Steam Controller via ") +
                                 (connection == ControllerConnection::Puck ? "puck" : "bluetooth"));
    return candidates.front();
}
LinuxHidTransport::LinuxHidTransport(ControllerConnection connection)
    : bluetooth_(connection == ControllerConnection::Bluetooth) {
    const auto path = discover(connection);
    descriptor_.reset(open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC));
    if (!descriptor_)
        throw std::system_error(errno, std::generic_category(), "open controller");
    hidraw_devinfo info{};
    if (ioctl(descriptor_.get(), HIDIOCGRAWINFO, &info) < 0)
        throw std::system_error(errno, std::generic_category(), "controller identity");
    const auto bus = connection == ControllerConnection::Bluetooth ? 5u : 3u;
    const auto product = connection == ControllerConnection::Bluetooth ? 0x1303 : 0x1304;
    if (info.bustype != bus || uint16_t(info.vendor) != 0x28de || uint16_t(info.product) != product)
        throw std::invalid_argument("unexpected controller identity");
}
void LinuxHidTransport::send(const std::array<uint8_t, 64> &report) {
    auto data = report;
    const int result = ioctl(descriptor_.get(), HIDIOCSFEATURE(64), data.data());
    if (result < 0)
        throw std::system_error(errno, std::generic_category(), "controller feature command");
    if (result != 64)
        throw std::system_error(EIO, std::generic_category(),
                                "feature command did not send 64 bytes");
}
std::array<uint8_t, 64> LinuxHidTransport::feature() {
    std::array<uint8_t, 64> result{};
    result[0] = 1;
    if (ioctl(descriptor_.get(), HIDIOCGFEATURE(64), result.data()) < 0)
        throw std::system_error(errno, std::generic_category(), "controller feature reply");
    return result;
}
std::optional<std::vector<uint8_t>> LinuxHidTransport::receive(double timeout) {
    if (!std::isfinite(timeout) || timeout < 0 || timeout > 60)
        throw std::invalid_argument("invalid controller read timeout");
    pollfd pending[] = {{descriptor_.get(), POLLIN, 0}, {menu_.socket.get(), POLLIN, 0}};
    int ready;
    const auto deadline = monotonic_time() + timeout;
    do {
        ready = poll(pending, menu_.socket ? 2 : 1,
                     int(std::ceil(std::max(0., deadline - monotonic_time()) * 1000)));
    } while (ready < 0 && errno == EINTR);
    if (ready < 0)
        throw std::system_error(errno, std::generic_category(), "controller read readiness");
    if (!ready)
        return {};
    std::vector<uint8_t> packet(64);
    if (menu_.socket && pending[1].revents && !pending[0].revents) {
        // The characteristic carries the HID payload without its report ID.
        const auto n = recv(menu_.socket.get(), packet.data() + 1, packet.size() - 1,
                            MSG_DONTWAIT | MSG_TRUNC);
        if (n == 45 || (n == 53 && menu_.id == 0x45)) {
            packet[0] = menu_.id;
            packet.resize(size_t(n) + 1);
            return packet;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            return {};
        menu_reports(false);
        menu_retry_ = monotonic_time() + 1;
        return {};
    }
    const auto count = ::read(descriptor_.get(), packet.data(), packet.size());
    if (count < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return {};
        throw std::system_error(errno, std::generic_category(), "controller read");
    }
    if (!count)
        throw std::system_error(ENODEV, std::generic_category(), "controller disconnected");
    packet.resize(size_t(count));
    if (packet[0] == 0x42 || packet[0] == 0x45 || packet[0] == 0x47) {
        // The Bluetooth equivalent of the full 0x42 report uses the same 32-bit sensor clock.
        hid_report_ = packet[0] == 0x42 ? 0x45 : packet[0];
        if (menu_.socket) {
            // One old HID report can be queued when Bluetooth switches streams.
            if (monotonic_time() - menu_started_ < .08)
                return {};
            menu_reports(false);
            menu_retry_ = monotonic_time() + 1;
        }
    }
    return packet;
}
double LinuxHidTransport::now() {
    return monotonic_time();
}
void LinuxHidTransport::wait(double seconds) {
    sleep_for(seconds);
}
bool LinuxHidTransport::menu_reports(bool enabled) {
    if (!enabled) {
        menu_.socket.reset();
        menu_.bus.reset();
        return false;
    }
    const double now = monotonic_time();
    if (!menu_.socket && bluetooth_ && hid_report_ && now >= menu_retry_) {
        menu_ = acquire_bluetooth_reports(descriptor_.get(), hid_report_);
        menu_started_ = monotonic_time();
        menu_retry_ = menu_started_ + (menu_.socket ? 0 : 1);
    }
    return bool(menu_.socket);
}
void LinuxHidTransport::close() noexcept {
    menu_reports(false);
    descriptor_.reset();
}

ControllerDevice::ControllerDevice(ControllerConnection connection)
    : ControllerDevice(std::make_unique<LinuxHidTransport>(connection)) {}
ControllerDevice::ControllerDevice(std::unique_ptr<HidTransport> transport)
    : transport_(std::move(transport)) {
    if (!transport_)
        throw std::invalid_argument("controller transport is missing");
}
ControllerDevice::~ControllerDevice() {
    try {
        close();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
    }
}
std::array<uint8_t, 64> ControllerDevice::query(uint8_t opcode,
                                                const std::vector<uint8_t> &payload) {
    if (closed_)
        throw std::logic_error("controller is closed");
    if (payload.size() > 61)
        throw std::invalid_argument("controller feature payload exceeds capacity");
    std::array<uint8_t, 64> command{};
    command[0] = 1;
    command[1] = opcode;
    command[2] = uint8_t(payload.size());
    std::copy(payload.begin(), payload.end(), command.begin() + 3);
    transport_->send(command);
    for (int i = 0; i < 20; ++i) {
        try {
            const auto reply = transport_->feature();
            if (reply[1] == opcode)
                return reply;
        } catch (const std::system_error &error) {
            if (error.code().value() != EPIPE)
                throw;
        }
        transport_->wait(.02);
    }
    static constexpr char digits[] = "0123456789abcdef";
    throw TimeoutError(std::string("feature query 0x") + digits[opcode >> 4] + digits[opcode & 15] +
                       " did not respond");
}
std::string ControllerDevice::serial() {
    if (closed_)
        throw std::logic_error("controller is closed");
    if (!serial_) {
        const auto reply = query(0xae, {1});
        serial_ = parse_serial(reply.data(), reply.size());
    }
    return *serial_;
}
std::optional<ControllerInput> ControllerDevice::read(double timeout) {
    if (closed_)
        throw std::logic_error("controller is closed");
    const auto packet = transport_->receive(timeout);
    if (!packet)
        return {};
    const double received = transport_->now();
    if (packet->empty())
        throw std::system_error(ENODEV, std::generic_category(), "controller disconnected");
    if ((*packet)[0] != 0x42 && (*packet)[0] != 0x45 && (*packet)[0] != 0x47)
        return {};
    return ControllerInput{decode_report(*packet), received};
}
std::vector<ControllerInput> ControllerDevice::collect(double duration) {
    std::vector<ControllerInput> result;
    const auto end = transport_->now() + duration;
    while (transport_->now() < end) {
        const auto input = read(std::max(0., std::min(.02, end - transport_->now())));
        if (input) {
            if (result.size() == 4096)
                throw std::runtime_error("controller report collection exceeded capacity");
            result.push_back(*input);
        }
    }
    return result;
}
void ControllerDevice::enable_imu() {
    cleanup.imu_disabled_behavior_verified.reset();
    collect(.1);
    const auto baseline = collect(.25);
    if (baseline.size() < 10)
        throw std::runtime_error("controller is asleep or not reporting");
    std::set<uint64_t> ticks;
    for (const auto &sample : baseline)
        ticks.insert(sample.report.sensor_tick);
    if (ticks.size() > 1)
        return;
    resume_imu();
    ticks.clear();
    for (const auto &sample : collect(.3))
        ticks.insert(sample.report.sensor_tick);
    if (ticks.size() < 10)
        throw std::runtime_error("raw IMU did not begin updating");
}
void ControllerDevice::resume_imu() {
    if (closed_)
        throw std::logic_error("controller is closed");
    changed_imu = true;
    cleanup.imu_disabled_behavior_verified.reset();
    transport_->send(setting_report(0x30, 0x18));
}
void ControllerDevice::enable_ir(unsigned brightness) {
    if (closed_)
        throw std::logic_error("controller is closed");
    const auto command = setting_report(0x24, brightness);
    if (!old_brightness) {
        const auto reply = query(0x89, {0x24, 0, 0});
        if (reply[0] != 1 || reply[1] != 0x89 || reply[2] != 3 || reply[3] != 0x24)
            throw std::invalid_argument("unexpected IR setting response");
        const auto previous = uint16_t(reply[4]) | uint16_t(reply[5]) << 8;
        if (previous > 100)
            throw std::invalid_argument("IR setting outside supported range");
        old_brightness = uint16_t(previous);
        cleanup.ir_returned_to_read_value.reset();
    }
    transport_->send(command);
}
void ControllerDevice::restore() {
    if (closed_)
        return;
    std::vector<std::string> errors;
    auto failure = [&](const std::exception &error) {
        const auto *io = dynamic_cast<const std::system_error *>(&error);
        const int code = io ? io->code().value() : 0;
        if (code == ENODEV || code == EIO || code == ENXIO)
            cleanup.device_disconnected = true;
        else
            errors.emplace_back(error.what());
    };
    if (old_brightness) {
        try {
            transport_->send(setting_report(0x24, *old_brightness));
            cleanup.ir_returned_to_read_value = old_brightness;
            old_brightness.reset();
        } catch (const std::exception &error) {
            failure(error);
        }
    }
    if (changed_imu) {
        try {
            transport_->send(setting_report(0x30, 0));
            collect(.15);
            const auto samples = collect(.3);
            std::set<uint64_t> ticks;
            for (const auto &sample : samples)
                ticks.insert(sample.report.sensor_tick);
            cleanup.imu_disabled_behavior_verified = !samples.empty() && ticks.size() == 1;
            if (!*cleanup.imu_disabled_behavior_verified)
                errors.emplace_back(
                    "IMU disable command sent, but disabled behavior was not verified");
            else
                changed_imu = false;
        } catch (const std::exception &error) {
            failure(error);
        }
    }
    if (cleanup.device_disconnected) {
        old_brightness.reset();
        changed_imu = false;
    }
    if (!errors.empty()) {
        std::string message = "controller cleanup: ";
        for (const auto &error : errors) {
            if (message != "controller cleanup: ")
                message += "; ";
            message += error;
        }
        throw std::runtime_error(message);
    }
}
void ControllerDevice::close() {
    if (closed_)
        return;
    std::exception_ptr failure;
    try {
        restore();
    } catch (...) {
        failure = std::current_exception();
    }
    transport_->close();
    closed_ = true;
    if (failure)
        std::rethrow_exception(failure);
}
} // namespace ct
