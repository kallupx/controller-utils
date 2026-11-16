#pragma once
#include <filesystem>

namespace ct {
enum class TrackingBackend { Tracker };
TrackingBackend
installed_backend(const std::filesystem::path &helpers = "/var/lib/controller-utils");
} // namespace ct
