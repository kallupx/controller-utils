#include "debug_log.h"
#include "platform/linux/files.h"
#include "platform/linux/resource.h"
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/un.h>

namespace ct {
namespace {
int64_t realtime_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace
DebugLog::DebugLog(const std::filesystem::path &data) : data_(data) {
    try {
        const auto path = data_ / "debug-capture.json";
        if (!std::filesystem::exists(path))
            return;
        const auto saved = json::parse(read_file(path, 256));
        auto id = json::string(json::field(saved.get(), "id"));
        if (id.empty() || id.size() > 17 || id.find_first_not_of("0123456789") != std::string::npos)
            throw std::invalid_argument("Invalid debug capture");
        recording_ = json::boolean(json::field(saved.get(), "recording"));
        const auto stopped = json::integer(json::field(saved.get(), "stopped"));
        if (stopped < 0 || stopped >= 100000000000000000LL)
            throw std::invalid_argument("Invalid debug capture time");
        id_ = std::move(id);
    } catch (const std::exception &) {
        recording_ = false;
        notice_ = "Previous capture unavailable. Turn on logging to start a new capture.";
    }
}
void DebugLog::event(const std::string &message) const {
    if (!recording_)
        return;
    FileDescriptor peer(socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (!peer)
        return;
    auto line = message.substr(0, 600);
    std::replace_if(line.begin(), line.end(), [](unsigned char c) { return c < ' '; }, ' ');
    const std::string packet = "SYSLOG_IDENTIFIER=controller-utils-debug\nCU_CAPTURE_ID=" + id_ +
                               "\nPRIORITY=6\nMESSAGE=" + line + "\n";
    const sockaddr_un journal{AF_UNIX, "/run/systemd/journal/socket"};
    sendto(peer.get(), packet.data(), packet.size(), MSG_NOSIGNAL,
           reinterpret_cast<const sockaddr *>(&journal), sizeof(journal));
}
void DebugLog::state(const std::string &message) {
    if (recording_ && last_state_ != message) {
        event(message);
        last_state_ = message;
    }
}
void DebugLog::action(bool save_only, const ChildFactory &children) {
    if (job_ || (save_only && id_.empty()))
        return;
    try {
        if (!save_only) {
            const auto now = realtime_us();
            const auto id = recording_ ? id_ : std::to_string(now);
            json::Value saved(json_object_new_object());
            json_object_object_add(saved.get(), "id", json_object_new_string(id.c_str()));
            json_object_object_add(saved.get(), "recording", json_object_new_boolean(!recording_));
            json_object_object_add(saved.get(), "stopped",
                                   json_object_new_int64(recording_ ? now : 0));
            atomic_write(data_ / "debug-capture.json", json::encode(saved.get()));
            if (recording_)
                event("Capture stopped");
            recording_ = !recording_;
            id_ = id;
            last_state_.clear();
            if (recording_)
                event("Capture started");
        }
        const bool save = save_only || !recording_;
        notice_ = save ? "Saving to Downloads..." : "Starting debug capture...";
        job_ = children({"/usr/bin/python3", "-IB",
                         (data_.parent_path() / "app/optical/frame_debug.py").string(),
                         save ? "save" : "snapshot", data_.string()});
    } catch (const std::exception &e) {
        notice_ = "Debug logging: " + std::string(e.what()).substr(0, 120);
    }
}
json::Value DebugLog::info() {
    try {
        if (job_ && job_->exit_status()) {
            auto finished = std::move(job_);
            const auto output = finished->poll();
            notice_ =
                output.empty()
                    ? "Debug helper stopped. Save the log to retry."
                    : json::string(json::field(output.back().get(), "message")).substr(0, 180);
        }
    } catch (const std::exception &) {
        job_.reset();
        notice_ = "Debug helper failed. Save the log to retry.";
    }
    json::Value value(json_object_new_object());
    json_object_object_add(value.get(), "recording", json_object_new_boolean(recording_));
    json_object_object_add(value.get(), "available", json_object_new_boolean(!id_.empty()));
    json_object_object_add(value.get(), "busy", json_object_new_boolean(bool(job_)));
    json_object_object_add(value.get(), "message", json_object_new_string(notice_.c_str()));
    return value;
}
} // namespace ct
