#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cassert>
#include <fstream>
#include <sstream>
#include <string>

static std::string directory;
static unsigned renewals, status_reads;
static bool continuous_renewal;
static bool renew_during_path_stat;
static bool remove_status;
static uint64_t clock_ns = 1000000000;
static void publish();

static int capability_clock_gettime(clockid_t clock, timespec *stamp) {
    assert(clock == CLOCK_MONOTONIC_RAW);
    stamp->tv_sec = clock_ns / 1000000000;
    stamp->tv_nsec = clock_ns % 1000000000;
    return 0;
}

static int capability_open(const char *path, int flags) {
    return ::open(std::strcmp(path, "/run/controller-utils-native-tracker") ? path
                                                                            : directory.c_str(),
                  flags);
}
static int capability_fstat(int fd, struct stat *info) {
    char path[64], target[1024];
    std::snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    const auto length = readlink(path, target, sizeof(target) - 1);
    if (length < 0)
        return ::fstat(fd, info);
    target[length] = 0;
    const bool status = std::string(target).find(directory + "/status") == 0;
    if (status) {
        ++status_reads;
        if (continuous_renewal || renewals) {
            if (renewals)
                --renewals;
            publish();
        }
        if (remove_status) {
            unlink((directory + "/status").c_str());
            remove_status = false;
        }
    }
    const int result = ::fstat(fd, info);
    // Only the fixture's ownership is simulated, renewal/unlink and every other field are real.
    if (!result && (status || std::string(target) == directory))
        info->st_uid = 0;
    return result;
}
static int capability_fstatat(int fd, const char *path, struct stat *info, int flags) {
    if (renew_during_path_stat) {
        int snapshot = ::openat(fd, path, O_RDONLY | O_NOFOLLOW);
        assert(snapshot >= 0);
        publish();
        const int result = ::fstat(snapshot, info);
        close(snapshot);
        assert(!result && info->st_nlink == 0);
        info->st_uid = 0;
        return result;
    }
    if (continuous_renewal)
        publish();
    const int result = ::fstatat(fd, path, info, flags);
    if (!result)
        info->st_uid = 0;
    return result;
}
#define open capability_open
#define fstat capability_fstat
#define fstatat capability_fstatat
#define clock_gettime capability_clock_gettime
#include "../driver/tracker_capability.h"
#undef clock_gettime
#undef fstat
#undef fstatat
#undef open

static ct::TrackerCapability value{};
static void publish() {
    const auto pending = directory + "/pending", path = directory + "/status";
    {
        std::ofstream file(pending, std::ios::binary);
        file.write(reinterpret_cast<const char *>(&value), sizeof(value));
        assert(file.good());
    }
    assert(!rename(pending.c_str(), path.c_str()));
}
static uint64_t raw_time() {
    return clock_ns;
}
int main() {
    char path[] = "/tmp/ct-capability-XXXXXX";
    assert(mkdtemp(path));
    directory = path;
    struct Cleanup {
        ~Cleanup() {
            unlink((directory + "/status").c_str());
            unlink((directory + "/pending").c_str());
            rmdir(directory.c_str());
        }
    } cleanup;
    std::ifstream process("/proc/self/stat");
    std::string line, field;
    std::getline(process, line);
    std::istringstream fields(line.substr(line.rfind(')') + 2));
    for (unsigned i = 3; i <= 22; ++i) {
        assert(bool(fields >> field));
    }
    value = {0x314c4f5349544343ULL, 1, 2, raw_time() + 400000000, uint32_t(getpid()), 1,
             std::stoull(field)};
    publish();
    assert(ct::tracker_ready(1.));
    status_reads = 0;
    renewals = 1;
    assert(ct::tracker_ready(1.) && status_reads == 1);
    status_reads = 0;
    continuous_renewal = true;
    for (unsigned i = 0; i < 1000; ++i)
        assert(ct::tracker_ready(1.));
    assert(status_reads == 1000);
    continuous_renewal = false;
    renew_during_path_stat = true;
    assert(ct::tracker_ready(1.));
    renew_during_path_stat = false;
    remove_status = true;
    assert(!ct::tracker_ready(1.));
    clock_ns = value.deadline;
    publish();
    assert(!ct::tracker_ready(1.));
    assert(!std::strcmp(ct::tracker_failure_reason, "native health lease expired"));
    value.deadline = raw_time() + 400000000;
    publish();
    assert(!chmod((directory + "/status").c_str(), 0666));
    assert(!ct::tracker_ready(1.));
    unlink((directory + "/status").c_str());
    assert(!symlink("/dev/zero", (directory + "/status").c_str()));
    assert(!ct::tracker_ready(1.));
    std::puts(
        "isolation capability: continuous renewal, withdrawal, expiry and trust checks passed");
}
