#include "fake_native_queue.h"
#include "steamvr/transport.h"
#include "platform/linux/error.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace {
using ct::steamvr::BlockQueue;
using ct::test::Clock;
using ct::test::Queue;
template <class Function> void rejects(Function function, const std::string &message) {
    bool rejected = false;
    try {
        function();
    } catch (const std::exception &error) {
        rejected = std::string(error.what()).find(message) != std::string::npos;
    }
    assert(rejected);
}
} // namespace
int main() {
    const auto profile = ct::controller_profile("TEST1234");
    {
        Queue queue;
        Clock clock;
        ct::QueueContext context(queue);
        for (int i = 0; i < 100; ++i) {
            ct::NativeTracking tracking(
                context, profile, [&] { return clock.now(); }, [&](double t) { clock.sleep(t); });
            assert(!tracking.poll(100.01));
            assert(tracking.session_start == 100);
            tracking.close();
            tracking.close();
        }
        assert(std::count(queue.opened.begin(), queue.opened.end(), 1) == 1);
        assert(std::count(queue.destroyed.begin(), queue.destroyed.end(), 1) == 0);
        assert(std::count(queue.destroyed.begin(), queue.destroyed.end(), 2) == 100);
        assert(std::count(queue.destroyed.begin(), queue.destroyed.end(), 3) == 100);
        assert(queue.writes.size() == 200);
        context.close();
        assert(std::count(queue.destroyed.begin(), queue.destroyed.end(), 1) == 1);
    }
    for (const char *path : {"/xrservice/controller/data", "/xrservice/controller/event"}) {
        Queue queue;
        ct::QueueContext context(queue);
        queue.connect_errors[path] = 2;
        queue.partial_connect = true;
        rejects([&] { context.preflight(); }, "Native controller startup is not ready");
        assert(queue.destroyed ==
               std::vector<uint64_t>(queue.opened.rbegin(), queue.opened.rend()));
        assert(queue.writes.empty());
    }
    {
        Queue queue;
        ct::QueueContext context(queue);
        context.preflight();
        assert((queue.opened == std::vector<uint64_t>{4, 5}));
        assert((queue.destroyed == std::vector<uint64_t>{5, 4}));
    }
    {
        Queue queue;
        Clock clock;
        ct::QueueContext context(queue);
        ct::NativeTracking tracking(
            context, profile, [&] { return clock.now(); }, [&](double t) { clock.sleep(t); });
        tracking.poll(100.01);
        queue.pose(100.02, 16, true);
        assert(!tracking.poll(100.03) && tracking.rejected_poses == 1 && tracking.requested);
        queue.pose(100.04);
        assert(tracking.poll(100.05)->timestamp == 100.04);
        assert(!tracking.poll(100.06));
        queue.pose(100.01);
        assert(!tracking.poll(100.07));
        queue.pose(101);
        assert(!tracking.poll(100.07));
        queue.pose(-1);
        assert(!tracking.poll(100.07));
        queue.pose(100.08);
        assert(tracking.poll(100.09));
        queue.pose(100.1, 17);
        rejects([&] { tracking.poll(100.11); }, "another controller");
        queue.packets.erase(3);
        assert(!tracking.poll(100.8));
        rejects([&] { tracking.poll(101.02); }, "stopped responding");
        tracking.close();
    }
    for (int error : {0, -6}) {
        Queue queue;
        Clock clock;
        ct::QueueContext context(queue);
        ct::NativeTracking tracking(
            context, profile, [&] { return clock.now(); }, [&](double t) { clock.sleep(t); });
        tracking.poll(100.01);
        queue.status(true, 101, error, 3, true);
        rejects([&] { tracking.poll(100.1); }, "session changed");
        tracking.close();
        assert(queue.writes.size() == 1 && !tracking.requested);
    }
    {
        Queue queue;
        Clock clock;
        ct::QueueContext context(queue);
        ct::NativeTracking tracking(
            context, profile, [&] { return clock.now(); }, [&](double t) { clock.sleep(t); });
        tracking.poll(100.01);
        queue.status(true, 100, -6, 3, true);
        queue.pose(100.09);
        assert(tracking.poll(100.1) && tracking.rejected_inputs == 1 && tracking.requested);
        queue.write_result = 3;
        assert(!tracking.sample(100.1, {}));
        assert(!tracking.controls(100.1, std::nullopt, {}, false));
        queue.write_result = 0;
        queue.status(false, 100, -6, 3, true);
        bool reconnect = false;
        try {
            tracking.poll(100.2);
        } catch (const ct::ConnectionError &) {
            reconnect = true;
        }
        assert(reconnect);
        tracking.close();
    }
    {
        Queue queue;
        Clock clock;
        ct::QueueContext context(queue);
        queue.release_error_once = 7;
        rejects(
            [&] {
                ct::NativeTracking tracking(
                    context, profile, [&] { return clock.now(); },
                    [&](double t) { clock.sleep(t); });
            },
            "Native queue publish: 7");
        assert(queue.writes.size() == 2);
        assert((queue.destroyed == std::vector<uint64_t>{2}));
        ct::wire::EventHeader event{};
        std::memcpy(&event, queue.writes.back().data, sizeof(event));
        assert(!event.connected);
    }
    {
        Queue queue;
        Clock clock;
        ct::QueueContext context(queue);
        ct::NativeTracking tracking(
            context, profile, [&] { return clock.now(); }, [&](double t) { clock.sleep(t); });
        tracking.poll(100.01);
        queue.ack_disconnect = false;
        rejects([&] { tracking.close(); }, "disconnect was not confirmed");
        assert(clock.value >= 1 && !tracking.requested);
        assert((queue.destroyed == std::vector<uint64_t>{3, 2}));
    }
    {
        Queue queue;
        Clock clock;
        ct::QueueContext context(queue);
        queue.connect_errors["/controller-utils/native/input_v1"] = 7;
        queue.partial_connect = true;
        rejects([&] { context.command_writer(); }, "Native queue");
        assert((queue.destroyed == std::vector<uint64_t>{1}));
        queue.connect_errors.clear();
        assert(context.command_writer() == 1);
    }
    {
        Queue queue;
        Clock clock;
        ct::QueueContext context(queue);
        ct::NativeTracking tracking(
            context, profile, [&] { return clock.now(); }, [&](double t) { clock.sleep(t); });
        queue.null_read = true;
        rejects([&] { tracking.poll(100.01); }, "null native output buffer");
        assert(queue.read_releases.back() == 2);
        queue.null_read = false;
        tracking.close();
    }
    std::cout << "native transport: single writer, 100 sessions, leases, partial delivery, "
                 "ownership and cleanup passed\n";
}
