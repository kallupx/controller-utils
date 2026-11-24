#pragma once
#include "body_pose.h"
#include "platform/json.h"
#include <functional>
#include <variant>
#include <vector>

namespace ct::ui {
enum class Operation { ShowSettings, InputToggle, InputOff, Hide, Quit };
struct KeyboardTyping {
    bool active;
};
struct SoftwareUpdate {
    std::string message;
    bool automatic, busy, ready;
};
struct Status {
    std::string message, pointer_label;
    bool enabled, binding_capture, steam_shortcuts, autostart, pinned;
    double brightness, smoothing;
    int pointer_buttons;
    std::optional<bool> gamepad_connected, status_attention;
    std::optional<SoftwareUpdate> update;
};
struct AppInfo {
    std::string version;
    bool recording = false, debug_available = false, debug_busy = false;
    std::string debug_message;
};
struct Controls {
    double time;
    uint32_t buttons;
    std::array<float, 2> triggers, left_stick, right_stick;
    std::array<float, 3> left_pad, right_pad;
};
struct Feedback {
    bool active;
    std::string state, message;
    double time = 0, progress = -1;
};
using Command =
    std::variant<Operation, KeyboardTyping, Status, AppInfo, Controls, Feedback, BodyPose>;
std::optional<Command> command(json_object *object);

struct Action {
    std::string name;
};
struct PointerEnabled {
    bool enabled;
};
struct KeyboardState {
    double time;
    bool visible, gaze_valid, gaze_on_keyboard;
};
struct OriginChanged {
    std::array<float, 12> transform;
};
struct Quit {};
using Event = std::variant<Action, PointerEnabled, KeyboardState, OriginChanged, Quit>;
using EventSink = std::function<void(Event)>;
json::Value event_json(const Event &event);
} // namespace ct::ui
