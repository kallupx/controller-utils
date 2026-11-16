#include "installer.h"
#include "setup.h"
#include "repair.h"
#include "update_public_key.h"
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <vector>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
void system_command(const std::vector<const char *> &arguments) {
    const auto child = fork();
    if (child < 0)
        throw std::runtime_error("Could not start system maintenance");
    if (!child) {
        auto command = arguments;
        command.push_back(nullptr);
        execv(command[0], const_cast<char *const *>(command.data()));
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR)
            throw std::runtime_error("Could not wait for system maintenance");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status))
        throw std::runtime_error("System maintenance failed");
}
void reload_rules() {
    system_command({"/usr/bin/udevadm", "control", "--reload-rules"});
    system_command({"/usr/bin/udevadm", "trigger", "--subsystem-match=hidraw", "--action=change"});
}
} // namespace

int main(int argc, char **argv) {
    try {
        umask(0077);
        if (argc == 4 && std::string(argv[1]) == "--stage") {
            ct::update::stage_package(std::filesystem::absolute(argv[2]),
                                      std::filesystem::absolute(argv[3]), UpdatePublicKey);
            return 0;
        }
        const bool install = argc == 3 && std::string(argv[1]) == "--install";
        const bool remove = argc == 2 && std::string(argv[1]) == "--uninstall";
        const bool repair = argc == 2 && std::string(argv[1]) == "--repair";
        const bool repair_status = argc == 2 && std::string(argv[1]) == "--repair-status";
        if (!install && !remove && !repair && !repair_status &&
            (argc != 2 ||
             (std::string(argv[1]) != "--apply" && std::string(argv[1]) != "--confirm")))
            throw std::invalid_argument(
                "usage: controller-utils-updater --apply|--confirm|--install "
                "FEED|--uninstall|--repair|--repair-status|--stage FEED DESTINATION");
        const auto caller = std::getenv("SUDO_UID");
        const auto user = getpwnam("steamos");
        if (!user || user->pw_uid != 1000 || (caller && std::string(caller) != "1000"))
            throw std::runtime_error("Unexpected Frame application owner");
        const std::filesystem::path base =
            std::filesystem::path(user->pw_dir) / ".local/share/controller-utils";
        const ct::update::UpdateInstallation installation{{base / "app"},
                                                          base / "app-data",
                                                          "/var/lib/controller-utils-updates",
                                                          user->pw_uid,
                                                          user->pw_gid};
        if (install) {
            ct::update::setup_install(installation, {}, std::filesystem::absolute(argv[2]),
                                      UpdatePublicKey);
            reload_rules();
            system_command({"/usr/bin/systemctl", "daemon-reload"});
        } else if (remove) {
            ct::update::setup_remove(installation, {}, UpdatePublicKey);
            reload_rules();
            system_command({"/usr/bin/systemctl", "daemon-reload"});
        } else if (repair) {
            if (getuid() != 0 || geteuid() != 0)
                throw std::runtime_error("Permission repair requires the authorized root updater");
            ct::update::repair_permissions(installation, {}, UpdatePublicKey, reload_rules);
        } else if (repair_status) {
            if (getuid() != 0 || geteuid() != 0)
                throw std::runtime_error(
                    "Permission inspection requires the authorized root updater");
            const auto status = ct::update::inspect_repair(installation, {}, UpdatePublicKey);
            const char *label = "reinstall-required";
            switch (status) {
            case ct::update::RepairStatus::NeedsAuthorization:
                label = "authorization-required";
                break;
            case ct::update::RepairStatus::Busy:
                label = "busy";
                break;
            case ct::update::RepairStatus::Intact:
                label = "intact";
                break;
            case ct::update::RepairStatus::Missing:
                label = "missing";
                break;
            case ct::update::RepairStatus::Conflict:
                label = "conflict";
                break;
            case ct::update::RepairStatus::ReinstallRequired:
                label = "reinstall-required";
                break;
            case ct::update::RepairStatus::Interrupted:
                label = "interrupted";
                break;
            }
            std::cout << "{\"op\":\"repair-status\",\"status\":\"" << label << "\"}\n";
        } else
            ct::update::install_update(installation, std::string(argv[1]) == "--confirm");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Controller Utils updater: " << error.what() << '\n';
        return 1;
    }
}
