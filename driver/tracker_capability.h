#pragma once
#include "../src/tracking/native_abi.h"
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ct {
using TrackerCapability = tracker::Capability;
inline thread_local const char *tracker_failure_reason = "not checked";
inline bool valid_tracker_capability(const TrackerCapability &c, uint64_t now, uint64_t start) {
    return tracker::valid_capability(c, now, start);
}
inline bool tracker_ready(double time) {
    auto reject = [](const char *reason) {
        tracker_failure_reason = reason;
        return false;
    };
    int directory = open("/run/controller-utils-native-tracker",
                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0)
        return reject("status directory missing");
    struct stat info{};
    const bool trusted = !fstat(directory, &info) && info.st_uid == 0 && !(info.st_mode & 0022);
    TrackerCapability value{};
    int file =
        trusted ? openat(directory, "status", O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK) : -1;
    bool valid = file >= 0;
    if (valid) {
        valid = !fstat(file, &info) && S_ISREG(info.st_mode) && info.st_uid == 0 &&
                !(info.st_mode & 0022) && info.st_nlink <= 1 && info.st_size == sizeof(value) &&
                read(file, &value, sizeof(value)) == sizeof(value);
        close(file);
        // Either stat can see an unlinked inode during renewal, a missing path still withdraws it.
        valid = valid && !fstatat(directory, "status", &info, AT_SYMLINK_NOFOLLOW) &&
                S_ISREG(info.st_mode) && info.st_uid == 0 && !(info.st_mode & 0022) &&
                info.st_nlink <= 1 && info.st_size == sizeof(value);
    }
    close(directory);
    if (!valid)
        return reject("status missing, withdrawn or untrusted");
    if (value.pid < 2 || !std::isfinite(time) || time <= 0)
        return reject("invalid status identity or time");
    char path[64], buffer[4096];
    std::snprintf(path, sizeof(path), "/proc/%u/stat", value.pid);
    file = open(path, O_RDONLY | O_CLOEXEC);
    if (file < 0)
        return reject("native process missing");
    auto length = read(file, buffer, sizeof(buffer) - 1);
    close(file);
    if (length <= 0)
        return reject("native process stat unreadable");
    buffer[length] = 0;
    char *rest = std::strrchr(buffer, ')');
    if (!rest || rest[1] != ' ')
        return reject("invalid native process stat");
    rest += 2;
    for (unsigned field = 3; field < 22; ++field) {
        rest = std::strchr(rest, ' ');
        if (!rest)
            return reject("truncated native process stat");
        ++rest;
    }
    char *end = nullptr;
    const auto start = std::strtoull(rest, &end, 10);
    // The producer can renew after RunFrame captured its timestamp. Check the
    // lease against a fresh clock read, so a new file cannot look too far ahead.
    timespec stamp{};
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &stamp))
        return reject("native clock unavailable");
    const uint64_t now = uint64_t(stamp.tv_sec) * 1000000000ULL + uint64_t(stamp.tv_nsec);
    if (end == rest || *end != ' ' || start != value.start)
        return reject("native process identity changed");
    if (value.deadline <= now)
        return reject("native health lease expired");
    if (!valid_tracker_capability(value, now, start))
        return reject("invalid native health lease");
    tracker_failure_reason = "ready";
    return true;
}
} // namespace ct
