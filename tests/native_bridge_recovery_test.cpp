#include <cstdlib>
#include <pwd.h>
#include <unistd.h>
#include <sys/wait.h>

extern "C" uid_t test_getuid() {
    return 1000;
}
extern "C" passwd *test_getpwuid(uid_t) {
    static passwd user{};
    user.pw_name = const_cast<char *>("steamos");
    user.pw_dir = std::getenv("CT_RECOVERY_TEST_HOME");
    return &user;
}
#define getuid test_getuid
#define getpwuid test_getpwuid
#include "../driver/native_bridge.cpp"
#undef getuid
#undef getpwuid
#include <cassert>
#include <iostream>

class Context final : public vr::IVRDriverContext {
  public:
    unsigned calls = 0;
    void *GetGenericInterface(const char *, vr::EVRInitError *error) override {
        ++calls;
        *error = vr::VRInitError_Init_InterfaceNotFound;
        return nullptr;
    }
    vr::DriverHandle_t GetDriverHandle() override {
        return 1;
    }
};
int main() {
    const auto home =
        std::filesystem::current_path() / "build" / ("bridge-recovery-" + std::to_string(getpid()));
    std::filesystem::create_directories(home);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::filesystem::remove_all(path);
        }
    } cleanup{home};
    assert(!setenv("CT_RECOVERY_TEST_HOME", home.c_str(), 1));
    auto state = ct::startup_recovery(home / ".local/share/controller-utils/app-data");
    for (int i = 0; i < 2; ++i) {
        const auto child = fork();
        assert(child >= 0);
        if (!child)
            _exit(state.begin() ? 0 : 1);
        int status = 0;
        assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    }
    Context context;
    NativeBridge bridge;
    assert(bridge.Init(&context) == vr::VRInitError_None);
    assert(!context.calls && state.disabled());
    bridge.Cleanup();
    assert(state.disabled());
    state.retry();
    assert(state.disabled());
    NativeBridge retry;
    assert(retry.Init(&context) == vr::VRInitError_Init_InterfaceNotFound);
    assert(context.calls && !state.disabled());
    assert(state.begin());
    state.clean();
    ct::atomic_write(home / ".local/share/controller-utils/app-data/startup-recovery", "damaged");
    Context damaged_context;
    NativeBridge damaged;
    assert(damaged.Init(&damaged_context) == vr::VRInitError_None);
    assert(!damaged_context.calls && state.failed());
    damaged.Cleanup();
    assert(state.failed());
    std::cout
        << "native driver recovery: startup failures block hooks and retry reaches interfaces\n";
}
