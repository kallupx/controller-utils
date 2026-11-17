#include "app/runner.h"
#include "platform/linux/files.h"
#include "fake_controller.h"
#include <cassert>
#include <dlfcn.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <libunwind.h>

namespace {
namespace j = ct::json;
struct Log {
    unsigned keyboard_requests = 0;
    bool keyboard_closed = false;
    bool keyboard_running = true;
    std::optional<std::string> keyboard_failure;
    std::string keyboard_overlay;
    unsigned system_requests = 0;
    bool system_closed = false;
    uint32_t system_tap = 0;
};
struct MaintenanceFixture {
    unsigned launches = 0;
    bool destroyed = false, unavailable = false;
    std::optional<int> status;
    std::vector<std::string> messages;
    std::vector<std::string> sent;
};
class MaintenanceChild final : public ct::ChildChannel {
    std::shared_ptr<MaintenanceFixture> fixture_;

  public:
    explicit MaintenanceChild(std::shared_ptr<MaintenanceFixture> fixture)
        : fixture_(std::move(fixture)) {}
    ~MaintenanceChild() override {
        fixture_->destroyed = true;
    }
    std::optional<int> exit_status() override {
        return fixture_->status;
    }
    void send(const std::string &message) override {
        fixture_->sent.push_back(message);
    }
    std::vector<j::Value> poll() override {
        std::vector<j::Value> result;
        for (const auto &message : fixture_->messages)
            result.push_back(j::parse(message));
        fixture_->messages.clear();
        return result;
    }
    std::vector<j::Value> close(const std::string &, double) override {
        return {};
    }
};
class SteamUi final : public ct::SteamUiSession {
    std::shared_ptr<Log> log_;
    bool system_;

  public:
    explicit SteamUi(std::shared_ptr<Log> log, bool system = false)
        : log_(std::move(log)), system_(system) {}
    bool running() const override {
        return log_->keyboard_running;
    }
    void send(const ct::FilterRequest &request) override {
        assert(system_ && request.system_only && request.enabled && !request.details);
        log_->system_tap = request.tap;
    }
    std::vector<ct::FilterEvent> poll() override {
        if (system_)
            return {ct::FilterState{true, 3, "TEST1234", ct::monotonic_time()}};
        if (log_->keyboard_failure)
            return {ct::FilterError{*log_->keyboard_failure}};
        if (!log_->keyboard_overlay.empty())
            return {ct::FilterState{false, -1, "keyboard-shortcut", ct::monotonic_time(), "off",
                                    log_->keyboard_overlay}};
        return {};
    }
    void close() override {
        if (system_)
            log_->system_closed = true;
        else
            log_->keyboard_closed = true;
    }
};
ct::ChildFactory children() {
    return [](const std::vector<std::string> &) -> std::unique_ptr<ct::ChildChannel> {
        throw std::logic_error("Unexpected helper process in native host fixture");
    };
}
ct::SteamUiFactory steam_ui(const std::shared_ptr<Log> &log) {
    return [log](const std::string &serial, bool keyboard) {
        if (!keyboard) {
            assert(serial == "TEST1234");
            ++log->system_requests;
            return std::make_unique<SteamUi>(log, true);
        }
        assert(serial == "keyboard-shortcut" && keyboard);
        ++log->keyboard_requests;
        return std::make_unique<SteamUi>(log);
    };
}
template <class Function> void rejects(Function function) {
    bool rejected = false;
    try {
        function();
    } catch (const std::exception &) {
        rejected = true;
    }
    assert(rejected);
}
template <class Function> Function symbol(void *library, const char *name) {
    const auto address = dlsym(library, name);
    assert(address);
    return reinterpret_cast<Function>(address);
}
size_t descriptors() {
    size_t count = 0;
    for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        (void)entry;
        ++count;
    }
    return count;
}
} // namespace
int main() {
    // Initialize libunwind's process-wide probe pipe before descriptor checks.
    void *frames[32];
    if (unw_backtrace(frames, 32) <= 0)
        return 1;
    const auto root =
        std::filesystem::current_path() / "build" / ("native-host-" + std::to_string(getpid()));
    const auto app = root / "app", data = root / "app-data";
    std::filesystem::create_directories(app / "assets");
    std::filesystem::create_directory(data);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::filesystem::remove_all(path);
        }
    } cleanup{root};
    ct::atomic_write(app / "release.json",
                     R"({"version":"0.1.0","sequence":1,"platform":"linuxarm64"})");
    const auto library = std::filesystem::current_path() / "build/fake_openvr.so";
    void *module = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
    assert(module);
    struct Module {
        void *handle;
        ~Module() {
            dlclose(handle);
        }
    } module_owner{module};
    const auto reset = symbol<void (*)()>(module, "ct_test_reset");
    const auto identified = symbol<unsigned (*)()>(module, "ct_test_identified");
    const auto startup_calls = symbol<unsigned (*)()>(module, "ct_test_startup_calls");
    const auto startup_enabled = symbol<unsigned (*)()>(module, "ct_test_startup_enabled");
    const auto shutdowns = symbol<unsigned (*)()>(module, "ct_test_shutdown");
    const auto poses = symbol<unsigned (*)()>(module, "ct_test_pose_calls");
    const auto keyboard_shown = symbol<unsigned (*)()>(module, "ct_test_keyboard_shown");
    const auto keyboard_positioned = symbol<unsigned (*)()>(module, "ct_test_keyboard_positioned");
    const auto before = descriptors();
    {
        reset();
        auto log = std::make_shared<Log>();
        auto hid = std::make_shared<ct::test::FakeState>();
        ct::SignalQueue signals{SIGUSR1, SIGTERM, SIGINT, SIGHUP};
        ct::ApplicationLock lock(data / "app.lock");
        lock.publish();
        ct::Runtime runtime(false, library);
        ct::ControllerReader reader([&] {
            return std::make_shared<ct::ControllerDevice>(
                std::make_unique<ct::test::FakeTransport>(hid));
        });
        reader.poll(true);
        ct::ui::Presentation presentation;
        auto camera = std::make_shared<MaintenanceFixture>();
        const auto child_factory =
            [&](const std::vector<std::string> &command) -> std::unique_ptr<ct::ChildChannel> {
            if (command.back() != "--camera-switch-worker")
                return children()(command);
            assert(command == (std::vector<std::string>{(app / "controller-utils").string(),
                                                        "--camera-switch-worker"}));
            ++camera->launches;
            camera->destroyed = false;
            return std::make_unique<MaintenanceChild>(camera);
        };
        ct::NativeApplicationHost host(runtime, reader, lock, presentation, app, data,
                                       child_factory, steam_ui(log));
        assert(!host.system_input(false) && log->system_requests == 0);
        assert(host.system_input(true) && log->system_requests == 1);
        const auto feature_reads = hid->feature_reads;
        const auto commands = hid->commands.size();
        for (unsigned tick = 0; tick < 1000; ++tick)
            assert(host.system_input(true));
        assert(hid->feature_reads == feature_reads && hid->commands.size() == commands);
        host.shortcut("dashboard");
        assert(log->system_tap == 1);
        host.shortcut("camera_switch");
        host.shortcut("camera_switch");
        assert(camera->launches == 1 && !host.camera_switch_error(true));
        camera->status = 256;
        camera->messages = {R"({"error":"Camera disconnected."})"};
        assert(host.camera_switch_error(true) == "Camera disconnected.");
        assert(camera->destroyed && !host.camera_switch_error(true));
        camera->status.reset();
        host.shortcut("camera_switch");
        assert(camera->launches == 2 && !camera->destroyed);
        assert(!host.camera_switch_error(false) && camera->destroyed);
        assert(!host.system_input(false) && log->system_closed);
        host.close();
    }
    for (unsigned cycle = 0; cycle < 10; ++cycle) {
        reset();
        ct::save_settings(data / "settings.json", {});
        auto log = std::make_shared<Log>();
        unsigned device_opens = 0;
        {
            ct::SignalQueue signals{SIGUSR1, SIGTERM, SIGINT, SIGHUP};
            ct::ApplicationLock lock(data / "app.lock");
            lock.publish();
            ct::Runtime runtime(false, library);
            ct::ControllerReader reader([&]() -> std::shared_ptr<ct::ControllerDevice> {
                ++device_opens;
                throw std::runtime_error("No test controller");
            });
            ct::ui::Presentation presentation;
            ct::NativeApplicationHost host(runtime, reader, lock, presentation, app, data,
                                           children(), steam_ui(log));
            for (const auto name : {"autostart", "pinned", "toggle"})
                presentation.emit(ct::ui::Action{name});
            bool status = false, updates = false, show = false;
            std::thread ui([&] {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while (presentation.running() && std::chrono::steady_clock::now() < deadline) {
                    for (const auto &message : presentation.commands()) {
                        status |= std::holds_alternative<ct::ui::Status>(message);
                        updates |= std::holds_alternative<ct::ui::Updates>(message);
                        if (const auto op = std::get_if<ct::ui::Operation>(&message))
                            show |= *op == ct::ui::Operation::ShowSettings;
                    }
                    if (status && updates && show) {
                        presentation.emit(ct::ui::Quit{});
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                presentation.close();
            });
            assert(!ct::run_application(host, runtime, reader, signals, data, false));
            ui.join();
            assert(!device_opens && !presentation.running());
            assert(identified() == 1 && startup_calls() == 2 && !startup_enabled());
            assert(poses() >= 1);
            const auto settings = ct::load_settings(data / "settings.json");
            assert(!settings.autostart && !settings.pinned && !settings.tracking_enabled);
            assert(status && updates && show);
        }
        assert(shutdowns() == 1 && descriptors() == before);
    }
    {
        reset();
        auto log = std::make_shared<Log>();
        ct::SignalQueue signals{SIGUSR1};
        ct::ApplicationLock lock(data / "app.lock");
        ct::Runtime runtime(false, library);
        ct::ControllerReader reader;
        ct::ui::Presentation presentation;
        ct::NativeApplicationHost host(runtime, reader, lock, presentation, app, data, children(),
                                       steam_ui(log));
        host.request_keyboard();
        host.request_keyboard();
        assert(log->keyboard_requests == 1 && !host.keyboard_error());
        log->keyboard_running = false;
        log->keyboard_failure = "fixture keyboard failure";
        assert(host.keyboard_error() == "fixture keyboard failure");
        assert(log->keyboard_closed && !host.keyboard_error());
        log->keyboard_running = true;
        host.request_keyboard();
        assert(log->keyboard_requests == 2);
        host.close_keyboard();
        assert(log->keyboard_closed);
        assert(!j::boolean(j::field(host.updates(false).get(), "configured")));
        host.confirm_update();
        host.close();
        assert(!presentation.running());
    }
    {
        reset();
        auto log = std::make_shared<Log>();
        log->keyboard_running = false;
        log->keyboard_overlay = "gamescope.gamescope-0.window.62";
        ct::ApplicationLock lock(data / "app.lock");
        ct::Runtime runtime(true, library);
        ct::ControllerReader reader;
        ct::ui::Presentation presentation;
        ct::NativeApplicationHost host(runtime, reader, lock, presentation, app, data, children(),
                                       steam_ui(log));
        setenv("CT_TEST_OPENVR_KEYBOARD_TARGET", log->keyboard_overlay.c_str(), 1);
        host.request_keyboard();
        assert(!host.keyboard_error());
        assert(keyboard_shown() == 1 && keyboard_positioned() == 1);
        assert(!host.keyboard_error() && keyboard_shown() == 1);
        setenv("CT_TEST_OPENVR_KEYBOARD_FAILURE", "1", 1);
        host.request_keyboard();
        const auto failure = host.keyboard_error();
        assert(failure && failure->find("Could not open the window keyboard") != std::string::npos);
        assert(keyboard_shown() == 2 && keyboard_positioned() == 1);
        unsetenv("CT_TEST_OPENVR_KEYBOARD_FAILURE");
        unsetenv("CT_TEST_OPENVR_KEYBOARD_TARGET");
        host.request_keyboard();
        assert(host.keyboard_error() == "The focused window is no longer available");
        assert(keyboard_shown() == 2);
        host.close();
    }
    {
        reset();
        auto maintenance = std::make_shared<MaintenanceFixture>();
        ct::ApplicationLock lock(data / "app.lock");
        ct::Runtime runtime(false, library);
        ct::ControllerReader reader;
        ct::ui::Presentation presentation;
        ct::ChildFactory factory = [maintenance](const std::vector<std::string> &args) {
            ++maintenance->launches;
            if (maintenance->unavailable)
                throw std::runtime_error("maintenance unavailable");
            assert(args.size() == 4 && args[0] == "/usr/bin/sudo" && args[1] == "-n" &&
                   args[2] == "/var/lib/controller-utils/controller-utils-updater" &&
                   (args[3] == "--repair" || args[3] == "--repair-status"));
            maintenance->messages.clear();
            maintenance->status.reset();
            return std::make_unique<MaintenanceChild>(maintenance);
        };
        ct::NativeApplicationHost host(runtime, reader, lock, presentation, app, data, factory, {},
                                       [] {});
        assert(maintenance->launches == 1);
        assert(host.permission_repair_status() == "Restoring helper permissions.");
        host.request_permission_repair();
        host.request_permission_inspection();
        assert(maintenance->launches == 1 && maintenance->sent.empty());
        assert(host.events().empty() && host.running());
        rejects([&] { host.confirm_update(); });
        maintenance->status = 0;
        assert(host.permission_repair_status() == "Helper permissions are ready.");
        const auto begin = [&](bool inspection) {
            if (inspection)
                host.request_permission_inspection();
            else
                host.request_permission_repair();
            assert(host.permission_repair_status() ==
                   (inspection ? "Checking helper permissions." : "Restoring helper permissions."));
        };
        begin(true);
        maintenance->messages = {R"({"op":"repair-status","status":"missing"})"};
        maintenance->status = 0;
        assert(host.permission_repair_status() ==
               "Helper permissions are missing. Repair is available.");
        begin(true);
        maintenance->messages = {R"({"op":"repair-status","status":"intact"})",
                                 R"({"op":"repair-status","status":"intact"})"};
        assert(host.permission_repair_status() == "Permission inspection failed.");
        begin(true);
        maintenance->messages = {R"({"op":"repair-status","status":"unexpected"})"};
        assert(host.permission_repair_status() == "Permission inspection failed.");
        begin(true);
        maintenance->messages = {"{"};
        assert(host.permission_repair_status() == "Permission inspection failed.");
        begin(true);
        maintenance->status = 0;
        assert(host.permission_repair_status() == "Permission inspection failed.");
        for (int exit : {1, 126, 127}) {
            begin(false);
            maintenance->status = exit << 8;
            assert(host.permission_repair_status() ==
                   "Permission maintenance failed. Review the installation.");
        }
        maintenance->unavailable = true;
        host.request_permission_repair();
        assert(host.permission_repair_status() == "Permission maintenance is unavailable.");
        maintenance->unavailable = false;
        begin(false);
        maintenance->messages = {R"({"op":"repair-ready"})"};
        assert(host.permission_repair_status() == "Permission inspection failed.");
        assert(maintenance->sent.empty());
        begin(false);
        maintenance->status = 0;
        assert(host.permission_repair_status() == "Helper permissions are ready.");
        maintenance->destroyed = false;
        const auto before_close = maintenance->launches;
        host.request_permission_repair();
        host.close();
        assert(maintenance->destroyed && maintenance->sent.empty() &&
               maintenance->launches == before_close + 1);
    }
    for (const auto failure :
         {"CT_TEST_OPENVR_IDENTIFY_FAILURE", "CT_TEST_OPENVR_STARTUP_FAILURE"}) {
        reset();
        auto log = std::make_shared<Log>();
        ct::SignalQueue signals{SIGUSR1, SIGTERM, SIGINT, SIGHUP};
        ct::ApplicationLock lock(data / "app.lock");
        ct::Runtime runtime(false, library);
        ct::ControllerReader reader;
        ct::ui::Presentation presentation;
        assert(!setenv(failure, "1", 1));
        rejects([&] {
            ct::NativeApplicationHost host(runtime, reader, lock, presentation, app, data,
                                           children(), steam_ui(log));
            ct::run_application(host, runtime, reader, signals, data, false);
        });
        unsetenv(failure);
        if (std::string(failure) == "CT_TEST_OPENVR_IDENTIFY_FAILURE")
            assert(log->keyboard_requests == 0);
        else
            assert(!presentation.running());
    }
    assert(descriptors() == before);
    std::cout << "native host: registration, real coordinator IO, settings, updates, keyboard "
                 "ownership, partial startup and ten closed lifecycles passed\n";
}
