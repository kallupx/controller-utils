#include "runner.h"
#include "update/download.h"
#include "ui/renderer.h"
#include <charconv>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <sys/stat.h>

int main(int argc, char **argv) {
    if (argc == 2 && std::string(argv[1]) == "--update-download-worker")
        return ct::update::download_worker();
    try {
        const bool registration = argc == 2 && (std::string(argv[1]) == "--register" ||
                                                std::string(argv[1]) == "--unregister" ||
                                                std::string(argv[1]) == "--prepare-registration");
        if (argc > 2 || (argc == 2 && std::string(argv[1]) != "--show" &&
                         std::string(argv[1]) != "--background" && !registration))
            throw std::invalid_argument(
                "usage: controller-utils "
                "[--show|--background|--register|--unregister|--prepare-registration]");
        umask(0077);
        const auto executable = std::filesystem::canonical("/proc/self/exe");
        const auto root = executable.parent_path(), data = root.parent_path() / "app-data";
        ct::SignalQueue signals{SIGUSR1, SIGTERM, SIGINT, SIGHUP};
        std::optional<int> inherited;
        if (const auto value = std::getenv("CONTROLLER_UTILS_LOCK_FD")) {
            const std::string text(value);
            int descriptor = -1;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), descriptor);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
                throw std::invalid_argument("Invalid inherited Controller Utils app lock");
            inherited = descriptor;
            unsetenv("CONTROLLER_UTILS_LOCK_FD");
        }
        ct::ApplicationLock lock(data / "app.lock", inherited);
        if (registration) {
            if (!lock.primary())
                throw std::runtime_error("Close Controller Utils before changing registration");
            const char *home = std::getenv("HOME");
            if (!home || home[0] != '/')
                throw std::runtime_error("Cannot locate the application home directory");
            ct::update::MaintenanceLock maintenance(data);
            if (std::string(argv[1]) == "--prepare-registration") {
                ct::ApplicationRegistration::prepare(root, std::filesystem::canonical(home));
                return 0;
            }
            ct::Runtime runtime;
            ct::ApplicationRegistration app(*static_cast<vr::IVRApplications *>(
                runtime.interface(vr::IVRApplications_Version)));
            if (std::string(argv[1]) == "--register")
                app.install(root, std::filesystem::canonical(home),
                            ct::load_settings(data / "settings.json").autostart);
            else
                app.remove(root, std::filesystem::canonical(home));
            ct::json::Value result(json_object_new_object());
            json_object_object_add(result.get(), "operation", json_object_new_string(argv[1]));
            json_object_object_add(result.get(), "success", json_object_new_boolean(true));
            std::cout << ct::json::encode(result.get()) << '\n';
            return 0;
        }
        if (!lock.primary()) {
            if (argc != 2 || std::string(argv[1]) != "--background")
                if (!lock.show_running(executable))
                    throw std::runtime_error("Controller Utils is already starting");
            return 0;
        }
        lock.publish();
        std::optional<std::string> runtime_problem;
        try {
            ct::check_runtime();
        } catch (const std::exception &error) {
            runtime_problem = "Native tracking is unavailable. Check for a compatible app update.";
            std::cerr << "Controller Utils runtime: " << error.what() << '\n';
        }
        ct::Runtime runtime(true);
        if (std::filesystem::exists(root / "controller-utils.vrmanifest")) {
            auto &api =
                *static_cast<vr::IVRApplications *>(runtime.interface(vr::IVRApplications_Version));
            if (!api.IsApplicationInstalled(ct::ApplicationKey)) {
                const char *home = std::getenv("HOME");
                if (!home || home[0] != '/')
                    throw std::runtime_error("Cannot locate the application home directory");
                ct::ApplicationRegistration(api).install(
                    root, std::filesystem::canonical(home),
                    ct::load_settings(data / "settings.json").autostart);
            }
        }
        ct::ControllerReader reader;
        ct::ui::Presentation presentation;
        ct::NativeApplicationHost host(runtime, reader, lock, presentation, root, data);
        ct::ui::Renderer renderer(runtime, presentation, root);
        std::exception_ptr control_failure, render_failure;
        std::thread control([&] {
            try {
                ct::run_application(host, runtime, reader, signals, data,
                                    argc == 2 && std::string(argv[1]) == "--background",
                                    std::move(runtime_problem));
            } catch (...) {
                control_failure = std::current_exception();
            }
            presentation.close();
        });
        try {
            renderer.run();
        } catch (...) {
            render_failure = std::current_exception();
        }
        presentation.close();
        control.join();
        if (render_failure)
            std::rethrow_exception(render_failure);
        if (control_failure)
            std::rethrow_exception(control_failure);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Controller Utils: " << error.what() << '\n';
        return 1;
    }
}
