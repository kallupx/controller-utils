#pragma once
#include <cmath>
#include <cstdint>
#include <initializer_list>

namespace ct::wire {
constexpr uint32_t ControllerId = 16;
constexpr uint32_t TouchpadCursor = 8;
struct AppInput {
    uint32_t id, flags;
    double session_start, time, pose_time, orientation[4], position[3];
    float trigger, grip, stick_x, stick_y, pad_x, pad_y, scroll_x, scroll_y;
    uint32_t buttons, reserved;
};
static_assert(sizeof(AppInput) == 128);

inline bool fresh_input_time(double time, double now) {
    const double age = now - time;
    return time > 0 && age >= -.02 && age <= .15;
}

inline bool app_input(const AppInput &value, double now, double previous, double session_start) {
    if (value.id != ControllerId || value.flags & ~(7u | TouchpadCursor) ||
        ((value.flags & TouchpadCursor) && ((value.flags & 3u) != 3 || !(value.buttons & 16u))) ||
        ((value.flags & 4u) && !(value.flags & 1u)) || value.buttons & ~255u || value.reserved ||
        value.session_start != session_start || value.time < session_start ||
        value.time <= previous || !fresh_input_time(value.time, now))
        return false;
    if (value.flags & 2u) {
        if (!fresh_input_time(value.pose_time, value.time))
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
