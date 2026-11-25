#pragma once
#include "controller/device.h"
#include "platform/linux/error.h"
#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <system_error>

namespace ct::test {
struct FakeState {
    bool bluetooth_menu_available = false, bluetooth_menu_active = false;
    double time = 1;
    bool raw = false, ignore_disable = false, reports = true;
    unsigned brightness = 25, feature_misses = 0, feature_reads = 0, closed = 0;
    uint8_t opcode = 0;
    int restoration_error = 0;
    int receive_error = 0;
    std::deque<std::vector<uint8_t>> pending;
    std::vector<std::array<uint8_t, 64>> commands;
};
class FakeTransport final : public ct::HidTransport {
    std::shared_ptr<FakeState> state;

  public:
    explicit FakeTransport(std::shared_ptr<FakeState> state) : state(std::move(state)) {}
    void send(const std::array<uint8_t, 64> &report) override {
        state->commands.push_back(report);
        if (state->restoration_error && report[1] == 0x87 && report[3] == 0x24 && report[4] == 25)
            throw std::system_error(state->restoration_error, std::generic_category());
        state->opcode = report[1];
        if (report[1] == 0x87 && report[3] == 0x24)
            state->brightness = report[4];
        if (report[1] == 0x87 && report[3] == 0x30 && (report[4] || !state->ignore_disable))
            state->raw = report[4] != 0;
    }
    std::array<uint8_t, 64> feature() override {
        ++state->feature_reads;
        if (state->feature_misses) {
            --state->feature_misses;
            throw std::system_error(EPIPE, std::generic_category());
        }
        std::array<uint8_t, 64> result{};
        result[0] = 1;
        result[1] = state->opcode;
        if (state->opcode == 0xae) {
            result[2] = 0x14;
            result[3] = 1;
            std::memcpy(result.data() + 4, "TEST1234", 8);
        } else {
            result[2] = 3;
            result[3] = 0x24;
            result[4] = uint8_t(state->brightness);
        }
        return result;
    }
    std::optional<std::vector<uint8_t>> receive(double timeout) override {
        if (state->receive_error)
            throw std::system_error(state->receive_error, std::generic_category());
        if (!state->pending.empty()) {
            auto packet = std::move(state->pending.front());
            state->pending.pop_front();
            return packet;
        }
        if (!state->reports || timeout < .005) {
            state->time += timeout;
            return {};
        }
        state->time += .005;
        std::vector<uint8_t> report(46);
        report[0] = 0x45;
        const uint32_t tick = state->raw ? uint32_t(std::round(state->time * 1000000)) : 1234;
        for (size_t i = 0; i < 4; ++i)
            report[30 + i] = uint8_t(tick >> (i * 8));
        return report;
    }
    double now() override {
        return state->time;
    }
    void wait(double seconds) override {
        state->time += seconds;
    }
    bool menu_reports(bool enabled) override {
        return state->bluetooth_menu_active = enabled && state->bluetooth_menu_available;
    }
    void close() noexcept override {
        ++state->closed;
    }
};
inline ct::ControllerDevice device(const std::shared_ptr<FakeState> &state) {
    return ct::ControllerDevice(std::make_unique<FakeTransport>(state));
}
} // namespace ct::test
