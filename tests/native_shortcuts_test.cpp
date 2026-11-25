#include "steamvr/shortcuts.h"
#include "steamvr/module.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
class Backend final : public ct::ShortcutBackend {
  public:
    std::vector<std::string> events;
    bool fail_progress = false, fail_prepare = false;
    unsigned prepared = 0;
    void recenter() override {
        events.emplace_back("recenter");
    }
    void toggle_room_view() override {
        events.emplace_back("passthrough");
    }
    void prepare_progress() override {
        ++prepared;
        if (fail_prepare)
            throw std::runtime_error("animation unavailable");
    }
    void post_progress(const ct::ShortcutProgress &event) override {
        events.push_back(
            (event.action == ct::ShortcutAction::Recenter ? "recenter:" : "passthrough:") +
            std::to_string(event.amount));
        if (fail_progress)
            throw std::runtime_error("progress failed");
    }
    void post_event(const vr::VREvent_t &event) override {
        assert(event.eventType == vr::VREvent_ScreenshotTriggered);
        assert(event.trackedDeviceIndex == vr::k_unTrackedDeviceIndexInvalid);
        assert(event.eventAgeSeconds == 0);
        const vr::VREvent_Data_t empty{};
        assert(!std::memcmp(&event.data, &empty, sizeof(empty)));
        events.push_back("screenshot");
    }
};
template <class Function> void rejects(Function function, const std::string &expected) {
    bool failed = false;
    try {
        function();
    } catch (const std::exception &error) {
        failed = error.what() == expected;
    }
    assert(failed);
}
} // namespace

int main() {
    {
        struct Fixture {
            char name[19] = "16CHmdSystemLatest";
            uintptr_t type[2]{};
            uintptr_t table[27]{};
            uint32_t thunk[2]{0x9100c000, 0x14000001};
            uint32_t target[2]{0xd65f03c0, 0xd503201f};
        } fixture;
        fixture.type[1] = reinterpret_cast<uintptr_t>(fixture.name);
        fixture.table[0] = uintptr_t(-24);
        fixture.table[1] = reinterpret_cast<uintptr_t>(fixture.type);
        fixture.table[26] = reinterpret_cast<uintptr_t>(fixture.thunk);
        const auto begin = reinterpret_cast<uintptr_t>(&fixture);
        ct::steamvr::Module module({{begin, begin + sizeof(fixture), true}});
        const char *digest = "110f46b5b35c069160560c6ad6786f647dd44e8760a52a46fc22dbbcd7630b91";
        assert(ct::validate_progress_table(module, fixture.table + 2, 4, digest));
        fixture.thunk[1] = 0x14000002;
        assert(!ct::validate_progress_table(module, fixture.table + 2, 4, digest));
        fixture.thunk[1] = 0x14000001;
        fixture.target[0] ^= 1;
        assert(!ct::validate_progress_table(module, fixture.table + 2, 4, digest));
    }
    using Action = ct::ShortcutAction;
    Backend backend;
    double now = 100;
    ct::NativeShortcuts shortcuts(backend, [&] { return now; });
    assert(backend.events.empty());
    shortcuts.send_action(Action::Recenter);
    shortcuts.send_action(Action::Passthrough);
    shortcuts.send_action(Action::Passthrough);
    assert((backend.events == std::vector<std::string>{"recenter", "passthrough", "passthrough"}));
    shortcuts.send_action(Action::Screenshot);
    assert(backend.events.back() == "screenshot" && !backend.prepared);
    shortcuts.progress(std::nullopt);
    assert(!backend.prepared);
    shortcuts.progress(Action::Recenter, .5);
    assert(backend.prepared == 1 && backend.events.back() == "recenter:0.500000");
    const auto count = backend.events.size();
    now += .01;
    shortcuts.progress(Action::Recenter, .6);
    assert(backend.events.size() == count);
    shortcuts.progress(Action::Passthrough, .1);
    assert(backend.events[backend.events.size() - 2] == "recenter:0.000000");
    assert(backend.events.back() == "passthrough:0.100000");
    now += .06;
    shortcuts.progress(Action::Passthrough, .2);
    assert(backend.events.back() == "passthrough:0.200000");
    shortcuts.progress(Action::Passthrough, 0);
    assert(backend.events.back() == "passthrough:0.000000");
    for (double invalid : {-1., 1.1, std::numeric_limits<double>::quiet_NaN()})
        rejects([&] { shortcuts.progress(Action::Recenter, invalid); },
                "invalid native shortcut progress");
    rejects([&] { shortcuts.progress(std::nullopt, .1); }, "invalid native shortcut progress");
    rejects([&] { shortcuts.progress(Action::Screenshot); }, "invalid native shortcut progress");
    shortcuts.close();
    assert(backend.events.back() == "passthrough:0.000000" && backend.prepared == 1);
    const auto closed_count = backend.events.size();
    shortcuts.close();
    assert(backend.events.size() == closed_count);
    rejects([&] { shortcuts.send_action(Action::Recenter); }, "shortcuts are closed");
    rejects([&] { shortcuts.send_action(Action::Screenshot); }, "shortcuts are closed");
    {
        Backend failed;
        failed.fail_prepare = true;
        ct::NativeShortcuts owner(failed, [&] { return now; });
        rejects([&] { owner.progress(Action::Recenter, .5); }, "animation unavailable");
        owner.send_action(Action::Recenter);
        assert(failed.events.back() == "recenter");
        owner.send_action(Action::Passthrough);
        assert(failed.events.back() == "passthrough");
        owner.send_action(Action::Screenshot);
        assert(failed.events.back() == "screenshot");
    }
    {
        Backend failed;
        ct::NativeShortcuts owner(failed, [&] { return now; });
        owner.progress(Action::Recenter, .5);
        failed.fail_progress = true;
        rejects([&] { owner.close(); }, "progress failed");
        failed.fail_progress = false;
    }
}
