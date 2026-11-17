#include "helper/backend.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include <map>

namespace {
class Host final : public ct::helper::BackendHost {
    std::map<uintptr_t, uint8_t> memory_;

  public:
    bool live = true, publication = true, published = false;
    unsigned busy = 0, installs = 0, withdrawals = 0, publishes = 0;
    std::vector<ct::helper::TrackerPatch> patches;
    Host() {
        put(ct::helper::CameraPatch::Address, ct::helper::CameraPatch::Original);
        scalar<uint64_t>(ct::helper::AcquisitionReader::AppGlobal, 0x10000);
        scalar<uint64_t>(0x10000, ct::helper::AcquisitionReader::AppVtable);
        scalar<uint64_t>(0x11970, 0x20000);
        scalar<uint64_t>(0x20000, 0x30000);
        scalar<uint64_t>(0x302b0, 0x70000);
        scalar<uint64_t>(0x50000, 0x60000);
        scalar<uint64_t>(0x603b8, 0x10000);
        scalar<uint64_t>(0x602b0, 0x40000);
    }
    void put(uintptr_t address, const ct::helper::Bytes &bytes) {
        for (size_t i = 0; i < bytes.size(); ++i)
            memory_[address + i] = bytes[i];
    }
    template <class T> void scalar(uintptr_t address, T value) {
        ct::helper::Bytes bytes(sizeof(value));
        std::memcpy(bytes.data(), &value, sizeof(value));
        put(address, bytes);
    }
    ct::tracker::Control control() {
        const auto bytes = read(ct::helper::AdapterPatch::Control, 160);
        ct::tracker::Control result;
        std::memcpy(&result, bytes.data(), sizeof(result));
        return result;
    }
    void control(const ct::tracker::Control &state) {
        ct::helper::Bytes bytes(sizeof(state));
        std::memcpy(bytes.data(), &state, sizeof(state));
        put(ct::helper::AdapterPatch::Control, bytes);
    }
    int descriptor() const override {
        return -1;
    }
    bool alive() const override {
        return live;
    }
    void refresh() override {
        if (!live)
            throw std::runtime_error("process gone");
    }
    ct::helper::Bytes read(uintptr_t address, size_t size) override {
        ct::helper::Bytes bytes(size);
        for (size_t i = 0; i < size; ++i)
            bytes[i] = memory_[address + i];
        return bytes;
    }
    void data(uintptr_t address, const ct::helper::Bytes &bytes) override {
        put(address, bytes);
    }
    void code(uintptr_t, const ct::helper::Bytes &) override {
        throw std::logic_error("unexpected live instruction write");
    }
    ct::helper::PatchResult patch(ct::helper::TrackerPatch action) override {
        using namespace ct::helper;
        patches.push_back(action);
        if (action == TrackerPatch::Install) {
            ++installs;
            if (busy) {
                --busy;
                return PatchResult::Busy;
            }
            auto state = control();
            if (!state.magic) {
                state.magic = ct::tracker::Magic;
                state.version = 1;
                control(state);
            }
            put(CameraPatch::Address, CameraPatch::Corrected);
            return PatchResult::Applied;
        }
        if (action == TrackerPatch::CameraRestore || action == TrackerPatch::RecoverUnarmed)
            put(CameraPatch::Address, CameraPatch::Original);
        return PatchResult::Restored;
    }
    bool publish(const ct::tracker::Control &, uint64_t) override {
        ++publishes;
        return published = publication;
    }
    void withdraw(uint64_t) override {
        ++withdrawals;
        published = false;
    }
    void ready(uint64_t raw) {
        auto state = control();
        state.phase = ct::tracker::Ready;
        state.frames++;
        state.callbacks = 0x80000;
        state.wrapper = 0x50000;
        state.manager = 0x60000;
        state.classic = 0x40000;
        control(state);
        scalar<uint32_t>(ct::helper::AdapterPatch::Control + 152, 1);
        const uintptr_t classic = 0x40000, slot = classic + 0x15c0, palm = classic + 0x3c0;
        scalar<uint8_t>(slot + 0x2508, 1);
        scalar<uint64_t>(slot + 8, 0xa0000);
        scalar<uint64_t>(0xa0008, 0xb0000);
        scalar<uint64_t>(0xa0010, 0xb0098);
        scalar<int32_t>(classic + 0x13c0, 0);
        scalar<int32_t>(classic + 0x13c4, 0);
        scalar<double>(classic + 0x13c8, double(raw) * 1e-9 - .1);
        scalar<uint64_t>(palm, 0x90000);
        scalar<uint64_t>(palm + 8, 0x903a0);
        scalar<uint64_t>(palm + 16, 0x903a0);
        scalar<uint8_t>(palm + 0x22, 1);
        for (size_t i = 0; i < 6; ++i)
            scalar<double>(0x90060 + i * 8, 1);
        for (unsigned camera = 0; camera < 2; ++camera) {
            const auto group = classic + 0x6280 + 0x1e0 + camera * 0xa8;
            const auto data = 0xc0000 + camera * 0x1000;
            scalar<uint64_t>(group + 0x48, data);
            scalar<uint64_t>(group + 0x50, data + 96);
            scalar<uint64_t>(group + 0x58, data + 96);
            for (unsigned i = 0; i < 12; ++i)
                scalar<double>(data + i * 8, i % 2 ? 4 : 3);
        }
    }
    void stopped() {
        auto state = control();
        state.phase = ct::tracker::Stopped;
        state.command = state.active = 0;
        state.wrapper = state.manager = state.classic = 0;
        control(state);
    }
};
template <class Function> void rejects(Function fn, const std::string &text) {
    bool failed = false;
    try {
        fn();
    } catch (const std::exception &error) {
        failed = std::string(error.what()).find(text) != std::string::npos;
    }
    assert(failed);
}
bool event(const std::vector<ct::json::Value> &messages, const char *operation) {
    return std::any_of(messages.begin(), messages.end(), [&](const auto &value) {
        return ct::json::string(ct::json::field(value.get(), "op")) == operation;
    });
}
} // namespace
int main() {
    using namespace ct::helper;
    double now = 100;
    uint64_t raw = 1000000000000ULL;
    Host host;
    auto tick = [&](double value) {
        now += value;
        raw += uint64_t(value * 1e9);
        host.stopped();
    };
    for (unsigned cycle = 0; cycle < 100; ++cycle) {
        XrBackend backend(host, [&] { return now; }, [&] { return raw; }, tick);
        assert(backend.start());
        assert(host.control().generation == cycle + 1);
        host.ready(raw);
        const auto first = backend.poll(true);
        assert(event(first, "native_helper_ready") && event(first, "native_helper_quality") &&
               event(first, "native_tracker_state"));
        assert(host.published);
        const auto quality = std::find_if(first.begin(), first.end(), [](const auto &message) {
            return ct::json::string(ct::json::field(message.get(), "op")) ==
                   "native_helper_quality";
        });
        assert(ct::json::boolean(ct::json::field(quality->get(), "good")));
        assert(backend.poll(true).empty());
        raw += 100000000;
        backend.renew();
        assert(host.control().deadline_raw_ns == raw + 1500000000ULL);
        unsigned pulses = 0;
        backend.restore([&] { ++pulses; });
        assert(pulses >= 2 && !host.published && !host.control().manager);
        assert(host.read(CameraPatch::Address, 8) == CameraPatch::Original);
    }
    {
        Host busy;
        busy.busy = 5;
        XrBackend backend(busy, [&] { return now; }, [&] { return raw; });
        for (unsigned attempt = 0; attempt < 4; ++attempt)
            assert(!backend.start());
        rejects([&] { backend.start(); }, "detector is busy");
        backend.restore();
        assert(busy.patches.back() == TrackerPatch::VerifyOriginal);
    }
    {
        Host pending;
        XrBackend backend(pending, [&] { return now; }, [&] { return raw; });
        assert(backend.start());
        assert(!event(backend.poll(true), "native_helper_ready"));
        now += 12.01;
        rejects([&] { backend.poll(true); }, "did not receive camera frames");
        backend.restore();
    }
    {
        Host fresh;
        XrBackend backend(
            fresh, [&] { return now; }, [&] { return raw; },
            [&](double value) {
                now += value;
                fresh.stopped();
            });
        assert(backend.start());
        fresh.ready(raw);
        assert(event(backend.poll(true), "native_helper_ready"));
        now += .51;
        assert(!event(backend.poll(true), "native_helper_ready"));
        assert(!fresh.published);
        fresh.ready(raw);
        backend.poll(false);
        now += .75;
        assert(!event(backend.poll(false), "native_helper_quality"));
        assert(fresh.published);
        fresh.ready(raw);
        assert(!event(backend.poll(false), "native_helper_ready") && fresh.published);
        now += 1.26;
        backend.poll(false);
        assert(!fresh.published);
        fresh.ready(raw);
        backend.poll(false);
        assert(fresh.published);
        fresh.scalar<uint64_t>(AcquisitionReader::AppGlobal, 0);
        now += .5;
        const auto changed = backend.poll(true);
        for (const auto &message : changed)
            if (ct::json::string(ct::json::field(message.get(), "op")) == "native_helper_quality")
                assert(!ct::json::boolean(ct::json::field(message.get(), "good")));
        auto state = fresh.control();
        ++state.generation;
        fresh.control(state);
        rejects([&] { backend.poll(true); }, "tracker stopped");
        backend.restore();
    }
    {
        Host stuck;
        XrBackend backend(
            stuck, [&] { return now; }, [&] { return raw; }, [&](double value) { now += value; });
        backend.start();
        stuck.ready(raw);
        backend.poll(false);
        rejects([&] { backend.restore(); }, "has not stopped");
        assert(!stuck.published && stuck.control().manager);
        assert(std::find(stuck.patches.begin(), stuck.patches.end(), TrackerPatch::CameraRestore) ==
               stuck.patches.end());
        stuck.live = false;
        backend.restore();
    }
    {
        Host incomplete;
        XrBackend backend(incomplete, [&] { return now; }, [&] { return raw; });
        incomplete.scalar<uint32_t>(AdapterPatch::Control, uint32_t(ct::tracker::Magic));
        backend.restore();
        assert(incomplete.patches.back() == TrackerPatch::RecoverUnarmed);
    }
    std::cout << "native backend: 100 generations, acquisition, camera freshness, busy startup, "
                 "leases and cleanup passed\n";
}
