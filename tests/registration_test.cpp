#include "app/registration.h"
#include "platform/linux/files.h"
#include "platform/linux/records.h"
#include "platform/json.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "fake_openvr_interfaces.h"
#pragma GCC diagnostic pop
#include <cassert>
#include <fstream>
#include <iostream>
#include <sys/wait.h>

namespace {
namespace fs = std::filesystem;
namespace j = ct::json;
enum class Failure {
    None,
    Add,
    DeliveredAdd,
    RetainedAdd,
    Startup,
    DeliveredRemove,
    RetainedRemove
};
struct Applications : ct_test::Applications {
    bool installed = false, startup = false;
    Failure failure = Failure::None;
    unsigned calls = 0;
    fs::path loaded;
    bool fail(Failure value) {
        if (failure != value)
            return false;
        failure = Failure::None;
        return true;
    }
    vr::EVRApplicationError AddApplicationManifest(const char *path, bool temporary) override {
        ++calls;
        assert(!temporary && fs::is_regular_file(path));
        if (fail(Failure::Add))
            return vr::VRApplicationError_InvalidManifest;
        if (fail(Failure::RetainedAdd))
            return vr::VRApplicationError_None;
        installed = true;
        loaded = path;
        return fail(Failure::DeliveredAdd) ? vr::VRApplicationError_InvalidManifest
                                           : vr::VRApplicationError_None;
    }
    vr::EVRApplicationError RemoveApplicationManifest(const char *path) override {
        ++calls;
        assert(fs::is_regular_file(path));
        if (fail(Failure::RetainedRemove))
            return vr::VRApplicationError_None;
        installed = false;
        startup = false;
        return fail(Failure::DeliveredRemove) ? vr::VRApplicationError_InvalidManifest
                                              : vr::VRApplicationError_None;
    }
    bool IsApplicationInstalled(const char *key) override {
        assert(key == std::string(ct::ApplicationKey));
        return installed;
    }
    vr::EVRApplicationError SetApplicationAutoLaunch(const char *key, bool enabled) override {
        assert(key == std::string(ct::ApplicationKey));
        ++calls;
        if (fail(Failure::Startup))
            return vr::VRApplicationError_UnknownApplication;
        startup = enabled;
        return vr::VRApplicationError_None;
    }
    bool GetApplicationAutoLaunch(const char *key) override {
        assert(key == std::string(ct::ApplicationKey));
        return startup;
    }
};
template <class Function> void rejects(Function function) {
    bool rejected = false;
    try {
        function();
    } catch (const std::exception &) {
        rejected = true;
    }
    assert(rejected);
}
struct Fixture {
    fs::path base, app, home, desktop, manifest, launcher;
    Applications api;
    ct::ApplicationRegistration registration{api};
    explicit Fixture(const fs::path &path)
        : base(path), app(path / "app"), home(path / "home"),
          desktop(home / ".local/share/applications/org.controllerutils.frame.desktop"),
          manifest(app / "controller-utils.vrmanifest"), launcher(path / "controller-utils") {
        assert(fs::create_directory(base));
        fs::create_directory(home);
        fs::create_directories(app / "assets");
        fs::copy_file("/proc/self/exe", launcher);
        assert(chmod(launcher.c_str(), 0755) == 0);
        ct::atomic_write(app / "assets/controller-icon.png", "icon", 0644);
    }
    void install(bool startup = true) {
        registration.install(app, home, startup);
        assert(api.installed && api.startup == startup && api.loaded == manifest);
    }
    void validate() {
        const auto child = fork();
        assert(child >= 0);
        if (!child) {
            execlp("desktop-file-validate", "desktop-file-validate", desktop.c_str(), nullptr);
            _exit(127);
        }
        int status = 0;
        assert(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
               WEXITSTATUS(status) == 0);
    }
    ~Fixture() {
        fs::remove_all(base);
    }
};
mode_t mode(const fs::path &path) {
    struct stat value{};
    assert(stat(path.c_str(), &value) == 0);
    return value.st_mode & 0777;
}
void no_pending(const fs::path &path) {
    for (const auto &entry : fs::recursive_directory_iterator(path))
        assert(entry.path().filename().string().find(".pending-") != 0);
}
} // namespace
int main() {
    const auto root = fs::canonical("build") / ("registration-" + std::to_string(getpid()));
    {
        Fixture f(root);
        f.install();
        f.validate();
        const auto value = j::parse(ct::read_file(f.manifest, 65536));
        assert(j::string(j::field(value.get(), "source")) == "controllerutils");
        const auto app = j::item(j::field(value.get(), "applications"), 0);
        for (const auto key : {"binary_path_linux", "binary_path_linux_arm"})
            assert(j::string(j::field(app, key)) == f.launcher.string());
        assert(j::string(j::field(app, "app_key")) == ct::ApplicationKey);
        assert(j::string(j::field(app, "arguments")) == "--background");
        assert(j::string(j::field(app, "working_directory")) == f.base.string());
        assert(j::boolean(j::field(app, "is_dashboard_overlay")));
        assert(j::string(j::field(app, "image_path")) ==
               (f.app / "assets/controller-icon.png").string());
        assert(mode(f.manifest) == 0644 && mode(f.desktop) == 0644);
        f.install(false);
        f.registration.remove(f.app, f.home);
        assert(!f.api.installed && !f.api.startup && !fs::exists(f.desktop));
        assert(fs::is_regular_file(f.manifest));
        f.registration.remove(f.app, f.home);
        f.install();
        no_pending(root);
    }
    {
        Fixture f(root.string() + " space%$\\\"`end");
        f.install();
        f.validate();
        const auto bytes = ct::read_file(f.desktop, 65536);
        assert(bytes.find("%%") != std::string::npos);
        assert(bytes.find("\\\\$") != std::string::npos);
    }
    {
        Fixture f(root);
        ct::ApplicationRegistration::prepare(f.app, f.home);
        assert(f.api.calls == 0 && !f.api.installed);
        assert(fs::is_regular_file(f.manifest) && fs::is_regular_file(f.desktop));
        const auto manifest = ct::read_file(f.manifest, 65536);
        ct::ApplicationRegistration::prepare(f.app, f.home);
        assert(ct::read_file(f.manifest, 65536) == manifest);
        f.install();
        f.validate();
    }
    for (const bool existing : {false, true}) {
        for (const auto failure :
             {Failure::Add, Failure::DeliveredAdd, Failure::RetainedAdd, Failure::Startup}) {
            if (existing && failure == Failure::RetainedAdd)
                continue;
            Fixture f(root);
            std::string manifest, desktop;
            if (existing) {
                f.install();
                assert(chmod(f.manifest.c_str(), 0640) == 0);
                assert(chmod(f.desktop.c_str(), 0600) == 0);
                manifest = ct::read_file(f.manifest, 65536);
                desktop = ct::read_file(f.desktop, 65536);
            }
            f.api.failure = failure;
            rejects([&] { f.registration.install(f.app, f.home, !existing); });
            assert(f.api.installed == existing && f.api.startup == existing);
            if (existing) {
                assert(ct::read_file(f.manifest, 65536) == manifest && mode(f.manifest) == 0640);
                assert(ct::read_file(f.desktop, 65536) == desktop && mode(f.desktop) == 0600);
            } else {
                assert(!fs::exists(f.manifest) && !fs::exists(f.desktop));
            }
            no_pending(root);
        }
    }
    for (const auto failure :
         {Failure::Startup, Failure::DeliveredRemove, Failure::RetainedRemove}) {
        Fixture f(root);
        f.install();
        const auto desktop = ct::read_file(f.desktop, 65536);
        f.api.failure = failure;
        rejects([&] { f.registration.remove(f.app, f.home); });
        assert(f.api.installed && f.api.startup && ct::read_file(f.desktop, 65536) == desktop);
    }
    for (unsigned invalid = 0; invalid < 8; ++invalid) {
        Fixture f(root);
        switch (invalid) {
        case 0:
            f.api.installed = true;
            break;
        case 1:
            ct::atomic_write(f.manifest, R"({"source":"other"})", 0644);
            break;
        case 2:
            fs::create_symlink(f.app / "assets/controller-icon.png", f.manifest);
            break;
        case 3:
            fs::create_hard_link(f.app / "assets/controller-icon.png", f.manifest);
            break;
        case 4:
            assert(chmod(f.home.c_str(), 0777) == 0);
            break;
        case 5:
            assert(chmod(f.launcher.c_str(), 0644) == 0);
            break;
        case 6:
            ct::atomic_write(f.launcher, "#!/bin/sh\nexit 0\n", 0755);
            break;
        case 7:
            fs::remove(f.app / "assets/controller-icon.png");
            break;
        }
        rejects([&] { f.registration.install(f.app, f.home, true); });
        assert(f.api.calls == 0);
        assert(chmod(f.home.c_str(), 0700) == 0);
    }
    std::cout << "Native registration and rollback checks passed\n";
}
