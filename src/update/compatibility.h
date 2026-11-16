#pragma once
#include "metadata.h"
#include "steamvr/compatibility.h"

namespace ct::update {
struct Installation {
    std::filesystem::path application;
    std::filesystem::path helpers = "/var/lib/controller-utils";
    std::array<std::filesystem::path, RuntimeFiles.size()> runtime{
        steamvr::compatibility::XrService.path};
};
struct InstalledRequirements {
    std::string machine;
    json::Value value;
};
InstalledRequirements capture_requirements(const Installation &installation,
                                           TrackingBackend active);
void check_requirements(json_object *expected, const InstalledRequirements &installed);
void check_payload(const std::filesystem::path &stage, json_object *expected);
} // namespace ct::update
