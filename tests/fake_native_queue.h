#pragma once
#include "steamvr/transport.h"
#include "platform/linux/error.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace ct::test {
using ct::steamvr::BlockQueue;
struct Clock {
    double value = 0;
    double now() const {
        return value;
    }
    void sleep(double seconds) {
        value += seconds;
    }
};
class Queue final : public BlockQueue {
  public:
    std::map<std::string, uint64_t> paths{{"/controller-utils/native/input_v1", 1},
                                          {"/controller-utils/native/status_v1", 2},
                                          {"/xrservice/controller_16/pose", 3},
                                          {"/xrservice/controller/data", 4},
                                          {"/xrservice/controller/event", 5}};
    std::map<uint64_t, std::vector<uint8_t>> packets;
    std::vector<uint64_t> opened, destroyed, read_releases;
    std::vector<ct::wire::Command> writes;
    std::map<std::string, int> connect_errors;
    bool partial_connect = false, null_read = false, null_write = false, ack_disconnect = true;
    int write_result = 0, release_error_once = 0, destroy_error = 0;
    unsigned count = 10;
    bool advancing_status = false;
    std::function<double()> connection_time;
    ct::wire::Command buffer{};
    Queue() {
        status(false, 0);
    }
    void status(bool connected, double session, int error = 0, unsigned version = 3,
                bool advance = false) {
        if (advance)
            ++count;
        ct::wire::Status value;
        value.version = version;
        value.connected = connected;
        value.input_count = count;
        value.error = error;
        value.session_start = session;
        value.native_ready = 1;
        packets[2].resize(sizeof(value));
        std::memcpy(packets[2].data(), &value, sizeof(value));
    }
    void pose(double timestamp, uint32_t controller = 16, bool malformed = false) {
        ct::wire::PoseBlock value{};
        value.controller_id = controller;
        value.timestamp = timestamp;
        value.orientation[0] = malformed ? std::numeric_limits<double>::quiet_NaN() : 1;
        value.position[0] = .1;
        value.position[1] = .2;
        value.position[2] = -.5;
        packets[3].resize(sizeof(value));
        std::memcpy(packets[3].data(), &value, sizeof(value));
    }
    int Create(uint64_t *, const char *, uint32_t, uint32_t, uint32_t, uint32_t) override {
        return 7;
    }
    int Connect(uint64_t *handle, const char *path) override {
        if (const auto error = connect_errors.find(path); error != connect_errors.end()) {
            if (partial_connect) {
                *handle = paths.at(path);
                opened.push_back(*handle);
            }
            return error->second;
        }
        *handle = paths.at(path);
        opened.push_back(*handle);
        return 0;
    }
    int Destroy(uint64_t handle) override {
        destroyed.push_back(handle);
        return destroy_error;
    }
    int AcquireWriteOnlyBlock(uint64_t handle, uint64_t *block, void **pointer) override {
        assert(handle == 1);
        if (write_result)
            return write_result;
        *block = 42;
        *pointer = null_write ? nullptr : &buffer;
        return 0;
    }
    int ReleaseWriteOnlyBlock(uint64_t handle, uint64_t block) override {
        assert(handle == 1 && block == 42);
        writes.push_back(buffer);
        if (!null_write && (buffer.kind == 1 || buffer.kind == 4)) {
            ct::wire::EventHeader event{};
            std::memcpy(&event, buffer.data, sizeof(event));
            if (event.connected || ack_disconnect)
                status(event.connected, connection_time ? connection_time() : 100, 0, 3, true);
        }
        const auto error = release_error_once;
        release_error_once = 0;
        return error;
    }
    int WaitAndAcquireReadOnlyBlock(uint64_t, uint64_t *, void **, int, uint32_t) override {
        return 7;
    }
    int AcquireReadOnlyBlock(uint64_t handle, uint64_t *block, void **pointer, int mode) override {
        assert(mode == 0);
        if (handle == 2 && advancing_status) {
            ct::wire::Status state{};
            std::memcpy(&state, packets.at(2).data(), sizeof(state));
            state.input_count = ++count;
            std::memcpy(packets.at(2).data(), &state, sizeof(state));
        }
        const auto packet = packets.find(handle);
        if (packet == packets.end())
            return 3;
        *block = handle + 100;
        *pointer = null_read ? nullptr : packet->second.data();
        return 0;
    }
    int ReleaseReadOnlyBlock(uint64_t handle, uint64_t block) override {
        assert(block == handle + 100);
        read_releases.push_back(handle);
        return 0;
    }
    int QueueHasReader(uint64_t, bool *) override {
        return 7;
    }
};
} // namespace ct::test
