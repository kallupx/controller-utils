#include "update/repair.h"
#include "update_fixture.h"
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>

using namespace ct;
using namespace ct::update;
using namespace update_fixture;
template <class F> void rejects(F f) {
    bool rejected = false;
    try {
        f();
    } catch (const std::exception &) {
        rejected = true;
    }
    assert(rejected);
}
struct FakeChild : ChildChannel {
    std::optional<int> exit_status() override {
        return 126 << 8;
    }
    void send(const std::string &) override {}
    std::vector<json::Value> poll() override {
        return {};
    }
    std::vector<json::Value> close(const std::string &, double) override {
        return {};
    }
};
int main() {
    if (getuid() != 0) {
        assert(inspect_repair({}, {}, "") == RepairStatus::NeedsAuthorization);
        rejects([] { repair_permissions({}, {}, "", [] {}); });
        std::cout << "repair: unprivileged root operations are rejected\n";
        return 0;
    }
    if (!std::getenv("CT_MAINTENANCE_ISOLATED")) {
        std::cout << "repair: isolated root fixture not requested\n";
        return 0;
    }
    const std::filesystem::path root = "/fixture";
    std::filesystem::create_directories(root / "native");
    std::filesystem::create_directories(root / "cache");
    std::filesystem::create_directories(root / "policies");
    std::filesystem::create_directories(root / "user");
    UpdateInstallation installation{
        {root / "user/app", root / "native"}, root / "user", root / "cache", 1000, 1000};
    SetupPolicies paths{root / "policies/sudoers", root / "policies/udev"};
    auto key = keypair();
    auto pub = public_key(key.get());
    auto expected = requirements();
    auto release_value = Release{"0.1.1", 2}.to_json();
    json_object_object_add(release_value.get(), "schema", json_object_new_int(5));
    json_object_object_add(release_value.get(), "expires", json_object_new_int(0));
    json_object_object_add(release_value.get(), "artifact",
                           json_object_new_string("controller-utils-fixture-linuxarm64.tar.gz"));
    json_object_object_add(release_value.get(), "size", json_object_new_int(1));
    json_object_object_add(release_value.get(), "sha256",
                           json_object_new_string(std::string(64, 'a').c_str()));
    json_object_object_add(release_value.get(), "notes", json_object_new_string("fixture"));
    json_object_object_add(release_value.get(), "requirements", expected.release());
    auto bytes = json::encode(release_value.get());
    sign(root / "signature", key.get(), bytes);
    auto sig = read_file(root / "signature", 64);
    auto updater = root / "native/controller-utils-updater";
    atomic_write(updater, program(), 0755);
    ProtectedDirectory native(root / "native"), cache(root / "cache");
    assert(inspect_repair(installation, paths, pub) == RepairStatus::ReinstallRequired);
    record_repair_evidence(native, bytes, sig, pub, {"0.1.1", 2});
    assert(verify_repair_updater(native, pub).sequence == 2);
    auto receipt = Release{"0.1.1", 2}.to_json();
    json_object_object_add(receipt.get(), "sha256",
                           json_object_new_string(std::string(64, 'a').c_str()));
    write_record(cache, "public.pem", pub, 0644);
    write_record(cache, "update.lock", "");
    write_record(cache, "setup.json", json::encode(receipt.get()));
    write_record(cache, "sudoers", "known sudoers\n");
    write_record(cache, "udev.rules", "known udev\n");
    atomic_write(root / "user/settings.json", "settings", 0600);
    atomic_write(root / "user/calibration.json", "calibration", 0600);
    unsigned reloads = 0;
    auto reload = [&] { ++reloads; };
    assert(inspect_repair(installation, paths, pub) == RepairStatus::Missing);
    repair_permissions(installation, paths, pub, reload);
    assert(reloads == 1 && inspect_repair(installation, paths, pub) == RepairStatus::Intact);
    repair_permissions(installation, paths, pub, reload);
    assert(reloads == 1);
    {
        FileDescriptor held(openat(cache.descriptor(), "update.lock", O_RDWR | O_CLOEXEC));
        assert(held && !flock(held.get(), LOCK_EX | LOCK_NB));
        assert(inspect_repair(installation, paths, pub) == RepairStatus::Busy);
        rejects([&] { repair_permissions(installation, paths, pub, reload); });
    }
    assert(read_file(root / "user/settings.json", 32) == "settings");
    assert(read_file(root / "user/calibration.json", 32) == "calibration");
    const auto evidence = *read_record(native, ".repair-evidence", MaxMetadata + 64);
    write_record(native, ".repair-evidence", "corrupted");
    assert(inspect_repair(installation, paths, pub) == RepairStatus::ReinstallRequired);
    rejects([&] { repair_permissions(installation, paths, pub, reload); });
    assert(read_file(paths.sudoers, 64) == "known sudoers\n");
    write_record(native, ".repair-evidence", evidence, 0644);
    std::filesystem::rename(root / "native", root / "native-original");
    std::filesystem::create_directory(root / "native");
    rejects([&] { verify_repair_updater(native, pub); });
    rejects([&] { repair_permissions(installation, paths, pub, reload); });
    assert(read_file(paths.sudoers, 64) == "known sudoers\n");
    std::filesystem::remove(root / "native");
    std::filesystem::rename(root / "native-original", root / "native");
    std::filesystem::create_directory_symlink(root / "policies", root / "linked");
    SetupPolicies unsafe{root / "linked/sudoers", paths.udev};
    assert(inspect_repair(installation, unsafe, pub) == RepairStatus::Conflict);
    rejects([&] { repair_permissions(installation, unsafe, pub, reload); });
    assert(read_file(paths.sudoers, 64) == "known sudoers\n");
    std::filesystem::remove(paths.sudoers);
    std::filesystem::remove(paths.udev);
    atomic_write(paths.udev, "unrelated", 0644);
    assert(inspect_repair(installation, paths, pub) == RepairStatus::Conflict);
    rejects([&] { repair_permissions(installation, paths, pub, reload); });
    assert(!std::filesystem::exists(paths.sudoers) && read_file(paths.udev, 32) == "unrelated");
    std::filesystem::remove(paths.udev);
    std::filesystem::create_symlink(root / "user/settings.json", paths.udev);
    assert(inspect_repair(installation, paths, pub) == RepairStatus::Conflict);
    rejects([&] { repair_permissions(installation, paths, pub, reload); });
    std::filesystem::remove(paths.udev);
    bool first = true;
    rejects([&] {
        repair_permissions(installation, paths, pub, [&] {
            if (first) {
                first = false;
                throw std::runtime_error("reload failure");
            }
        });
    });
    assert(!std::filesystem::exists(paths.sudoers) && !std::filesystem::exists(paths.udev));
    rejects([&] {
        repair_permissions(installation, paths, pub,
                           [] { throw std::runtime_error("reload unavailable"); });
    });
    assert(inspect_repair(installation, paths, pub) == RepairStatus::Interrupted);
    assert(read_record(cache, "repair-journal", 2));
    repair_permissions(installation, paths, pub, reload);
    assert(inspect_repair(installation, paths, pub) == RepairStatus::Intact);
    std::filesystem::remove(paths.sudoers);
    std::filesystem::remove(paths.udev);
    write_record(cache, "repair-journal", "10");
    atomic_write(paths.sudoers, "known sudoers\n", 0440);
    atomic_write(root / "policies/.sudoers.controller-utils-repair.pending", "known", 0440);
    assert(inspect_repair(installation, paths, pub) == RepairStatus::Interrupted);
    repair_permissions(installation, paths, pub, reload);
    assert(!std::filesystem::exists(root / "policies/.sudoers.controller-utils-repair.pending"));
    assert(inspect_repair(installation, paths, pub) == RepairStatus::Intact);
    atomic_write(updater, "replacement", 0755);
    assert(inspect_repair(installation, paths, pub) == RepairStatus::ReinstallRequired);
    rejects([&] { repair_permissions(installation, paths, pub, reload); });
    atomic_write(updater, program(), 0755);
    assert(inspect_repair(installation, paths, pub) == RepairStatus::Intact);
    write_record(cache, "setup.json", "bad");
    assert(inspect_repair(installation, paths, pub) == RepairStatus::ReinstallRequired);
    unsigned launches = 0;
    ChildFactory runner = [&](const std::vector<std::string> &command) {
        ++launches;
        assert((command == std::vector<std::string>{
                               "/usr/bin/sudo", "-n",
                               "/var/lib/controller-utils/controller-utils-updater", "--repair"}));
        return std::make_unique<FakeChild>();
    };
    assert(launches == 0);
    rejects([&] { start_repair_action(runner, [] { throw std::runtime_error("unavailable"); }); });
    assert(launches == 0);
    auto action = start_repair_action(runner, [] {});
    assert(launches == 1 && action->exit_status() == (126 << 8));
    ChildFactory inspector = [&](const std::vector<std::string> &command) {
        ++launches;
        assert((command ==
                std::vector<std::string>{"/usr/bin/sudo", "-n",
                                         "/var/lib/controller-utils/controller-utils-updater",
                                         "--repair-status"}));
        return std::make_unique<FakeChild>();
    };
    auto inspection = start_inspection_action(inspector, [] {});
    assert(launches == 2 && inspection->exit_status() == (126 << 8));
    std::cout << "repair: signed updater, policies, conflict, rollback, recovery and preservation "
                 "passed\n";
}
