#pragma once
#include <json-c/json.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace ct::wire {
constexpr uint32_t Magic = 0x31525443, ControllerId = 16, EventSize = 24592;
struct Command {
    uint32_t magic, kind, size, onboard_size, default_size, reserved;
    unsigned char data[EventSize];
};
struct EventHeader {
    uint32_t id, connected;
    uint64_t hardware;
};
struct Imu {
    uint32_t id, padding;
    double time;
    float acceleration[3], gyro[3];
    uint32_t extra[2];
};
struct AppInput {
    uint32_t id, flags;
    double session_start, time, pose_time, orientation[4], position[3];
    float trigger, grip, stick_x, stick_y, pad_x, pad_y, scroll_x, scroll_y;
    uint32_t buttons, reserved;
};
struct Status {
    uint32_t magic = Magic, version = 3, connected = 0;
    int32_t error = 0;
    uint32_t last_kind = 0, input_count = 0, imu_count = 0, dropped = 0;
    double last_input = 0, last_pose = 0, session_start = 0;
    uint32_t native_ready = 0, pose_controller_id = 0;
};
struct PoseBlock {
    uint32_t controller_id, padding;
    double timestamp, orientation[4], position[3], velocity[3], angular_velocity[3],
        acceleration[3];
};
static_assert(sizeof(Command) == 24616 && sizeof(EventHeader) == 16);
static_assert(sizeof(Imu) == 48 && sizeof(Status) == 64);
static_assert(sizeof(AppInput) == 128);
static_assert(sizeof(PoseBlock) == 144);

inline json_object *field(json_object *object, const char *name) {
    json_object *value = nullptr;
    if (object)
        json_object_object_get_ex(object, name, &value);
    return value;
}
inline bool finite_json(json_object *object) {
    if (!object)
        return true;
    if (json_object_is_type(object, json_type_double))
        return std::isfinite(json_object_get_double(object));
    if (json_object_is_type(object, json_type_array)) {
        for (size_t i = 0; i < json_object_array_length(object); ++i)
            if (!finite_json(json_object_array_get_idx(object, i)))
                return false;
    }
    if (json_object_is_type(object, json_type_object)) {
        json_object_object_foreach(object, key, value) {
            (void)key;
            if (!finite_json(value))
                return false;
        }
    }
    return true;
}
inline bool vector3(json_object *object, double *out, bool unit = false) {
    if (!object || !json_object_is_type(object, json_type_array) ||
        json_object_array_length(object) != 3)
        return false;
    double norm = 0;
    for (unsigned i = 0; i < 3; ++i) {
        auto *value = json_object_array_get_idx(object, i);
        if (!value || (!json_object_is_type(value, json_type_double) &&
                       !json_object_is_type(value, json_type_int)))
            return false;
        out[i] = json_object_get_double(value);
        if (!std::isfinite(out[i]) || std::abs(out[i]) > 1)
            return false;
        norm += out[i] * out[i];
    }
    return !unit || std::abs(norm - 1) < .02;
}
inline bool config(const char *data, uint32_t size, bool is_default, std::string &serial) {
    if (!size || size >= 12288 || data[size] || std::memchr(data, 0, size))
        return false;
    auto *tokener = json_tokener_new_ex(32);
    if (!tokener)
        return false;
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    auto *root = json_tokener_parse_ex(tokener, data, int(size));
    bool valid = json_tokener_get_error(tokener) == json_tokener_success && root &&
                 json_tokener_get_parse_end(tokener) == size &&
                 json_object_is_type(root, json_type_object) && finite_json(root);
    if (valid) {
        auto *value = field(root, "device_serial_number");
        valid = value && json_object_is_type(value, json_type_string);
        if (valid) {
            const char *text = json_object_get_string(value);
            valid =
                json_object_get_string_len(value) <= 96 &&
                size_t(json_object_get_string_len(value)) == std::strlen(text) &&
                !std::strncmp(text, "controllerutils-sc26-", sizeof("controllerutils-sc26-") - 1);
            if (valid)
                serial = text;
        }
        auto *model = field(root, "model_number");
        valid = valid && model && json_object_is_type(model, json_type_string) &&
                !std::strcmp(json_object_get_string(model), "ibex_ev1");
        auto *device_class = field(root, "device_class");
        valid = valid && device_class && json_object_is_type(device_class, json_type_string) &&
                !std::strcmp(json_object_get_string(device_class), "controller");
        auto *imu = field(root, "imu");
        double x[3], z[3], position[3];
        valid = valid && vector3(field(imu, "plus_x"), x, true) &&
                vector3(field(imu, "plus_z"), z, true) && vector3(field(imu, "position"), position);
        if (valid)
            valid = std::abs(x[0] * z[0] + x[1] * z[1] + x[2] * z[2]) < .02;
        if (valid && is_default) {
            auto *leds = field(root, "lighthouse_config");
            auto *points = field(leds, "modelPoints");
            auto *normals = field(leds, "modelNormals");
            valid = points && normals && json_object_is_type(points, json_type_array) &&
                    json_object_is_type(normals, json_type_array) &&
                    json_object_array_length(points) == 1 &&
                    json_object_array_length(normals) == 1 &&
                    vector3(json_object_array_get_idx(points, 0), position) &&
                    vector3(json_object_array_get_idx(normals, 0), z, true);
        }
    }
    if (root)
        json_object_put(root);
    json_tokener_free(tokener);
    return valid;
}
inline bool event(const Command &command, std::string &serial) {
    if (command.magic != Magic || command.reserved || command.kind != 4 ||
        command.size != EventSize)
        return false;
    EventHeader header;
    std::memcpy(&header, command.data, sizeof(header));
    if (header.id != ControllerId || header.connected > 1)
        return false;
    std::string other;
    return config(reinterpret_cast<const char *>(command.data + 16), command.onboard_size, false,
                  serial) &&
           config(reinterpret_cast<const char *>(command.data + 0x3010), command.default_size, true,
                  other) &&
           serial == other;
}
inline bool imu(const Command &command, double now, double previous) {
    if (command.magic != Magic || command.reserved || command.kind != 2 ||
        command.size != sizeof(Imu) || command.onboard_size || command.default_size)
        return false;
    Imu value;
    std::memcpy(&value, command.data, sizeof(value));
    if (value.id != ControllerId || value.padding || value.extra[0] || value.extra[1] ||
        !std::isfinite(value.time) || value.time <= previous || std::abs(now - value.time) > .5)
        return false;
    for (float x : value.acceleration)
        if (!std::isfinite(x) || std::abs(x) > 2000)
            return false;
    for (float x : value.gyro)
        if (!std::isfinite(x) || std::abs(x) > 100)
            return false;
    return true;
}
inline bool app_input(const Command &command, double now, double previous, double session_start) {
    if (command.magic != Magic || command.reserved || command.kind != 3 ||
        command.size != sizeof(AppInput) || command.onboard_size || command.default_size)
        return false;
    AppInput value;
    std::memcpy(&value, command.data, sizeof(value));
    if (value.id != ControllerId || value.flags & ~3u || value.buttons & ~255u || value.reserved ||
        value.session_start != session_start || !std::isfinite(value.time) ||
        value.time < session_start || value.time <= previous || now - value.time < -.02 ||
        now - value.time > .15 || !std::isfinite(value.pose_time))
        return false;
    if (value.flags & 2u) {
        if (value.pose_time <= 0 || value.time - value.pose_time < -.02 ||
            value.time - value.pose_time > .15)
            return false;
    } else if (value.pose_time != 0)
        return false;
    double norm = 0;
    for (double x : value.orientation) {
        if (!std::isfinite(x))
            return false;
        norm += x * x;
    }
    if (std::abs(norm - 1) > .001)
        return false;
    for (double x : value.position)
        if (!std::isfinite(x) || std::abs(x) > 20)
            return false;
    for (float x : {value.trigger, value.grip})
        if (!std::isfinite(x) || x < 0 || x > 1)
            return false;
    for (float x :
         {value.stick_x, value.stick_y, value.pad_x, value.pad_y, value.scroll_x, value.scroll_y})
        if (!std::isfinite(x) || std::abs(x) > 1)
            return false;
    return true;
}
} // namespace ct::wire
