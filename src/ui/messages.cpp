#include "messages.h"

namespace ct::ui {
namespace {
bool boolean(json_object *object, const char *key) {
    auto value = field(object, key);
    if (json_object_get_type(value) != json_type_boolean)
        throw std::runtime_error(std::string("invalid UI flag: ") + key);
    return json_object_get_boolean(value);
}
std::string text(json_object *object, const char *key, size_t limit) {
    auto value = field(object, key);
    if (json_object_get_type(value) != json_type_string ||
        size_t(json_object_get_string_len(value)) > limit)
        throw std::runtime_error(std::string("invalid UI text: ") + key);
    return json_object_get_string(value);
}
template <size_t N>
std::array<float, N> input_array(json_object *object, const char *key, bool pad) {
    auto value = field(object, key);
    if (json_object_get_type(value) != json_type_array || json_object_array_length(value) != N)
        throw std::runtime_error("invalid controller input array");
    std::array<float, N> result{};
    for (size_t i = 0; i < N; ++i) {
        const double n = number(json_object_array_get_idx(value, i));
        if (n < (pad && i < 2 ? -1 : 0) || n > (pad && i == 2 ? 2 : 1))
            throw std::runtime_error("controller input outside range");
        result[i] = float(n);
    }
    return result;
}
} // namespace
std::optional<Command> command(json_object *object) {
    json_object *operation = nullptr;
    if (!object || json_object_get_type(object) != json_type_object ||
        !json_object_object_get_ex(object, "op", &operation) ||
        json_object_get_type(operation) != json_type_string)
        return {};
    const std::string op = json_object_get_string(operation);
    const auto fields = json_object_object_length(object);
    if (fields == 1) {
        if (op == "show_settings")
            return Operation::ShowSettings;
        if (op == "input_toggle")
            return Operation::InputToggle;
        if (op == "input_off")
            return Operation::InputOff;
        if (op == "hide")
            return Operation::Hide;
        if (op == "quit")
            return Operation::Quit;
    }
    if (op == "keyboard_typing" && fields == 2)
        return KeyboardTyping{boolean(object, "active")};
    if (op == "app_info" && (fields == 2 || fields == 3)) {
        AppInfo info{};
        info.version = text(object, "version", 32);
        if (fields == 3) {
            const auto debug = field(object, "debug");
            info.recording = boolean(debug, "recording");
            info.debug_available = boolean(debug, "available");
            info.debug_busy = boolean(debug, "busy");
            info.debug_message = text(debug, "message", 200);
        }
        return info;
    }
    if (op == "status") {
        const double light = number(field(object, "brightness"));
        const double blend = number(field(object, "smoothing_ms"));
        const double binding = number(field(object, "pointer_buttons"));
        if (binding <= 0 || binding > 0x0ccfffff || binding != int(binding) ||
            (uint32_t(binding) & ~0x0ccfffffu) || light < 0 || light > 100 || blend < 0 ||
            blend > 150)
            throw std::runtime_error("invalid control settings");
        Status result{text(object, "message", 240),
                      text(object, "pointer_label", 120),
                      boolean(object, "enabled"),
                      boolean(object, "binding_capture"),
                      boolean(object, "steam_shortcuts"),
                      boolean(object, "autostart"),
                      boolean(object, "pinned"),
                      light,
                      blend,
                      int(binding),
                      {},
                      {},
                      {}};
        for (const auto &entry : {std::pair{"gamepad_connected", &result.gamepad_connected},
                                  std::pair{"status_attention", &result.status_attention}}) {
            json_object *value = nullptr;
            if (json_object_object_get_ex(object, entry.first, &value))
                *entry.second = boolean(object, entry.first);
        }
        if (auto update = json::find(object, "update"))
            result.update =
                SoftwareUpdate{text(update, "message", 180), boolean(update, "automatic"),
                               boolean(update, "busy"), boolean(update, "ready")};
        return result;
    }
    if (op == "controls") {
        const double time = number(field(object, "time")), bits = number(field(object, "buttons"));
        if (time <= 0 || bits < 0 || bits > UINT32_MAX || bits != uint32_t(bits))
            throw std::runtime_error("invalid controller input time or buttons");
        return Controls{time,
                        uint32_t(bits),
                        input_array<2>(object, "triggers", false),
                        input_array<2>(object, "left_stick", true),
                        input_array<2>(object, "right_stick", true),
                        input_array<3>(object, "left_pad", true),
                        input_array<3>(object, "right_pad", true)};
    }
    if (op == "tracking_feedback") {
        Feedback result{boolean(object, "active"), {}, {}};
        if (fields != (result.active ? 6 : 2))
            throw std::runtime_error("invalid gamepad feedback command");
        if (result.active) {
            result.message = text(object, "message", 200);
            result.state = text(object, "state", 32);
            if (result.state != "connecting" && result.state != "palm" &&
                result.state != "realign" && result.state != "restart" && result.state != "ready" &&
                result.state != "error")
                throw std::runtime_error("unknown gamepad feedback phase");
            result.time = number(field(object, "time"));
            if (result.time <= 0)
                throw std::runtime_error("invalid gamepad feedback timestamp");
            const auto amount = field(object, "progress");
            if (json_object_get_type(amount) != json_type_null) {
                result.progress = number(amount);
                if (result.progress < 0 || result.progress > 1)
                    throw std::runtime_error("invalid gamepad feedback progress");
            }
        }
        return result;
    }
    if (op == "native_pose") {
        BodyPose pose;
        parse_body_pose(object, pose);
        return pose;
    }
    return {};
}
json::Value event_json(const Event &event) {
    json::Value result(json_object_new_object());
    const auto add = [&](const char *key, json_object *value) {
        json_object_object_add(result.get(), key, value);
    };
    std::visit(
        [&](const auto &value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, Action>) {
                add("op", json_object_new_string("action"));
                add("action", json_object_new_string(value.name.c_str()));
            } else if constexpr (std::is_same_v<T, PointerEnabled>) {
                add("op", json_object_new_string("pointer_enabled"));
                add("enabled", json_object_new_boolean(value.enabled));
            } else if constexpr (std::is_same_v<T, KeyboardState>) {
                add("op", json_object_new_string("keyboard_state"));
                add("time", json_object_new_double(value.time));
                add("visible", json_object_new_boolean(value.visible));
                add("gaze_valid", json_object_new_boolean(value.gaze_valid));
                add("gaze_on_keyboard", json_object_new_boolean(value.gaze_on_keyboard));
            } else if constexpr (std::is_same_v<T, OriginChanged>) {
                add("op", json_object_new_string("origin_changed"));
                auto transform = json_object_new_array();
                for (auto n : value.transform)
                    json_object_array_add(transform, json_object_new_double(n));
                add("transform", transform);
            } else if constexpr (std::is_same_v<T, Quit>) {
                add("op", json_object_new_string("action"));
                add("action", json_object_new_string("quit"));
            }
        },
        event);
    return result;
}
} // namespace ct::ui
