#include "gestures.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace ct {
std::string binding_label(uint32_t buttons) {
    if (buttons == 0x20080)
        return "L4 + R4";
    std::string result;
    for (const auto &button : ButtonLabels) {
        if (!(buttons & button.mask))
            continue;
        if (!result.empty())
            result += " + ";
        result += button.label;
    }
    return result;
}
std::optional<uint32_t> BindingCapture::update(std::optional<uint32_t> buttons, double now) {
    if (now >= deadline)
        return 0;
    if (!buttons)
        return {};
    *buttons &= ButtonMask;
    if (!armed) {
        armed = *buttons == 0;
        return {};
    }
    if (*buttons && !started)
        started = now;
    pending |= *buttons;
    if (pending && (!*buttons || now - *started >= .2))
        return pending;
    return {};
}
std::vector<std::string> SteamShortcuts::update(uint32_t buttons, double now, bool enabled,
                                                bool keyboard, bool observed) {
    const bool current_down = buttons & 0x10000;
    const bool reset = !last_sample || now - *last_sample > .25;
    const bool chord = (buttons & 0x10004) == 0x10004;
    const bool screenshot = (buttons & 0x10200) == 0x10200;
    constexpr uint32_t CameraChord = 0x10000 | CameraSwitchButton;
    const bool camera = (buttons & CameraChord) == CameraChord;
    std::vector<std::string> actions;
    hold_enabled = enabled;
    if (!reset && keyboard && last_tap && now - *last_tap > .5 && !down) {
        actions.emplace_back("dashboard");
        last_tap.reset();
    }
    if (keyboard && chord && !keyboard_chord && !reset)
        actions.emplace_back("keyboard");
    keyboard_chord = chord;
    if (enabled && screenshot && !screenshot_chord && !reset)
        actions.emplace_back("screenshot");
    screenshot_chord = screenshot;
    if (!enabled || reset)
        camera_armed = false;
    if (enabled && observed) {
        if (camera && camera_armed && (buttons & ButtonMask) == CameraChord)
            actions.emplace_back("camera_switch");
        camera_armed = !camera;
    }
    if ((!enabled && !keyboard) || reset) {
        down = current_down;
        started.reset();
        last_tap.reset();
        fired = false;
        second_tap = false;
    }
    last_sample = now;
    if (!enabled && !keyboard)
        return actions;
    if (current_down && !down) {
        started = now;
        fired = false;
        second_tap = last_tap && now - *last_tap <= .5;
    }
    if ((current_down || down) && (buttons & (ButtonMask & ~0x10000u))) {
        started.reset();
        last_tap.reset();
        fired = true;
    }
    if (!current_down && down && started) {
        if (enabled && !fired && now - *started >= 1 && now - *started < 4) {
            actions.emplace_back("recenter");
            last_tap.reset();
        }
        if (!fired && now - *started < 1) {
            if (enabled && second_tap) {
                actions.emplace_back("passthrough");
                last_tap.reset();
            } else if (!enabled && keyboard)
                actions.emplace_back("dashboard");
            else
                last_tap = now;
        }
        started.reset();
    }
    down = current_down;
    return actions;
}
std::optional<double> SteamShortcuts::progress(double now) const {
    if (!hold_enabled || !down || !started || !last_sample || now - *last_sample < 0 ||
        now - *last_sample > .15 || fired)
        return {};
    const double elapsed = now - *started;
    return elapsed >= .5 && elapsed < 4 ? std::optional<double>(std::min(1., elapsed))
                                        : std::nullopt;
}
void KeyboardMode::observe(const KeyboardObservation &event, double now) {
    if (!std::isfinite(event.time) || event.time <= 0 || event.time - now > .02 ||
        (event.gaze_on_keyboard && !(event.visible && event.gaze_valid)))
        throw std::invalid_argument("Invalid native keyboard observation.");
    if (!observation || event.time > observation->time)
        observation = event;
}
bool KeyboardMode::update(const std::optional<ControllerInput> &input, bool enabled,
                          double received, double now) {
    const bool fresh =
        input && received - input->received >= -.02 && received - input->received <= .15;
    if (!enabled) {
        typing = false;
        released_at.reset();
        contact = fresh && (input->report.buttons & 0x02200000);
        return false;
    }
    if (!fresh)
        return typing;
    const bool touch = input->report.buttons & 0x02200000;
    const bool held = (input->report.buttons & ButtonMask) ||
                      std::max(input->report.triggers[0], input->report.triggers[1]) > .1;
    if (typing) {
        if (touch || held)
            released_at.reset();
        else if (!released_at)
            released_at = received;
        else if (received - *released_at >= .2) {
            typing = false;
            released_at.reset();
        }
    } else if (touch && !contact && !held) {
        typing = observation && now - observation->time >= -.02 && now - observation->time <= .2 &&
                 observation->gaze_on_keyboard;
    }
    contact = touch;
    return typing;
}
} // namespace ct
