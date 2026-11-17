#include "backend.h"
#include "worker.h"
#include "platform/linux/error.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace ct::helper {
namespace {
double seconds(uint64_t raw) {
    return double(raw / 1000000000ULL) + double(raw % 1000000000ULL) * 1e-9;
}
json::Value message(const char *operation) {
    json::Value result(json_object_new_object());
    json_object_object_add(result.get(), "op", json_object_new_string(operation));
    return result;
}
json::Value quality(double time, bool good) {
    auto result = message("native_helper_quality");
    json_object_object_add(result.get(), "time", json_object_new_double(time));
    json_object_object_add(result.get(), "good", json_object_new_boolean(good));
    return result;
}
} // namespace
XrBackend::XrBackend(BackendHost &host, std::function<double()> monotonic,
                     std::function<uint64_t()> raw, std::function<void(double)> sleep)
    : host_(host), monotonic_(monotonic ? std::move(monotonic) : monotonic_time),
      raw_(raw ? std::move(raw) : raw_time_ns), sleep_(sleep ? std::move(sleep) : sleep_for),
      control_(host_),
      acquisition_([this](uintptr_t address, size_t size) { return host_.read(address, size); },
                   [this] { return seconds(raw_()); }) {
    host_.refresh();
    if (host_.read(CameraPatch::Address, 8) != CameraPatch::Original)
        throw std::runtime_error("Native compatibility state differs from the supported original.");
}
bool XrBackend::start() {
    if (started_)
        return true;
    host_.refresh();
    const auto result = host_.patch(TrackerPatch::Install);
    if (result == PatchResult::Busy) {
        if (++attempts_ >= 5)
            throw std::runtime_error("Native detector is busy. Try Start again.");
        return false;
    }
    control_.begin(raw_());
    started_ = true;
    started_at_ = frames_at_ = monotonic_();
    return true;
}
void XrBackend::renew() {
    if (started_)
        control_.renew(raw_());
}
std::vector<json::Value> XrBackend::poll(bool requested) {
    if (!started_)
        throw std::logic_error("native tracking backend has not started");
    std::vector<json::Value> events;
    host_.refresh();
    const auto now = monotonic_();
    const auto state = control_.state();
    if (state.generation != *control_.generation() || state.error ||
        state.phase == tracker::Stopping || state.phase == tracker::Stopped ||
        state.phase == tracker::Failed || state.worker_exited)
        throw std::runtime_error("Private native tracker stopped (phase " +
                                 std::to_string(state.phase) + ", error " +
                                 std::to_string(state.error) + ").");
    if (state.frames != frames_) {
        frames_ = state.frames;
        frames_at_ = now;
    }
    // Camera handoffs can pause frames briefly after acquisition has completed.
    const double frame_timeout = announced_ && !requested ? 1.25 : .5;
    bool live = state.phase == tracker::Ready && frames_ && now - frames_at_ < frame_timeout;
    if (live)
        live = host_.publish(state, raw_());
    else
        host_.withdraw(raw_());
    if (!announced_ && live) {
        auto ready = message("native_helper_ready");
        json_object_object_add(ready.get(), "isolated", json_object_new_boolean(true));
        events.push_back(std::move(ready));
        announced_ = true;
    }
    if (!announced_ && now - started_at_ > 12)
        throw std::runtime_error("Private native tracker did not receive camera frames in time.");
    if (now >= sample_at_) {
        if (announced_ && requested) {
            bool good = false;
            try {
                const auto snapshot = acquisition_.snapshot(state.wrapper);
                good = acquisition_quality(snapshot.get()).ready();
            } catch (const std::invalid_argument &) {
            }
            events.push_back(quality(seconds(raw_()), good));
        }
        auto status = tracker_status(state);
        json_object_object_add(status.get(), "op", json_object_new_string("native_tracker_state"));
        events.push_back(std::move(status));
        sample_at_ = now + .5;
    }
    return events;
}

void XrBackend::restore(const std::function<void()> &pulse) {
    if (pulse)
        pulse();
    host_.withdraw(raw_());
    if (!host_.alive())
        return;
    host_.refresh();
    const auto raw = host_.read(AdapterPatch::Control, sizeof(tracker::Control));
    if (raw == Bytes(raw.size())) {
        host_.patch(TrackerPatch::VerifyOriginal);
        return;
    }
    uint64_t magic = 0;
    uint32_t version = 0;
    std::memcpy(&magic, raw.data(), 8);
    std::memcpy(&version, raw.data() + 8, 4);
    if (magic == tracker::Magic && version == 1)
        control_.request_stop();
    const auto stopped = host_.read(AdapterPatch::Control, sizeof(tracker::Control));
    if (magic != tracker::Magic || version != 1 ||
        std::all_of(stopped.begin() + 24, stopped.end(), [](uint8_t byte) { return !byte; })) {
        host_.patch(TrackerPatch::RecoverUnarmed);
        return;
    }
    const auto deadline = monotonic_() + 2;
    while (!control_.stopped()) {
        if (monotonic_() >= deadline)
            throw std::runtime_error("Private native worker has not stopped. Restart SteamVR "
                                     "before using the previous tracker.");
        sleep_(.01);
        host_.refresh();
        if (pulse)
            pulse();
    }
    if (pulse)
        pulse();
    host_.patch(TrackerPatch::CameraRestore);
}
} // namespace ct::helper
