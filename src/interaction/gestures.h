#pragma once
#include "controller/report.h"
#include <optional>
#include <string>
#include <vector>

namespace ct {
struct ButtonLabel {
    uint32_t mask;
    const char *label;
};
inline constexpr ButtonLabel ButtonLabels[] = {{1, "A"},
                                               {2, "B"},
                                               {4, "X"},
                                               {8, "Y"},
                                               {0x10, "Quick Access"},
                                               {0x20, "R3"},
                                               {0x40, "View"},
                                               {0x80, "R4"},
                                               {0x100, "R5"},
                                               {0x200, "R1"},
                                               {0x400, "D-pad down"},
                                               {0x800, "D-pad right"},
                                               {0x1000, "D-pad left"},
                                               {0x2000, "D-pad up"},
                                               {0x4000, "Menu"},
                                               {0x8000, "L3"},
                                               {0x10000, "Steam"},
                                               {0x20000, "L4"},
                                               {0x40000, "L5"},
                                               {0x80000, "L1"},
                                               {0x400000, "Right pad"},
                                               {0x800000, "R2"},
                                               {0x4000000, "Left pad"},
                                               {0x8000000, "L2"}};
inline constexpr uint32_t ButtonMask = [] {
    uint32_t mask = 0;
    for (const auto &button : ButtonLabels)
        mask |= button.mask;
    return mask;
}();
std::string binding_label(uint32_t buttons);
inline constexpr uint32_t CameraSwitchButton = 0x80000;

class BindingCapture {
  public:
    double deadline;
    bool armed = false;
    uint32_t pending = 0;
    std::optional<double> started;
    explicit BindingCapture(double now) : deadline(now + 10) {}
    std::optional<uint32_t> update(std::optional<uint32_t> buttons, double now);
};

class SteamShortcuts {
  public:
    bool down = false, fired = false, keyboard_chord = false, screenshot_chord = false,
         camera_armed = false, second_tap = false;
    bool hold_enabled = false;
    std::optional<double> started, last_tap, last_sample;
    std::vector<std::string> update(uint32_t buttons, double now, bool enabled,
                                    bool keyboard = false, bool observed = true);
    std::optional<double> progress(double now) const;
};

struct KeyboardObservation {
    double time;
    bool visible, gaze_valid, gaze_on_keyboard;
};

class KeyboardMode {
  public:
    std::optional<KeyboardObservation> observation;
    bool contact = false, typing = false;
    std::optional<double> released_at;
    void observe(const KeyboardObservation &event, double now);
    bool update(const std::optional<ControllerInput> &input, bool enabled, double received,
                double now);
};
} // namespace ct
