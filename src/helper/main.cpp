#include "backend.h"
#include "discovery.h"
#include "input_gates.h"
#include "native_paths.h"
#include "supervisor.h"
#include "platform/linux/protected_directory.h"
#include "platform/linux/files.h"
#include "steamvr/compatibility.h"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/utsname.h>

namespace {
void check(int value, const char *operation) {
    if (value < 0)
        throw std::system_error(errno, std::generic_category(), operation);
}
void preflight(int argc, char **argv) {
    if (argc != 2 || std::string(argv[1]) != "--private" || geteuid() != 0 || !getenv("SUDO_UID") ||
        std::strcmp(getenv("SUDO_UID"), "1000"))
        throw std::runtime_error("Use the installed Controller Utils compatibility command.");
    utsname version{};
    check(uname(&version), "read native compatibility version");
    if (std::strcmp(version.machine, "aarch64") || sysconf(_SC_PAGESIZE) != 4096)
        throw std::runtime_error("This Frame version needs a compatibility update.");
    for (const int stream : {STDIN_FILENO, STDOUT_FILENO}) {
        struct stat info{};
        check(fstat(stream, &info), "verify native application channel");
        if (!S_ISFIFO(info.st_mode) || info.st_uid != 1000)
            throw std::runtime_error("Use the installed Controller Utils application.");
    }
    ct::ProtectedDirectory directory("/var/lib/controller-utils");
    const auto file = directory.file("controller-utils-helper", 20 * 1024 * 1024);
    struct stat pinned{}, running{};
    check(fstat(file.get(), &pinned), "verify installed native helper");
    check(stat("/proc/self/exe", &running), "verify running native helper");
    if (pinned.st_dev != running.st_dev || pinned.st_ino != running.st_ino)
        throw std::runtime_error("Run the installed native compatibility helper.");
}
ct::FileDescriptor lock(const ct::ProtectedDirectory &run) {
    ct::FileDescriptor file(
        openat(run.descriptor(), std::filesystem::path(ct::helper::TrackingLock).filename().c_str(),
               O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600));
    if (!file)
        check(-1, "open native tracking lock");
    struct stat info{};
    check(fstat(file.get(), &info), "verify native tracking lock");
    if (!S_ISREG(info.st_mode) || info.st_uid != 0 || (info.st_mode & 0022) || info.st_nlink != 1)
        throw std::runtime_error("Unprotected native tracking lock.");
    check(flock(file.get(), LOCK_EX | LOCK_NB), "lock native tracking session");
    return file;
}
ct::ProtectedDirectory status_directory(const ct::ProtectedDirectory &run) {
    const auto name = std::filesystem::path(ct::helper::TrackerStatusDirectory).filename().string();
    if (mkdirat(run.descriptor(), name.c_str(), 0755) && errno != EEXIST)
        check(-1, "create native readiness directory");
    ct::ProtectedDirectory result(ct::helper::TrackerStatusDirectory);
    check(fchmod(result.descriptor(), 0755), "native readiness directory permissions");
    return result;
}
} // namespace
int main(int argc, char **argv) {
    try {
        preflight(argc, argv);
        umask(0077);
        ct::ProtectedDirectory run("/run");
        auto owned_lock = lock(run);
        const auto &binary = ct::steamvr::compatibility::XrService;
        const auto pid = ct::helper::find_process(binary);
        const auto fingerprint = ct::sha256("/proc/" + std::to_string(pid) + "/exe");
        if (!ct::steamvr::compatibility::supported_xr_service(fingerprint))
            throw std::runtime_error("XRService changed. Native protocol must be reverified");
        ct::PinnedProcess process(pid, {binary.path, fingerprint, 1000, 0x200000}, true);
        ct::helper::NativePackage package(fingerprint);
        ct::helper::CapabilityPublisher capability(status_directory(run), process.pid(),
                                                   process.started());
        ct::helper::XrProcess host(process, &package, &capability);
        ct::helper::XrBackend backend(host);
        ct::helper::InputGates actions(backend);
        ct::helper::RecoveryGuard guard(actions);
        ct::helper::Supervisor supervisor(backend, guard);
        supervisor.run();
        return 0;
    } catch (const std::exception &error) {
        struct stat output{};
        if (!fstat(STDOUT_FILENO, &output) && S_ISFIFO(output.st_mode) && output.st_uid == 1000) {
            try {
                ct::helper::RecoveryOutput{}.error(error.what());
            } catch (...) {
            }
        }
        return 1;
    }
}
