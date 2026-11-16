#pragma once
#include "platform/json.h"
#include "tracking/backend.h"
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ct::update {
inline constexpr char GitHubReleases[] = "https://github.com/kallupx/controller-utils/releases/";
inline constexpr char GitHubChannel[] =
    "https://github.com/kallupx/controller-utils/releases/latest/download/";
inline constexpr size_t MaxArchive = 128 * 1024 * 1024;
inline constexpr size_t MaxContents = 256 * 1024 * 1024;
inline constexpr size_t MaxMetadata = 65536;
inline constexpr std::array<const char *, 5> DriverFiles{
    "driver.vrdrivermanifest", "bin/linuxarm64/driver_controllerutils_native.so",
    "resources/input/sc26_profile.json", "resources/input/vrcompositor_bindings_sc26.json",
    "resources/rendermodels/sc26_input/sc26_input.obj"};
inline constexpr std::array<const char *, 1> RuntimeFiles{"XRService"};
inline std::string backend_name(TrackingBackend backend) {
    if (backend == TrackingBackend::Tracker)
        return "native-tracker";
    throw std::invalid_argument("Invalid native tracking backend");
}
inline TrackingBackend backend_from_name(const std::string &name) {
    if (name == "native-tracker")
        return TrackingBackend::Tracker;
    throw std::invalid_argument("Invalid native tracking backend");
}
std::vector<std::string> helper_files(TrackingBackend backend);
void validate_requirements(json_object *value);

struct Release {
    std::string version;
    uint64_t sequence;
    static Release from_json(json_object *value);
    json::Value to_json() const;
    bool operator==(const Release &other) const;
};
Release release(const std::filesystem::path &root);
std::string release_url(const std::string &channel, const Release &release);
std::string https_url(const std::string &url);
std::string fixed_https_directory(const std::string &url);
std::optional<std::string> channel(const std::filesystem::path &root);
void validate_public_key(const std::filesystem::path &path);
void verify_signed_bytes(const std::string &contents, const std::string &signature,
                         const std::string &public_key);

struct Metadata {
    Release release;
    uint64_t expires, size;
    std::string artifact, sha256, notes;
    json::Value requirements;
    static Metadata from_json(json_object *value, uint64_t minimum, double now);
};
Metadata verify_metadata_key(const std::filesystem::path &metadata,
                             const std::filesystem::path &signature, const std::string &public_key,
                             uint64_t minimum = 0, std::optional<double> now = std::nullopt);
Metadata verify_metadata(const std::filesystem::path &metadata,
                         const std::filesystem::path &signature,
                         const std::filesystem::path &public_key, uint64_t minimum = 0,
                         std::optional<double> now = std::nullopt);
} // namespace ct::update
