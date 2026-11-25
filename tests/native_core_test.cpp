#include "controller/report.h"
#include "controller/sensor_clock.h"
#include "interaction/gestures.h"
#include "platform/json.h"
#include "steamvr/codec.h"
#include "tracking/acquisition.h"
#include "tracking/math.h"
#include "tracking/position.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
template <class Function> void rejects(Function function) {
    bool rejected = false;
    try {
        function();
    } catch (const std::exception &) {
        rejected = true;
    }
    assert(rejected);
}
} // namespace
int main() {
    using namespace ct;
    {
        SteamShortcuts keys;
        keys.update(0, 10, true, true);
        keys.update(0x10000, 10.01, true, true);
        keys.update(0x10000, 10.2, true, true);
        keys.update(0x10000, 10.4, true, true);
        assert(!keys.progress(10.50));
        keys.update(0x10000, 10.52, true, true);
        assert(keys.progress(10.52));
        assert(keys.update(0x10004, 10.55, true, true) == std::vector<std::string>{"keyboard"});
        assert(!keys.progress(10.55));
        assert(keys.update(0, 10.6, true, true).empty());
        for (double now = 10.8; now < 11.5; now += .1)
            assert(keys.update(0, now, true, true).empty());
        keys.update(0x10000, 11.5, true, true);
        assert(keys.update(0, 11.6, true, true).empty());
        for (double now : {11.8, 12.0})
            assert(keys.update(0, now, true, true).empty());
        assert(keys.update(0, 12.11, true, true) == std::vector<std::string>{"dashboard"});
        keys.update(0x10000, 12.2, true, true);
        keys.update(0, 12.25, true, true);
        keys.update(0x10000, 12.35, true, true);
        assert(keys.update(0, 12.4, true, true) == std::vector<std::string>{"passthrough"});
        for (double now : {12.6, 12.8, 13., 13.2})
            assert(keys.update(0, now, true, true).empty());
        keys.update(0x10000, 13.3, true, true);
        for (double now = 13.4; now < 14.4; now += .1)
            keys.update(0x10000, now, true, true);
        assert(keys.update(0, 14.4, true, true) == std::vector<std::string>{"recenter"});
        keys.update(0x10000, 14.5, false, true);
        assert(keys.update(0, 14.6, false, true) == std::vector<std::string>{"dashboard"});
    }
    for (uint32_t first : {0x10000u, 0x200u, 0x10200u}) {
        SteamShortcuts keys;
        assert(keys.update(0, 10, true, true).empty());
        const auto initial = keys.update(first, 10.01, true, true);
        assert(initial == (first == 0x10200 ? std::vector<std::string>{"screenshot"}
                                            : std::vector<std::string>{}));
        const auto chord = keys.update(0x10200, 10.02, true, true);
        assert(chord == (first == 0x10200 ? std::vector<std::string>{}
                                          : std::vector<std::string>{"screenshot"}));
        for (double now = 10.1; now < 11.5; now += .1) {
            assert(keys.update(0x10200, now, true, true).empty());
            assert(!keys.progress(now));
        }
        assert(keys.update(0x10000, 11.5, true, true).empty());
        assert(keys.update(0x10200, 11.51, true, true) == std::vector<std::string>{"screenshot"});
        assert(keys.update(0, 11.6, true, true).empty());
        for (double now = 11.7; now < 12.3; now += .1)
            assert(keys.update(0, now, true, true).empty());
    }
    {
        SteamShortcuts keys;
        constexpr uint32_t Steam = 0x10000, Camera = 0x90000, Screenshot = 0x10200;
        assert(keys.update(Camera, 10, true, true).empty());
        assert(keys.update(0, 10.01, true, true).empty());
        assert(keys.update(Steam, 10.02, true, true).empty());
        assert(keys.update(Camera, 10.03, true, true) == std::vector<std::string>{"camera_switch"});
        assert(keys.update(Camera, 10.04, true, true).empty());
        assert(!keys.progress(10.04));
        assert(keys.update(0, 10.20, false, false, false).empty());
        assert(keys.update(Camera, 10.21, true, true).empty());
        assert(keys.update(0, 10.22, true, true).empty());
        assert(keys.update(Camera | Screenshot, 10.23, true, true) ==
               std::vector<std::string>{"screenshot"});
        assert(keys.update(Camera, 10.24, true, true).empty());
        assert(keys.update(0, 10.25, true, true).empty());
        assert(keys.update(Camera, 10.26, true, true) == std::vector<std::string>{"camera_switch"});
        assert(keys.update(0, 10.27, true, true).empty());
    }
    for (bool keyboard : {false, true}) {
        SteamShortcuts keys;
        assert(keys.update(0x10200, 10, true, keyboard).empty());
        assert(keys.update(0, 10.1, true, keyboard).empty());
        assert(keys.update(0x10200, 11, true, keyboard).empty());
        assert(keys.update(0, 11.1, false, keyboard).empty());
        assert(keys.update(0x10200, 11.2, false, keyboard).empty());
        assert(keys.update(0x10200, 11.3, true, keyboard).empty());
        assert(keys.update(0x10000, 11.4, true, keyboard).empty());
        assert(keys.update(0x10200, 11.5, true, keyboard) ==
               std::vector<std::string>{"screenshot"});
    }
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    static_assert(ButtonMask == 0x0ccfffff);
    rejects([&] { decode_report(nullptr, 0); });
    rejects([&] { setting_report(0x24, 101); });
    rejects([&] { setting_report(0x30, 1); });
    rejects([&] { setting_report(0x124, 0); });
    assert(setting_report(0x24, 100)[4] == 100);
    assert(setting_report(0x30, 0x18)[4] == 0x18);
    rejects([&] { integrate({1, 0, 0, 0}, {}, nan); });
    rejects([&] { integrate({nan, 0, 0, 0}, {}, .01); });
    rejects([&] { integrate({1, 0, 0, 0}, {infinity, 0, 0}, .01); });
    rejects([&] { SensorClock{}.add(0, nan); });
    rejects([&] { SensorClock{}.add(0, 1, 128); });
    PoseHistory history;
    rejects([&] { history.at(infinity); });
    rejects([&] { history.add(nan, std::nullopt); });
    rejects([&] { KeyboardMode{}.observe({nan, true, true, true}, 1); });
    rejects([&] { KeyboardMode{}.observe({1, false, true, true}, 1); });
    InertialPosition position;
    rejects([&] { position.sample(1, {100, 0, 0}, 0); });
    rejects([&] { position.sample(1, {}, nan); });
    rejects([&] { position.observe(1, {nan, 0, 0}, 1); });
    rejects([&] { json::parse("{\"valid\":true}junk"); });
    rejects([&] { json::parse(std::string("{\"text\":\"") + char(0xff) + "\"}"); });
    rejects([&] { json::parse("{}", 1); });
    auto json_value = json::parse("{\"number\":true,\"flag\":1}");
    rejects([&] { json::number(json::field(json_value.get(), "number")); });
    rejects([&] { json::boolean(json::field(json_value.get(), "flag")); });
    rejects([&] { json::field(json_value.get(), "missing"); });
    assert(json::encode(json_value.get()) == R"({"number":true,"flag":1})");
    rejects([&] { json::encode(json_value.get(), 1); });
    json_object_object_add(json_value.get(), "number", json_object_new_double(infinity));
    rejects([&] { json::encode(json_value.get()); });
    auto nested = json::parse("[]");
    auto parent = nested.get();
    for (unsigned i = 0; i < 65; ++i) {
        auto child = json_object_new_array();
        json_object_array_add(parent, child);
        parent = child;
    }
    rejects([&] { json::encode(nested.get()); });
    rejects([&] { bridge_input(0, 1, std::nullopt, {}, false); });
    auto input = bridge_input(10, 10.1, std::nullopt, {}, false);
    assert(wire::app_input(input, 10.1, 0, 10));
    InputControls confirmed;
    confirmed.activate = false;
    input = bridge_input(10, 10.1, std::nullopt, confirmed, true);
    assert(wire::app_input(input, 10.1, 0, 10));
    assert(input.flags == 5);
    for (auto flags : {4u, 8u}) {
        input.flags = flags;
        assert(!wire::app_input(input, 10.1, 0, 10));
    }
    input.flags = 3u | wire::TouchpadCursor;
    input.pose_time = input.time;
    assert(!wire::app_input(input, 10.1, 0, 10));
    input.buttons = 16u;
    assert(wire::app_input(input, 10.1, 0, 10));
    for (int i = 0; i < 10000; ++i) {
        const double time = 10 + i * .01;
        position.sample(time, {}, 0);
        if (i > 6 && i % 3 == 0)
            position.observe(time - .02, {0, 0, -.4}, time);
    }
    assert(position.history.size() <= 512 && position.observations.size() <= 24);
    assert(distance(*position.value(109.99), {0, 0, -.4}) < 1e-9);
    std::cout << "native core: boundary validation, wire format and bounded state passed\n";
}
