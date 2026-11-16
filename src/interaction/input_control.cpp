#include "input_control.h"
#include "gestures.h"
#include "platform/linux/resource.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace ct {
namespace {
std::string join(const std::vector<std::string> &errors) {
    std::string text;
    for (const auto &error : errors) {
        if (!text.empty())
            text += "; ";
        text += error;
    }
    return text;
}
bool fresh(const std::optional<double> &stamp, double now) {
    return stamp && -.02 <= now - *stamp && now - *stamp <= .15;
}
} // namespace
InputControl::InputControl(InputBackend &backend, std::string serial,
                           std::function<double()> monotonic, std::function<double()> raw)
    : backend_(backend), serial_(std::move(serial)),
      monotonic_(monotonic ? std::move(monotonic) : monotonic_time),
      raw_(raw ? std::move(raw) : raw_time) {}
void InputControl::controls(const std::optional<ControllerInput> &input, bool enabled,
                            bool typing) {
    if (enabled && typing)
        throw std::invalid_argument("invalid native input activation");
    if (input) {
        const double received = input->received;
        if (!std::isfinite(received))
            throw std::invalid_argument("invalid controller input timestamp");
        if (received - monotonic_() > .02)
            throw std::invalid_argument("controller input timestamp is in the future");
        const auto controls = input_controls(input->report);
        if (!state.received || received > *state.received) {
            state.input = controls;
            state.received = received;
            state.buttons = input->report.buttons;
            state.raw_received = received + raw_() - monotonic_();
        }
    }
    const bool was_enabled = state.enabled;
    state.enabled = enabled;
    state.typing = typing;
    if (!enabled)
        release(typing);
    else if (!input && !was_enabled && state.gate_ready && backend_.has_filter() &&
             backend_.filter_running())
        state.error.reset();
}
void InputControl::disable() {
    std::vector<std::string> errors;
    if (state.beam_hidden) {
        try {
            backend_.helper_command('v');
            state.beam_hidden = false;
        } catch (const std::exception &error) {
            errors.push_back(error.what());
        }
    }
    if (state.native_active) {
        try {
            if (backend_.release_controls())
                state.native_active = false;
        } catch (const std::exception &error) {
            errors.push_back(error.what());
        }
    }
    state.gate_confirmed.reset();
    state.gate_confirmed_index.reset();
    state.gate_epoch.reset();
    if (state.gate_index) {
        try {
            backend_.helper_command('-');
            state.gate_index.reset();
        } catch (const std::exception &error) {
            errors.push_back(error.what());
        }
    }
    if (!errors.empty())
        throw std::runtime_error(join(errors));
}
void InputControl::request_filter(bool enabled, double now, bool typing) {
    typing = typing && !state.error && fresh(state.received, now);
    const bool changed = enabled != state.filter_requested || typing != state.filter_typing;
    if (changed) {
        state.filter_requested = enabled;
        state.filter_typing = typing;
        state.filter_epoch = enabled ? std::optional<double>(now) : std::nullopt;
        state.filter_active = false;
        state.filter_confirmed.reset();
        state.filter_index.reset();
    }
    if (backend_.has_filter() && backend_.filter_running() &&
        (changed || now - state.last_filter_heartbeat >= .2)) {
        FilterRequest request;
        request.enabled = enabled;
        request.details = typing || enabled;
        request.typing = typing;
        if (state.input) {
            request.touches =
                unsigned(state.input->scroll_touch) | (unsigned(state.input->pad_touch) << 1);
            request.held = (state.buttons & ButtonMask) ||
                           std::max(state.input->trigger, state.input->grip) > .1;
        }
        backend_.send_filter(request);
        state.last_filter_heartbeat = now;
    }
}
std::vector<std::string> InputControl::release(bool typing) {
    std::vector<std::string> errors;
    state.typing = typing;
    try {
        disable();
    } catch (const std::exception &error) {
        errors.push_back(error.what());
    }
    try {
        request_filter(false, monotonic_(), typing);
    } catch (const std::exception &error) {
        errors.push_back(error.what());
    }
    if (!errors.empty())
        state.error = join(errors).substr(0, 300);
    return errors;
}
void InputControl::fail(const std::string &message) {
    release();
    state.error = message.empty() ? "Steam input control failed." : message.substr(0, 300);
}
bool InputControl::gate_ack(int index, double time) {
    if (index < -1 || index > 15 || !std::isfinite(time) || time < 0 || time - monotonic_() > .02) {
        fail("Invalid native input gate confirmation.");
        return false;
    }
    if (state.gate_confirmed && time < *state.gate_confirmed)
        return false;
    const bool reset = index >= 0 && (state.gate_confirmed_index != index ||
                                      !state.gate_confirmed || time - *state.gate_confirmed >= .75);
    state.gate_confirmed_index = index >= 0 ? std::optional<int>(index) : std::nullopt;
    state.gate_confirmed = time;
    return reset;
}
bool InputControl::poll(double now, bool tracking_ready) {
    const bool wanted = state.enabled && fresh(state.received, now) && tracking_ready &&
                        state.gate_ready && !state.error;
    try {
        if (!wanted)
            disable();
        request_filter(wanted, now, state.typing && fresh(state.received, now));
        if (!backend_.has_filter())
            return false;
        for (const auto &event : backend_.poll_filter()) {
            if (auto error = std::get_if<FilterError>(&event))
                throw std::runtime_error(error->message);
            const auto &ack = std::get<FilterState>(event);
            if ((ack.active ? ack.index < 0 || ack.index > 15 : ack.index != -1) ||
                ack.serial != serial_ || !std::isfinite(ack.time) || ack.time < 0 ||
                ack.time - now > .02)
                throw std::invalid_argument("Invalid Steam input filter confirmation.");
            if (state.filter_confirmed && ack.time < *state.filter_confirmed)
                continue;
            if (ack.keyboard_health != "off" && ack.keyboard_health != "starting" &&
                ack.keyboard_health != "ready" && ack.keyboard_health != "recovering")
                throw std::invalid_argument("Invalid Steam keyboard input health.");
            if (state.keyboard_health != ack.keyboard_health) {
                state.keyboard_health = ack.keyboard_health;
                std::cerr << "{\"op\":\"keyboard_input_health\",\"state\":\"" << ack.keyboard_health
                          << "\"}\n";
            }
            state.filter_active = ack.active && state.filter_requested && state.filter_epoch &&
                                  ack.time >= *state.filter_epoch;
            state.filter_index = state.filter_active ? std::optional<int>(ack.index) : std::nullopt;
            state.filter_confirmed = ack.time;
        }
        if (!backend_.filter_running())
            throw std::runtime_error(
                "Steam input filter stopped. Restart tracking to restore UI control.");
        const bool confirmed = wanted && state.filter_active && state.filter_confirmed &&
                               0 <= now - *state.filter_confirmed &&
                               now - *state.filter_confirmed <= .75;
        if (!confirmed) {
            disable();
            return false;
        }
        if (state.gate_index != state.filter_index)
            disable();
        if (state.gate_index != state.filter_index || now - state.last_gate_heartbeat >= .2) {
            if (state.gate_index != state.filter_index || now - state.last_gate_heartbeat >= .75) {
                state.gate_epoch = now;
                state.gate_confirmed.reset();
                state.gate_confirmed_index.reset();
            }
            backend_.helper_command('A' + *state.filter_index);
            state.gate_index = state.filter_index;
            state.last_gate_heartbeat = now;
        }
        return state.gate_confirmed_index == state.filter_index && state.gate_confirmed &&
               state.gate_epoch && *state.gate_confirmed >= *state.gate_epoch &&
               0 <= now - *state.gate_confirmed && now - *state.gate_confirmed <= .75;
    } catch (const std::exception &error) {
        fail(error.what());
        return false;
    }
}
void InputControl::update_beam(double now) {
    if (!state.beam_ready)
        return;
    const bool touch =
        state.native_active && state.input && state.input->pad_touch && fresh(state.received, now);
    if (!touch) {
        if (state.beam_hidden) {
            backend_.helper_command('v');
            state.beam_hidden = false;
        }
        return;
    }
    if (state.beam_hidden && now - state.last_beam_heartbeat < .2)
        return;
    if (!state.beam_device) {
        auto index = backend_.pointer_device("controllerutils-sc26-input-" + serial_);
        if (!index || *index >= 64)
            return;
        state.beam_device = index;
    }
    backend_.helper_command(0x80 + *state.beam_device);
    state.beam_hidden = true;
    state.last_beam_heartbeat = now;
}
} // namespace ct
