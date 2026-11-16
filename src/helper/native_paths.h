#pragma once

namespace ct::helper {
inline constexpr char TrackerDirectory[] = "/var/lib/controller-utils/native-tracker";
inline constexpr char TrackerLibrary[] =
    "/var/lib/controller-utils/native-tracker/libnative_tracker.so";
inline constexpr char TrackerStatusDirectory[] = "/run/controller-utils-native-tracker";
inline constexpr char TrackingLock[] = "/run/controller-utils-native-tracking.lock";
} // namespace ct::helper
