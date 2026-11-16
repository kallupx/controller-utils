#include "transport.h"
#include "platform/linux/error.h"
#include "platform/linux/resource.h"
#include <algorithm>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>

namespace ct {
QueueContext::~QueueContext() {
    try {
        close();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
    }
}
uint64_t QueueContext::command_writer() {
    if (!writer_) {
        uint64_t handle = 0;
        const int result = queue_.Connect(&handle, "/controller-utils/native/input_v1");
        if (result || !handle) {
            if (handle) {
                const int cleanup = queue_.Destroy(handle);
                if (cleanup)
                    std::cerr << "Native command queue partial cleanup: " << cleanup << '\n';
            }
            throw std::runtime_error("Native queue /controller-utils/native/input_v1: " +
                                     (result ? std::to_string(result) : "null handle"));
        }
        writer_ = handle;
    }
    return writer_;
}
void QueueContext::preflight() {
    std::vector<uint64_t> handles;
    std::exception_ptr failure;
    try {
        for (const char *path : {"/xrservice/controller/data", "/xrservice/controller/event"}) {
            uint64_t handle = 0;
            const int result = queue_.Connect(&handle, path);
            if (handle)
                handles.push_back(handle);
            if (result == 2)
                throw ConnectionError(
                    "Native controller startup is not ready. Restart SteamVR if this persists.");
            if (result || !handle)
                throw std::runtime_error(std::string("Native tracking readiness ") + path + ": " +
                                         (result ? std::to_string(result) : "null handle"));
        }
    } catch (...) {
        failure = std::current_exception();
    }
    std::string errors;
    for (auto it = handles.rbegin(); it != handles.rend(); ++it) {
        const int result = queue_.Destroy(*it);
        if (result) {
            if (!errors.empty())
                errors += "; ";
            errors += std::to_string(result);
        }
    }
    if (!errors.empty()) {
        if (failure)
            std::cerr << "Native tracking readiness cleanup: " << errors << '\n';
        else
            throw std::runtime_error("Native tracking readiness cleanup: " + errors);
    }
    if (failure)
        std::rethrow_exception(failure);
}
void QueueContext::close() {
    const auto handle = std::exchange(writer_, 0);
    if (handle) {
        const int result = queue_.Destroy(handle);
        if (result)
            throw std::runtime_error("Native command queue cleanup: " + std::to_string(result));
    }
}
NativeTracking::NativeTracking(QueueContext &context, const std::string &profile,
                               std::function<double()> monotonic, std::function<void(double)> sleep)
    : context_(context), monotonic_(monotonic ? std::move(monotonic) : monotonic_time),
      sleep_(sleep ? std::move(sleep) : sleep_for), profile_(profile) {
    started_ = monotonic_();
    try {
        source_ = context_.command_writer();
        status_queue_ = open("/controller-utils/native/status_v1");
        const auto packet = read(status_queue_, sizeof(wire::Status));
        if (!packet)
            throw std::runtime_error("Native tracking driver is not ready.");
        state = decode_status(packet->data(), packet->size());
        if (state->version != 3)
            throw std::runtime_error(
                "Native tracking driver needs the Controller Utils update and a SteamVR restart.");
        if (state->connected)
            throw std::runtime_error("Another native controller session is active.");
        previous_input_count_ = last_input_count_ = state->input_count;
        const auto event = bridge_event(true, 0x47, profile_, profile_);
        requested = true;
        write(event);
    } catch (...) {
        const auto failure = std::current_exception();
        try {
            close();
        } catch (const std::exception &error) {
            std::cerr << "Native startup cleanup: " << error.what() << '\n';
        }
        std::rethrow_exception(failure);
    }
}
NativeTracking::~NativeTracking() {
    try {
        close();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
    }
}
uint64_t NativeTracking::open(const char *path, bool optional) {
    uint64_t handle = 0;
    const int result = context_.queue().Connect(&handle, path);
    if (result) {
        if (handle) {
            const int cleanup = context_.queue().Destroy(handle);
            if (cleanup)
                throw std::runtime_error("Native queue partial cleanup: " +
                                         std::to_string(cleanup));
        }
        if (optional && result == 2)
            return 0;
        throw std::runtime_error(std::string("Native queue ") + path + ": " +
                                 std::to_string(result));
    }
    if (!handle)
        throw std::runtime_error(std::string("Native queue ") + path + ": null handle");
    handles_.push_back(handle);
    return handle;
}
bool NativeTracking::write(const wire::Command &command, bool allow_full) {
    uint64_t block = 0;
    void *pointer = nullptr;
    auto &queue = context_.queue();
    const int result = queue.AcquireWriteOnlyBlock(source_, &block, &pointer);
    if (allow_full && result == 3)
        return false;
    if (result)
        throw std::runtime_error("Native queue write: " + std::to_string(result));
    std::exception_ptr failure;
    try {
        if (!pointer)
            throw std::runtime_error("null native command buffer");
        std::memcpy(pointer, &command, sizeof(command));
    } catch (...) {
        failure = std::current_exception();
    }
    const int released = queue.ReleaseWriteOnlyBlock(source_, block);
    if (released)
        throw std::runtime_error("Native queue publish: " + std::to_string(released));
    if (failure)
        std::rethrow_exception(failure);
    return true;
}
std::optional<std::vector<uint8_t>> NativeTracking::read(uint64_t handle, size_t size) {
    uint64_t block = 0;
    void *pointer = nullptr;
    auto &queue = context_.queue();
    const int result = queue.AcquireReadOnlyBlock(handle, &block, &pointer, 0);
    if (result == 3)
        return {};
    if (result)
        throw std::runtime_error("Native queue read: " + std::to_string(result));
    std::optional<std::vector<uint8_t>> packet;
    std::exception_ptr failure;
    try {
        if (!pointer)
            throw std::runtime_error("null native output buffer");
        const auto *bytes = static_cast<const uint8_t *>(pointer);
        packet.emplace(bytes, bytes + size);
    } catch (...) {
        failure = std::current_exception();
    }
    const int released = queue.ReleaseReadOnlyBlock(handle, block);
    if (released)
        throw std::runtime_error("Native queue read release: " + std::to_string(released));
    if (failure)
        std::rethrow_exception(failure);
    return packet;
}
bool NativeTracking::sample(double timestamp, const ControllerReport &imu) {
    if (closed_)
        throw std::logic_error("Native tracking session is closed.");
    return write(bridge_imu(timestamp, imu.acceleration, imu.angular_velocity), true);
}
bool NativeTracking::controls(double timestamp, const std::optional<BodyPoseMessage> &pose,
                              const InputControls &controls, bool enabled) {
    if (closed_)
        throw std::logic_error("Native tracking session is closed.");
    return session_start &&
           write(bridge_input(*session_start, timestamp, pose, controls, enabled), true);
}
std::optional<NativePoseInput> NativeTracking::poll(double now) {
    if (closed_)
        throw std::logic_error("Native tracking session is closed.");
    if (const auto packet = read(status_queue_, sizeof(wire::Status))) {
        const auto next = decode_status(packet->data(), packet->size());
        if (next.input_count > last_input_count_) {
            state = next;
            last_input_count_ = next.input_count;
            last_status_time_ = now;
            if (next.connected && session_start && *session_start != next.session_start) {
                requested = false;
                throw std::runtime_error("Native tracking session changed unexpectedly.");
            }
            if (next.error == -5)
                throw std::runtime_error(
                    "Turn off the right Frame controller to use native gamepad tracking.");
            if (next.error == -8)
                throw std::runtime_error("Isolated native tracker is unavailable; restart tracking "
                                         "or restore the previous version.");
            if (next.error == -6 && !next.connected && session_start)
                throw ConnectionError("Native tracking session stopped. Reconnecting...");
            if (next.error == -6 && next.connected)
                ++rejected_inputs;
            else if (next.error)
                throw std::runtime_error("Native tracking driver rejected input (" +
                                         std::to_string(next.error) + ").");
            if (next.connected)
                session_start = next.session_start;
            else if (session_start)
                throw std::runtime_error(
                    "Native tracking session stopped. Reconnect the gamepad and start again.");
        }
    }
    if (!session_start) {
        if (monotonic_() - started_ > 2)
            throw TimeoutError("Native tracking driver did not accept the connection.");
        return {};
    }
    if (now - last_status_time_ > 1)
        throw TimeoutError("Native tracking driver stopped responding.");
    if (!pose_queue_)
        pose_queue_ = open("/xrservice/controller_16/pose", true);
    if (!pose_queue_)
        return {};
    const auto packet = read(pose_queue_, sizeof(wire::PoseBlock));
    if (!packet)
        return {};
    NativePoseInput pose;
    try {
        pose = decode_pose(packet->data(), packet->size());
    } catch (const std::invalid_argument &) {
        ++rejected_poses;
        return {};
    }
    if (pose.controller_id != 16)
        throw std::runtime_error("Native tracking returned another controller's pose.");
    if (!pose.valid || pose.timestamp <= std::max(last_pose_time_, *session_start) ||
        now - pose.timestamp < -.02 || now - pose.timestamp > .15)
        return {};
    last_pose_time_ = pose.timestamp;
    return pose;
}
void NativeTracking::close() {
    if (closed_)
        return;
    closed_ = true;
    std::exception_ptr failure;
    if (requested) {
        try {
            write(bridge_event(false, 0x47, profile_, profile_));
            const double deadline = monotonic_() + 1;
            bool confirmed = false;
            while (monotonic_() < deadline) {
                if (const auto packet = read(status_queue_, sizeof(wire::Status))) {
                    const auto next = decode_status(packet->data(), packet->size());
                    if (next.input_count > previous_input_count_ && !next.connected) {
                        confirmed = true;
                        break;
                    }
                }
                sleep_(.01);
            }
            if (!confirmed)
                throw TimeoutError(
                    "Native disconnect was not confirmed; input watchdog will release it.");
        } catch (...) {
            failure = std::current_exception();
        }
        requested = false;
    }
    for (auto it = handles_.rbegin(); it != handles_.rend(); ++it) {
        const int result = context_.queue().Destroy(*it);
        if (result && !failure)
            failure = std::make_exception_ptr(
                std::runtime_error("Native queue disconnect: " + std::to_string(result)));
    }
    handles_.clear();
    if (failure)
        std::rethrow_exception(failure);
}
} // namespace ct
