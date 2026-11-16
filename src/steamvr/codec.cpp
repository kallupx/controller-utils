#include "codec.h"
#include "platform/json.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace ct {
InputControls input_controls(const ControllerReport &state) {
    const auto &triggers = state.triggers, &stick = state.right_stick;
    const auto &pad = state.right_pad, &scroll = state.left_pad;
    if (std::any_of(triggers.begin(), triggers.end(),
                    [](double v) { return !std::isfinite(v) || v < 0 || v > 1; }) ||
        std::any_of(stick.begin(), stick.end(),
                    [](double v) { return !std::isfinite(v) || std::abs(v) > 1; }) ||
        !finite(pad) || !finite(scroll) || std::abs(pad[0]) > 1 || std::abs(pad[1]) > 1 ||
        std::abs(scroll[0]) > 1 || std::abs(scroll[1]) > 1 || pad[2] < 0 || pad[2] >= 2 ||
        scroll[2] < 0 || scroll[2] >= 2)
        throw std::invalid_argument("invalid native controller input");
    const auto buttons = state.buttons;
    InputControls result;
    result.trigger = triggers[1];
    result.grip = triggers[0];
    result.stick_x = stick[0];
    result.stick_y = stick[1];
    result.pad_x = pad[0];
    result.pad_y = pad[1];
    result.pad_touch = buttons & 0x00200000;
    result.pad_click = buttons & 0x00400000;
    result.scroll_x = scroll[0];
    result.scroll_y = scroll[1];
    result.scroll_touch = buttons & 0x02000000;
    result.scroll_click = buttons & 0x04000000;
    result.back = buttons & 2;
    result.bumper = buttons & 0x200;
    result.stick_click = buttons & 0x20;
    result.stick_touch = (buttons & 0x20) || std::hypot(stick[0], stick[1]) > .08;
    return result;
}
std::string controller_profile(const std::string &serial) {
    if (serial.empty() || std::any_of(serial.begin(), serial.end(), [](unsigned char c) {
            return !((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'));
        }))
        throw std::invalid_argument("invalid controller serial");
    return R"({"device_class":"controller","device_serial_number":"controllerutils-sc26-)" +
           serial +
           R"(","manufacturer":"Valve","model_number":"ibex_ev1","revision":2,"tracked_controller_role":"right_hand","imu":{"plus_x":[1,0,0],"plus_z":[0,0,1],"position":[0,0,0]},"lighthouse_config":{"modelPoints":[[0,0,0]],"modelNormals":[[0,0,1]]}})";
}
wire::Command bridge_event(bool connected, uint64_t hardware_id, const std::string &onboard,
                           const std::string &defaults) {
    if (onboard.size() >= 12288 || defaults.size() >= 12288)
        throw std::invalid_argument("controller configuration exceeds event capacity");
    auto first = json::parse(onboard, 12287), second = json::parse(defaults, 12287);
    if (json_object_get_type(first.get()) != json_type_object ||
        json_object_get_type(second.get()) != json_type_object)
        throw std::invalid_argument("controller configurations must be objects");
    auto first_serial = json::find(first.get(), "device_serial_number");
    auto second_serial = json::find(second.get(), "device_serial_number");
    if (!first_serial || !second_serial || json_object_get_type(first_serial) != json_type_string ||
        json_object_get_type(second_serial) != json_type_string)
        throw std::invalid_argument("configurations need the same bounded serial number");
    const auto serial = json::string(first_serial);
    if (serial.empty() || serial.find('\0') != std::string::npos || serial.size() > 96 ||
        serial != json::string(second_serial))
        throw std::invalid_argument("configurations need the same bounded serial number");
    wire::Command command{};
    command.magic = wire::Magic;
    command.kind = 4;
    command.size = wire::EventSize;
    command.onboard_size = uint32_t(onboard.size());
    command.default_size = uint32_t(defaults.size());
    const wire::EventHeader header{wire::ControllerId, uint32_t(connected), hardware_id};
    std::memcpy(command.data, &header, sizeof(header));
    std::memcpy(command.data + 16, onboard.data(), onboard.size());
    std::memcpy(command.data + 16 + 12288, defaults.data(), defaults.size());
    return command;
}
wire::Command bridge_imu(double timestamp, const Vector3 &acceleration,
                         const Vector3 &angular_velocity) {
    if (!std::isfinite(timestamp) || !finite(acceleration) || !finite(angular_velocity))
        throw std::invalid_argument("invalid bridge IMU sample");
    for (const auto &values : {acceleration, angular_velocity})
        for (double value : values)
            if (std::abs(value) > std::numeric_limits<float>::max())
                throw std::out_of_range("float too large to pack with f format");
    wire::Imu sample{};
    sample.id = wire::ControllerId;
    sample.time = timestamp;
    for (size_t i = 0; i < 3; ++i) {
        sample.acceleration[i] = float(acceleration[i]);
        sample.gyro[i] = float(angular_velocity[i]);
    }
    wire::Command command{};
    command.magic = wire::Magic;
    command.kind = 2;
    command.size = sizeof(sample);
    std::memcpy(command.data, &sample, sizeof(sample));
    return command;
}
wire::Command bridge_input(double session_start, double timestamp,
                           const std::optional<BodyPoseMessage> &pose,
                           const InputControls &controls, bool enabled) {
    const double pose_time = pose ? pose->time : 0;
    const Quaternion orientation = pose ? pose->orientation : Quaternion{1, 0, 0, 0};
    const Vector3 position = pose ? pose->position : Vector3{};
    const std::array<double, 8> analog{controls.trigger,  controls.grip,    controls.stick_x,
                                       controls.stick_y,  controls.pad_x,   controls.pad_y,
                                       controls.scroll_x, controls.scroll_y};
    if (!std::isfinite(session_start) || !std::isfinite(timestamp) || !std::isfinite(pose_time) ||
        !finite(orientation) || !finite(position) || session_start <= 0 ||
        timestamp < session_start || std::abs(dot(orientation, orientation) - 1) > .001 ||
        std::any_of(position.begin(), position.end(), [](double v) { return std::abs(v) > 20; }) ||
        std::any_of(analog.begin(), analog.end(), [](double v) { return !std::isfinite(v); }) ||
        analog[0] < 0 || analog[0] > 1 || analog[1] < 0 || analog[1] > 1 ||
        std::any_of(analog.begin() + 2, analog.end(), [](double v) { return std::abs(v) > 1; }) ||
        (pose && (timestamp - pose_time < -.02 || timestamp - pose_time > .15)))
        throw std::invalid_argument("invalid native input state");
    wire::AppInput input{};
    input.id = wire::ControllerId;
    input.flags = unsigned(enabled) | (pose ? 2u : 0u);
    input.session_start = session_start;
    input.time = timestamp;
    input.pose_time = pose_time;
    std::copy(orientation.begin(), orientation.end(), input.orientation);
    std::copy(position.begin(), position.end(), input.position);
    input.trigger = float(controls.trigger);
    input.grip = float(controls.grip);
    input.stick_x = float(controls.stick_x);
    input.stick_y = float(controls.stick_y);
    input.pad_x = float(controls.pad_x);
    input.pad_y = float(controls.pad_y);
    input.scroll_x = float(controls.scroll_x);
    input.scroll_y = float(controls.scroll_y);
    const std::array<bool, 8> buttons{
        controls.back,      controls.bumper,    controls.stick_touch,  controls.stick_click,
        controls.pad_touch, controls.pad_click, controls.scroll_touch, controls.scroll_click};
    for (size_t i = 0; i < buttons.size(); ++i)
        if (buttons[i])
            input.buttons |= 1u << i;
    wire::Command command{};
    command.magic = wire::Magic;
    command.kind = 3;
    command.size = sizeof(input);
    std::memcpy(command.data, &input, sizeof(input));
    return command;
}
wire::Status decode_status(const uint8_t *data, size_t size) {
    if (size != sizeof(wire::Status))
        throw std::invalid_argument("incorrect bridge status size");
    wire::Status result;
    std::memcpy(&result, data, sizeof(result));
    if (result.magic != wire::Magic || (result.version != 2 && result.version != 3) ||
        !std::isfinite(result.last_input) || !std::isfinite(result.last_pose) ||
        !std::isfinite(result.session_start))
        throw std::invalid_argument("invalid bridge status");
    return result;
}
NativePoseInput decode_pose(const uint8_t *data, size_t size) {
    if (size != sizeof(wire::PoseBlock))
        throw std::invalid_argument("incorrect pose block size");
    wire::PoseBlock block;
    std::memcpy(&block, data, sizeof(block));
    if (!std::isfinite(block.timestamp))
        throw std::invalid_argument("non-finite pose timestamp");
    NativePoseInput result;
    result.controller_id = block.controller_id;
    result.timestamp = block.timestamp;
    if (block.timestamp < 0)
        return result;
    for (const auto *values : {block.orientation, block.position, block.velocity,
                               block.angular_velocity, block.acceleration}) {
        const size_t count = values == block.orientation ? 4 : 3;
        for (size_t i = 0; i < count; ++i)
            if (!std::isfinite(values[i]))
                throw std::invalid_argument("non-finite pose data");
    }
    Quaternion q{};
    Vector3 position{};
    std::copy_n(block.orientation, 4, q.begin());
    std::copy_n(block.position, 3, position.begin());
    if (std::abs(dot(q, q) - 1) > .02)
        throw std::invalid_argument("invalid pose quaternion");
    result.valid = true;
    result.orientation = q;
    result.position = position;
    std::copy_n(block.velocity, 3, result.velocity.begin());
    std::copy_n(block.angular_velocity, 3, result.angular_velocity.begin());
    std::copy_n(block.acceleration, 3, result.acceleration.begin());
    return result;
}
} // namespace ct
