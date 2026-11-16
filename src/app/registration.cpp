#include "registration.h"
#include "platform/json.h"
#include "platform/linux/files.h"
#include "platform/linux/records.h"
#include <array>
#include <cerrno>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>

namespace ct {
namespace {
constexpr char ManifestName[] = "controller-utils.vrmanifest";
constexpr char DesktopName[] = "org.controllerutils.frame.desktop";
struct Saved {
    std::optional<std::string> bytes;
    mode_t mode = 0644;
};
Saved save(const ProtectedDirectory &directory, const char *name) {
    Saved result{read_record(directory, name)};
    if (result.bytes) {
        const auto file = directory.file(name, 65536);
        struct stat info{};
        if (fstat(file.get(), &info))
            throw std::system_error(errno, std::generic_category(), "inspect registration file");
        if (info.st_mode & 07000)
            throw std::runtime_error("Unsafe registration file permissions");
        result.mode = info.st_mode & 0777;
    }
    return result;
}
void restore(const ProtectedDirectory &directory, const char *name, const Saved &saved) {
    if (saved.bytes)
        write_record(directory, name, *saved.bytes, saved.mode);
    else
        erase_record(directory, name);
}
void path_text(const std::filesystem::path &path) {
    if (!path.is_absolute() || path.lexically_normal() != path || path.string().size() > 4096)
        throw std::invalid_argument("Invalid application registration path");
    for (unsigned char byte : path.string())
        if (byte < 32 || byte == 127)
            throw std::invalid_argument("Invalid application registration path");
}
std::filesystem::path desktop_directory(const std::filesystem::path &home) {
    path_text(home);
    auto path = home;
    for (const auto component : {".local", "share", "applications"}) {
        ProtectedDirectory directory(path, getuid());
        if (mkdirat(directory.descriptor(), component, 0700) && errno != EEXIST)
            throw std::system_error(errno, std::generic_category(),
                                    "create application registration directory");
        directory.verify();
        path /= component;
    }
    return path;
}
std::string desktop_value(const std::string &value) {
    std::string result;
    for (char byte : value) {
        if (byte == '\\')
            result += '\\';
        result += byte;
    }
    return result;
}
std::string desktop_argument(const std::filesystem::path &path) {
    std::string result = "\"";
    for (char byte : path.string()) {
        if (byte == '%') {
            result += "%%";
            continue;
        }
        if (byte == '\\' || byte == '"' || byte == '`' || byte == '$')
            result += '\\';
        result += byte;
    }
    return desktop_value(result + '"');
}
void text(json_object *object, const char *name, const std::string &value) {
    json_object_object_add(object, name, json_object_new_string(value.c_str()));
}
std::string manifest(const std::filesystem::path &root) {
    json::Value value(json_object_new_object());
    text(value.get(), "source", "controllerutils");
    auto entries = json_object_new_array();
    auto app = json_object_new_object();
    json_object_array_add(entries, app);
    json_object_object_add(value.get(), "applications", entries);
    text(app, "app_key", ApplicationKey);
    text(app, "launch_type", "binary");
    json_object_object_add(app, "is_dashboard_overlay", json_object_new_boolean(true));
    for (const auto key : {"binary_path_linux", "binary_path_linux_arm"})
        text(app, key, (root.parent_path() / "controller-utils").string());
    text(app, "arguments", "--background");
    text(app, "working_directory", root.parent_path().string());
    text(app, "image_path", (root / "assets/controller-icon.png").string());
    auto strings = json_object_new_object(), english = json_object_new_object();
    json_object_object_add(app, "strings", strings);
    json_object_object_add(strings, "en_us", english);
    text(english, "name", "Controller Utils");
    text(english, "description", "Steam Controller tracking and controls");
    return json::encode(value.get()) + '\n';
}
void owned_manifest(const Saved &saved, const std::filesystem::path &root) {
    if (!saved.bytes)
        return;
    auto value = json::parse(*saved.bytes);
    const auto entries = json::field(value.get(), "applications");
    if (json::string(json::field(value.get(), "source")) != "controllerutils" ||
        json::array_size(entries) != 1 ||
        json::string(json::field(json::item(entries, 0), "app_key")) != ApplicationKey ||
        json::string(json::field(json::item(entries, 0), "binary_path_linux")) !=
            (root.parent_path() / "controller-utils").string())
        throw std::runtime_error("Application registration belongs to another installation");
}
struct Files {
    ProtectedDirectory application, desktop;
    Saved previous_manifest, previous_desktop;
    Files(const std::filesystem::path &root, const std::filesystem::path &home)
        : application(root, getuid()), desktop(desktop_directory(home), getuid()),
          previous_manifest(save(application, ManifestName)),
          previous_desktop(save(desktop, DesktopName)) {
        owned_manifest(previous_manifest, root);
    }
};
void prepare_files(Files &files, const std::filesystem::path &root) {
    ProtectedDirectory parent(root.parent_path(), getuid()), assets(root / "assets", getuid());
    const auto launcher = parent.file("controller-utils", MaxRecordSize);
    struct stat info{};
    if (fstat(launcher.get(), &info) || !(info.st_mode & 0111))
        throw std::runtime_error("Controller Utils launcher is missing or not executable");
    char magic[4]{};
    if (pread(launcher.get(), magic, sizeof(magic), 0) != sizeof(magic) ||
        std::memcmp(magic, "\177ELF", 4))
        throw std::runtime_error("Controller Utils requires a native launcher");
    assets.file("controller-icon.png", 16 * 1024 * 1024);
    write_record(files.application, ManifestName, manifest(root), 0644);
    const auto entry =
        "[Desktop Entry]\nType=Application\nName=Controller Utils\n"
        "Comment=Steam Controller tracking and controls\nExec=" +
        desktop_argument(root.parent_path() / "controller-utils") +
        " --show\nIcon=" + desktop_value((root / "assets/controller-icon.png").string()) +
        "\nTerminal=false\nCategories=Utility;\nStartupNotify=false\n";
    write_record(files.desktop, DesktopName, entry, 0644);
}
template <class Function> void attempt(std::string &errors, Function function) {
    try {
        function();
    } catch (const std::exception &error) {
        if (!errors.empty())
            errors += ". ";
        errors += error.what();
    }
}
} // namespace
void ApplicationRegistration::prepare(const std::filesystem::path &root,
                                      const std::filesystem::path &home) {
    path_text(root);
    Files files(root, home);
    try {
        prepare_files(files, root);
    } catch (const std::exception &error) {
        const std::string failure = error.what();
        std::string errors;
        attempt(errors, [&] { restore(files.application, ManifestName, files.previous_manifest); });
        attempt(errors, [&] { restore(files.desktop, DesktopName, files.previous_desktop); });
        throw std::runtime_error(failure +
                                 (errors.empty() ? "" : ". Registration restoration: " + errors));
    }
}
void ApplicationRegistration::identify() {
    const auto result = api_.IdentifyApplication(uint32_t(getpid()), ApplicationKey);
    if (result)
        throw std::runtime_error("cannot identify Controller Utils: " + std::to_string(result));
}
void ApplicationRegistration::autostart(bool enabled) {
    const auto result = api_.SetApplicationAutoLaunch(ApplicationKey, enabled);
    if (result || api_.GetApplicationAutoLaunch(ApplicationKey) != enabled)
        throw std::runtime_error("could not update startup setting: " + std::to_string(result));
}
void ApplicationRegistration::install(const std::filesystem::path &root,
                                      const std::filesystem::path &home, bool start) {
    path_text(root);
    Files files(root, home);
    const bool was_installed = api_.IsApplicationInstalled(ApplicationKey);
    if (was_installed && !files.previous_manifest.bytes)
        throw std::runtime_error("Existing registration belongs to another installation");
    const bool startup = was_installed && api_.GetApplicationAutoLaunch(ApplicationKey);
    bool sdk_touched = false;
    try {
        prepare_files(files, root);
        sdk_touched = true;
        const auto result = api_.AddApplicationManifest((root / ManifestName).c_str(), false);
        if (result || !api_.IsApplicationInstalled(ApplicationKey))
            throw std::runtime_error("Application registration was not retained: " +
                                     std::to_string(result));
        autostart(start);
    } catch (const std::exception &error) {
        const std::string failure = error.what();
        std::string errors;
        if (sdk_touched) {
            attempt(errors, [&] {
                api_.RemoveApplicationManifest((root / ManifestName).c_str());
                if (api_.IsApplicationInstalled(ApplicationKey))
                    throw std::runtime_error("Could not undo application registration");
            });
        }
        attempt(errors, [&] { restore(files.application, ManifestName, files.previous_manifest); });
        attempt(errors, [&] { restore(files.desktop, DesktopName, files.previous_desktop); });
        if (sdk_touched && was_installed) {
            attempt(errors, [&] {
                if (api_.AddApplicationManifest((root / ManifestName).c_str(), false) ||
                    !api_.IsApplicationInstalled(ApplicationKey))
                    throw std::runtime_error("Could not restore application registration");
            });
            attempt(errors, [&] { autostart(startup); });
        }
        throw std::runtime_error(failure +
                                 (errors.empty() ? "" : ". Registration restoration: " + errors));
    }
}
void ApplicationRegistration::remove(const std::filesystem::path &root,
                                     const std::filesystem::path &home) {
    path_text(root);
    Files files(root, home);
    const bool installed = api_.IsApplicationInstalled(ApplicationKey);
    if (installed && !files.previous_manifest.bytes)
        throw std::runtime_error("Existing registration belongs to another installation");
    const bool startup = installed && api_.GetApplicationAutoLaunch(ApplicationKey);
    try {
        if (installed) {
            autostart(false);
            const auto result = api_.RemoveApplicationManifest((root / ManifestName).c_str());
            if (result || api_.IsApplicationInstalled(ApplicationKey))
                throw std::runtime_error("Could not remove application registration: " +
                                         std::to_string(result));
        }
        erase_record(files.desktop, DesktopName);
    } catch (const std::exception &error) {
        const std::string failure = error.what();
        std::string errors;
        attempt(errors, [&] { restore(files.desktop, DesktopName, files.previous_desktop); });
        if (installed) {
            attempt(errors, [&] {
                if (api_.AddApplicationManifest((root / ManifestName).c_str(), false) ||
                    !api_.IsApplicationInstalled(ApplicationKey))
                    throw std::runtime_error("Could not restore application registration");
            });
            attempt(errors, [&] { autostart(startup); });
        }
        throw std::runtime_error(failure +
                                 (errors.empty() ? "" : ". Registration restoration: " + errors));
    }
}
} // namespace ct
