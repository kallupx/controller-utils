#include "update/transaction.h"
#include "update_fixture.h"
#include "app/settings.h"
#include <cerrno>
#include <iostream>
#include <limits>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <sys/wait.h>

namespace {
namespace u = ct::update;
namespace j = ct::json;
enum class Fault { None, Fail, Exit };
Fault fault = Fault::None;
size_t operations = 0, target = 0;
bool before() {
    if (fault == Fault::None)
        return false;
    ++operations;
    if (fault == Fault::Fail && operations == target) {
        fault = Fault::None;
        errno = ENOSPC;
        return true;
    }
    return false;
}
int after(int result) {
    if (!result && fault == Fault::Exit && operations == target)
        _exit(77);
    return result;
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
using namespace update_fixture;
struct Fixture {
    std::filesystem::path base, app, data;
    Key key = keypair();
    j::Value expected = requirements();
    std::unique_ptr<ct::ApplicationLock> application;
    std::unique_ptr<u::MaintenanceLock> maintenance;
    std::unique_ptr<u::Transaction> transaction;
    unsigned captures = 0;
    unsigned mismatch_after = 0;
    bool incompatible = false;
    explicit Fixture(const std::filesystem::path &path)
        : base(path), app(path / "app"), data(path / "data") {
        assert(std::filesystem::create_directory(base));
        assert(std::filesystem::create_directory(data));
        ct::atomic_write(app / "release.json",
                         R"({"version":"0.1.0","sequence":1,"platform":"linuxarm64"})");
        ct::atomic_write(app / "controller-utils", "previous-native-program", 0755);
        ct::atomic_write(app / "assets/update-public.pem", public_key(key.get()), 0644);
        ct::atomic_write(app / "assets/update-channel.json",
                         R"({"url":"https://updates.example.test/stable/"})", 0644);
        ct::atomic_write(app / "assets/tracking-calibration.json", "private calibration", 0644);
        ct::atomic_write(data / "calibrations/controller.json", "current calibration", 0600);
        ct::atomic_write(app / "controller-utils.vrmanifest", "private registration", 0644);
        ct::save_settings(data / "settings.json", {});
        application = std::make_unique<ct::ApplicationLock>(data / "app.lock");
        maintenance = std::make_unique<u::MaintenanceLock>(data);
        transaction = std::make_unique<u::Transaction>(app, data, *application, *maintenance, [&] {
            ++captures;
            return u::InstalledRequirements{
                incompatible || (mismatch_after && captures >= mismatch_after) ? "x86_64"
                                                                               : "aarch64",
                j::parse(j::encode(expected.get()))};
        });
        pending(2);
    }
    void pending(unsigned sequence) {
        const auto pending = data / "updates/pending";
        std::filesystem::create_directories(pending);
        auto entries = update_fixture::payload();
        const u::Release next{"0.1." + std::to_string(sequence - 1), sequence};
        for (auto &entry : entries) {
            if (entry.name == "release.json")
                entry.data = j::encode(next.to_json().get());
            if (entry.name == "assets/update-public.pem")
                entry.data = public_key(key.get());
        }
        const std::string artifact = "controller-utils-fixture-linuxarm64.tar.gz";
        const auto archive = update_fixture::make_archive(pending / artifact, entries);
        auto value = next.to_json();
        json_object_object_add(value.get(), "schema", json_object_new_int(5));
        json_object_object_add(value.get(), "expires", json_object_new_int64(4102444800));
        json_object_object_add(value.get(), "size", json_object_new_uint64(archive.size));
        json_object_object_add(value.get(), "artifact", json_object_new_string(artifact.c_str()));
        json_object_object_add(value.get(), "sha256",
                               json_object_new_string(archive.sha256.c_str()));
        json_object_object_add(value.get(), "notes", json_object_new_string("Native test release"));
        json_object_object_add(value.get(), "requirements", json_object_get(expected.get()));
        const auto bytes = j::encode(value.get());
        ct::atomic_write(pending / "latest.json", bytes);
        sign(pending / "latest.sig", key.get(), bytes);
    }
    void previous() {
        assert(u::release(app).sequence == 1);
        assert(ct::read_file(app / "controller-utils", 128) == "previous-native-program");
        assert(ct::read_file(app / "assets/tracking-calibration.json", 128) ==
               "private calibration");
        assert(ct::read_file(app / "controller-utils.vrmanifest", 128) == "private registration");
        assert(ct::load_settings(data / "settings.json").brightness == 100);
    }
    ~Fixture() {
        transaction.reset();
        maintenance.reset();
        application.reset();
        std::filesystem::remove_all(base);
    }
};
void state_checks(const std::filesystem::path &root) {
    std::filesystem::create_directory(root);
    u::MaintenanceLock lock(root);
    const auto &cache = lock.cache();
    assert(u::read_history(cache).highest_sequence == 0);
    rejects([&] { u::MaintenanceLock duplicate(root); });
    for (const auto text : {"[]", R"({"highest_sequence":true})", R"({"failed_sequence":-1})",
                            R"({"manual_sequence":18446744073709551615})", R"({"last_check":-1})",
                            R"({"last_check":"old"})", R"({"message":false})"}) {
        const auto value = j::parse(text);
        rejects([&] { u::History::from_json(value.get()); });
    }
    u::History history;
    history.highest_sequence = 100;
    history.manual_sequence = 101;
    history.last_check = 12345;
    history.message = "Ready to check for updates.";
    u::save_history(cache, history);
    auto loaded = u::read_history(cache);
    assert(loaded.highest_sequence == 100 && loaded.manual_sequence == 101 &&
           loaded.last_check == 12345 && loaded.message == history.message);
    const auto before = ct::read_file(cache.path() / "state.json", u::MaxMetadata);
    for (const auto &invalid : {std::string(241, 'x'), std::string("hello\0world", 11)}) {
        history.message = invalid;
        rejects([&] { u::save_history(cache, history); });
        assert(ct::read_file(cache.path() / "state.json", u::MaxMetadata) == before);
    }
    ct::write_record(cache, "state.json", "broken history");
    rejects([&] { u::read_history(cache); });
    assert(ct::read_file(cache.path() / "state.json", 100) == "broken history");
    std::filesystem::create_symlink(cache.path() / "state.json", cache.path() / "link");
    rejects([&] { ct::read_record(cache, "link"); });
    rejects([&] { ct::write_record(cache, "link", "overwrite"); });
    rejects([&] { ct::erase_record(cache, "link"); });
    rejects([&] { ct::write_record(cache, "../escape", "bad"); });
    rejects([&] { ct::read_record(cache, "state.json", 1); });
    assert(ct::read_file(cache.path() / "state.json", 100) == "broken history");
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
extern "C" int __real_fsync(int);
extern "C" ssize_t __real_write(int, const void *, size_t);
extern "C" int __real_renameat(int, const char *, int, const char *);
extern "C" int __real_renameat2(int, const char *, int, const char *, unsigned);
extern "C" int __wrap_fsync(int descriptor) {
    return before() ? -1 : after(__real_fsync(descriptor));
}
extern "C" ssize_t __wrap_write(int descriptor, const void *bytes, size_t count) {
    if (before())
        return -1;
    const auto result = __real_write(descriptor, bytes, count);
    if (result >= 0 && fault == Fault::Exit && operations == target)
        _exit(77);
    return result;
}
extern "C" int __wrap_renameat(int a, const char *source, int b, const char *destination) {
    return before() ? -1 : after(__real_renameat(a, source, b, destination));
}
extern "C" int __wrap_renameat2(int a, const char *source, int b, const char *destination,
                                unsigned flags) {
    return before() ? -1 : after(__real_renameat2(a, source, b, destination, flags));
}
int main() {
    const auto root = std::filesystem::current_path() / "build" /
                      ("update-transaction-" + std::to_string(getpid()));
    assert(std::filesystem::create_directory(root));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::filesystem::remove_all(path);
        }
    } cleanup{root};
    state_checks(root / "state");
    {
        Fixture fixture(root / "success");
        assert(fixture.transaction->activate());
        assert(fixture.captures == 2 && u::release(fixture.app).sequence == 2);
        assert(!std::filesystem::exists(fixture.app / "assets/tracking-calibration.json"));
        assert(ct::read_file(fixture.data / "calibrations/controller.json", 128) ==
               "current calibration");
        u::confirm(fixture.app, fixture.data, *fixture.application);
        fixture.transaction->recover();
        assert(u::release(fixture.app).sequence == 2);
        fixture.pending(3);
        assert(fixture.transaction->activate());
        fixture.transaction->recover();
        assert(u::release(fixture.app).sequence == 2);
    }
    {
        Fixture fixture(root / "rollback");
        assert(fixture.transaction->activate());
        auto changed = ct::Settings{};
        changed.brightness = 50;
        ct::save_settings(fixture.data / "settings.json", changed);
        fixture.transaction->recover();
        fixture.previous();
        auto history = u::read_history(fixture.maintenance->cache());
        assert(history.highest_sequence == 2 && history.failed_sequence == 2 &&
               !history.manual_sequence);
        assert(!fixture.transaction->activate());
        history.manual_sequence = 2;
        u::save_history(fixture.maintenance->cache(), history);
        assert(fixture.transaction->activate());
    }
    {
        Fixture fixture(root / "missing-app");
        assert(fixture.transaction->activate());
        std::filesystem::remove_all(fixture.app);
        fixture.transaction->recover();
        fixture.previous();
    }
    {
        Fixture fixture(root / "no-settings");
        std::filesystem::remove(fixture.data / "settings.json");
        assert(fixture.transaction->activate());
        ct::save_settings(fixture.data / "settings.json", {});
        fixture.transaction->recover();
        assert(!std::filesystem::exists(fixture.data / "settings.json"));
    }
    {
        Fixture fixture(root / "disabled");
        auto settings = ct::Settings{};
        settings.automatic_updates = false;
        ct::save_settings(fixture.data / "settings.json", settings);
        assert(!fixture.transaction->activate());
        auto history = u::read_history(fixture.maintenance->cache());
        history.manual_sequence = 2;
        u::save_history(fixture.maintenance->cache(), history);
        assert(fixture.transaction->activate());
    }
    {
        Fixture fixture(root / "incompatible");
        fixture.incompatible = true;
        rejects([&] { fixture.transaction->activate(); });
        fixture.previous();
    }
    {
        Fixture fixture(root / "changed-compatibility");
        fixture.mismatch_after = 2;
        rejects([&] { fixture.transaction->activate(); });
        assert(fixture.captures == 2);
        fixture.previous();
    }
    {
        Fixture fixture(root / "unknown-backup");
        const auto backup = fixture.data / "updates/previous-app";
        std::filesystem::create_directory(backup);
        ct::atomic_write(backup / "preserve", "unrecognized backup");
        rejects([&] { fixture.transaction->activate(); });
        assert(ct::read_file(backup / "preserve", 128) == "unrecognized backup");
        fixture.previous();
    }
    {
        Fixture fixture(root / "corrupt-records");
        ct::write_record(fixture.maintenance->cache(), "state.json", "corrupt history");
        rejects([&] { fixture.transaction->activate(); });
        fixture.previous();
        ct::write_record(fixture.maintenance->cache(), "transaction.json", "corrupt journal");
        rejects([&] { fixture.transaction->recover(); });
        fixture.previous();
    }
    {
        Fixture fixture(root / "bad-startup");
        assert(fixture.transaction->activate());
        ct::atomic_write(fixture.app / "release.json", "invalid candidate release");
        ct::atomic_write(fixture.app / "controller-utils", "invalid candidate binary");
        rejects([&] { u::confirm(fixture.app, fixture.data, *fixture.application); });
        fixture.transaction->recover();
        fixture.previous();
    }
    {
        Fixture fixture(root / "secondary-launch");
        ct::ApplicationLock secondary(fixture.data / "app.lock");
        assert(!secondary.primary());
        rejects([&] {
            u::Transaction invalid(fixture.app, fixture.data, secondary, *fixture.maintenance,
                                   [] { return u::InstalledRequirements{}; });
        });
        fixture.previous();
    }
    size_t count = 0;
    {
        Fixture fixture(root / "operation-count");
        operations = 0;
        target = SIZE_MAX;
        fault = Fault::Fail;
        assert(fixture.transaction->activate());
        fault = Fault::None;
        count = operations;
    }
    const auto before_cycles = descriptors();
    for (size_t point = 1; point <= count; ++point) {
        Fixture fixture(root / "write-failure");
        operations = 0;
        target = point;
        fault = Fault::Fail;
        rejects([&] { fixture.transaction->activate(); });
        fault = Fault::None;
        fixture.transaction->recover();
        fixture.previous();
    }
    for (size_t point = 1; point <= count; ++point) {
        Fixture fixture(root / "interrupted");
        const pid_t child = fork();
        assert(child >= 0);
        if (!child) {
            operations = 0;
            target = point;
            fault = Fault::Exit;
            fixture.transaction->activate();
            _exit(99);
        }
        int status = 0;
        assert(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
               WEXITSTATUS(status) == 77);
        assert(std::filesystem::is_regular_file(fixture.app / "controller-utils"));
        fixture.transaction->recover();
        fixture.previous();
    }
    size_t recovery_count = 0;
    {
        Fixture fixture(root / "recovery-operation-count");
        assert(fixture.transaction->activate());
        operations = 0;
        target = SIZE_MAX;
        fault = Fault::Fail;
        fixture.transaction->recover();
        fault = Fault::None;
        recovery_count = operations;
    }
    for (size_t point = 1; point <= recovery_count; ++point) {
        Fixture fixture(root / "failed-recovery");
        assert(fixture.transaction->activate());
        operations = 0;
        target = point;
        fault = Fault::Fail;
        rejects([&] { fixture.transaction->recover(); });
        fault = Fault::None;
        fixture.transaction->recover();
        fixture.previous();
    }
    for (size_t point = 1; point <= recovery_count; ++point) {
        Fixture fixture(root / "interrupted-recovery");
        assert(fixture.transaction->activate());
        auto changed = ct::Settings{};
        changed.brightness = 50;
        ct::save_settings(fixture.data / "settings.json", changed);
        const pid_t child = fork();
        assert(child >= 0);
        if (!child) {
            operations = 0;
            target = point;
            fault = Fault::Exit;
            fixture.transaction->recover();
            _exit(99);
        }
        int status = 0;
        assert(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
               WEXITSTATUS(status) == 77);
        fixture.transaction->recover();
        fixture.previous();
    }
    assert(descriptors() == before_cycles);
    std::cout << "native update history, activation, confirmation and rollback passed, " << count
              << " write failures, " << count << " interrupted updates and " << recovery_count
              << " failed and interrupted recoveries, no descriptor growth\n";
}
