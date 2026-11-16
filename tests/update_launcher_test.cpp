#include "update/launcher.h"
#include "update/transaction.h"
#include "update_fixture.h"
#include "app/settings.h"
#include "platform/linux/signals.h"
#include <charconv>
#include <cstdlib>
#include <iostream>
#include <poll.h>
#include <fcntl.h>
#include <sys/wait.h>

namespace {
namespace u = ct::update;
namespace j = ct::json;
using namespace update_fixture;
int mock_application(int argc, char **argv) {
    const auto inherited = std::getenv("CONTROLLER_UTILS_LOCK_FD");
    assert(inherited);
    int descriptor = -1;
    const std::string text(inherited);
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), descriptor);
    assert(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && descriptor >= 3);
    const auto root = std::filesystem::path(argv[0]).parent_path();
    const auto data = root.parent_path() / "app-data";
    ct::SignalQueue signals{SIGUSR1, SIGTERM, SIGINT, SIGHUP};
    ct::ApplicationLock lock(data / "app.lock", descriptor);
    assert(lock.primary() && (fcntl(descriptor, F_GETFD) & FD_CLOEXEC));
    lock.publish();
    u::confirm(root, data, lock);
    auto event = u::release(root).to_json();
    json_object_object_add(event.get(), "kind", json_object_new_string("started"));
    json_object_object_add(
        event.get(), "background",
        json_object_new_boolean(argc == 2 && std::string(argv[1]) == "--background"));
    json_object_object_add(event.get(), "directory",
                           json_object_new_string(std::filesystem::current_path().c_str()));
    std::cout << j::encode(event.get()) << std::endl;
    if (!std::getenv("CT_LAUNCHER_TEST_STAY"))
        return 0;
    for (;;) {
        pollfd descriptor{signals.descriptor(), POLLIN, 0};
        if (poll(&descriptor, 1, 5000) <= 0)
            return 2;
        while (const auto signal = signals.receive()) {
            if (*signal == SIGUSR1)
                std::cout << R"({"kind":"show"})" << std::endl;
            else
                return 0;
        }
    }
}
struct Fixture {
    std::filesystem::path base, app, data;
    Key key = keypair();
    j::Value expected = requirements();
    explicit Fixture(const std::filesystem::path &path, const std::filesystem::path &executable)
        : base(path), app(path / "app"), data(path / "app-data") {
        std::filesystem::create_directories(app / "assets");
        std::filesystem::create_directory(data);
        std::filesystem::copy_file(executable, app / "controller-utils");
        assert(!chmod((app / "controller-utils").c_str(), 0755));
        ct::atomic_write(app / "release.json", j::encode(u::Release{"0.1.0", 1}.to_json().get()),
                         0644);
        ct::atomic_write(app / "assets/update-public.pem", public_key(key.get()), 0644);
        ct::atomic_write(app / "assets/update-channel.json",
                         R"({"url":"https://updates.example.test/stable/"})", 0644);
        ct::atomic_write(app / "assets/tracking-calibration.json", "private calibration", 0644);
        ct::atomic_write(app / "controller-utils.vrmanifest", "private registration", 0644);
        auto settings = ct::Settings{};
        settings.brightness = 42;
        ct::save_settings(data / "settings.json", settings);
    }
    void pending() {
        const auto directory = data / "updates/pending";
        std::filesystem::create_directories(directory);
        auto entries = update_fixture::payload();
        for (auto &entry : entries)
            if (entry.name == "assets/update-public.pem")
                entry.data = public_key(key.get());
        const auto archive =
            make_archive(directory / "controller-utils-fixture-linuxarm64.tar.gz", entries);
        auto metadata = archive.release.to_json();
        json_object_object_add(metadata.get(), "schema", json_object_new_int(5));
        json_object_object_add(metadata.get(), "expires", json_object_new_uint64(archive.expires));
        json_object_object_add(metadata.get(), "artifact",
                               json_object_new_string(archive.artifact.c_str()));
        json_object_object_add(metadata.get(), "size", json_object_new_uint64(archive.size));
        json_object_object_add(metadata.get(), "sha256",
                               json_object_new_string(archive.sha256.c_str()));
        json_object_object_add(metadata.get(), "notes", json_object_new_string("fixture release"));
        json_object_object_add(metadata.get(), "requirements", json_object_get(expected.get()));
        const auto bytes = j::encode(metadata.get());
        ct::atomic_write(directory / "latest.json", bytes);
        sign(directory / "latest.sig", key.get(), bytes);
    }
    ~Fixture() {
        std::filesystem::remove_all(base);
    }
};
class Child {
    pid_t pid_ = -1;
    ct::FileDescriptor output_;
    bool exited_ = false;

  public:
    Child(Fixture &fixture, const std::vector<std::string> &arguments = {}, bool stay = false,
          bool production = false) {
        int channels[2];
        assert(!pipe2(channels, O_CLOEXEC));
        ct::FileDescriptor input(channels[0]), output(channels[1]);
        pid_ = fork();
        assert(pid_ >= 0);
        if (!pid_) {
            assert(dup2(output.get(), STDOUT_FILENO) == STDOUT_FILENO);
            input.reset();
            output.reset();
            if (stay)
                assert(!setenv("CT_LAUNCHER_TEST_STAY", "1", 1));
            else
                unsetenv("CT_LAUNCHER_TEST_STAY");
            try {
                if (production) {
                    const auto launcher = fixture.base / "controller-utils";
                    std::filesystem::copy_file("build/controller-utils-launcher", launcher);
                    assert(!chmod(launcher.c_str(), 0755));
                    execl(launcher.c_str(), launcher.c_str(), "--background", nullptr);
                    _exit(120);
                }
                _exit(u::launch(fixture.app, fixture.data, arguments));
            } catch (const std::exception &error) {
                std::cerr << "launcher test: " << error.what() << '\n';
                _exit(1);
            }
        }
        output_ = std::move(input);
    }
    j::Value event() {
        std::string line;
        const auto deadline = ct::monotonic_time() + 5;
        for (;;) {
            assert(ct::monotonic_time() < deadline);
            pollfd descriptor{output_.get(), POLLIN, 0};
            assert(poll(&descriptor, 1, 1000) > 0);
            char byte;
            assert(read(output_.get(), &byte, 1) == 1);
            if (byte == '\n')
                return j::parse(line);
            assert(line.size() < 8192);
            line.push_back(byte);
        }
    }
    int wait() {
        int status = 0;
        assert(waitpid(pid_, &status, 0) == pid_ && WIFEXITED(status));
        exited_ = true;
        return WEXITSTATUS(status);
    }
    void stop() {
        assert(!kill(pid_, SIGTERM));
        assert(wait() == 0);
    }
    ~Child() {
        if (!exited_) {
            kill(pid_, SIGKILL);
            while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {
            }
        }
    }
};
} // namespace
int main(int argc, char **argv) {
    if (std::filesystem::path(argv[0]).filename() == "controller-utils")
        return mock_application(argc, argv);
    unsetenv("CONTROLLER_UTILS_LOCK_FD");
    const auto executable = std::filesystem::canonical("/proc/self/exe");
    const auto root =
        std::filesystem::current_path() / "build" / ("update-launcher-" + std::to_string(getpid()));
    std::filesystem::create_directory(root);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::filesystem::remove_all(path);
        }
    } cleanup{root};
    {
        Fixture fixture(root / "normal", executable);
        Child child(fixture);
        const auto event = child.event();
        assert(j::string(j::field(event.get(), "kind")) == "started");
        assert(j::unsigned_integer(j::field(event.get(), "sequence")) == 1);
        assert(j::string(j::field(event.get(), "directory")) == fixture.app.string());
        assert(!j::boolean(j::field(event.get(), "background")));
        assert(child.wait() == 0);
    }
    {
        Fixture fixture(root / "production", executable);
        Child child(fixture, {}, false, true);
        const auto event = child.event();
        assert(j::boolean(j::field(event.get(), "background")));
        assert(child.wait() == 0);
    }
    {
        Fixture fixture(root / "secondary", executable);
        Child first(fixture, {}, true);
        first.event();
        fixture.pending();
        const auto signature = ct::sha256(fixture.data / "updates/pending/latest.sig");
        const auto program = ct::sha256(fixture.app / "controller-utils");
        Child second(fixture, {"--show"});
        assert(second.wait() == 0);
        const auto event = first.event();
        assert(j::string(j::field(event.get(), "kind")) == "show");
        assert(signature == ct::sha256(fixture.data / "updates/pending/latest.sig"));
        assert(program == ct::sha256(fixture.app / "controller-utils"));
        assert(!std::filesystem::exists(fixture.data / "updates/transaction.json"));
        first.stop();
    }
    {
        Fixture fixture(root / "failed-candidate", executable);
        fixture.pending();
        Child child(fixture);
        const auto event = child.event();
        assert(j::unsigned_integer(j::field(event.get(), "sequence")) == 1);
        assert(child.wait() == 0);
        ct::ProtectedDirectory cache(fixture.data / "updates", getuid());
        assert(u::read_history(cache).failed_sequence == 0);
        assert(std::filesystem::exists(cache.path() / "pending"));
        assert(ct::load_settings(fixture.data / "settings.json").brightness == 42);
    }
    {
        Fixture fixture(root / "script-refusal", executable);
        ct::atomic_write(fixture.app / "controller-utils", "#!/bin/sh\nexit 88\n", 0755);
        Child child(fixture);
        assert(child.wait() == 1);
    }
    {
        Fixture fixture(root / "bad-arguments", executable);
        Child child(fixture, {"--arbitrary"});
        assert(child.wait() == 1);
    }
    std::cout << "native launcher: real exec, inherited lease, second launch, deferred "
                 "installation and script refusal passed\n";
}
