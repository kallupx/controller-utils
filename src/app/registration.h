#pragma once
#include <filesystem>
#include <openvr.h>

namespace ct {
inline constexpr char ApplicationKey[] = "org.controllerutils.frame";
class ApplicationRegistration {
    vr::IVRApplications &api_;

  public:
    explicit ApplicationRegistration(vr::IVRApplications &api) : api_(api) {}
    static void prepare(const std::filesystem::path &application,
                        const std::filesystem::path &home);
    void identify();
    void autostart(bool enabled);
    void install(const std::filesystem::path &application, const std::filesystem::path &home,
                 bool start_automatically);
    void remove(const std::filesystem::path &application, const std::filesystem::path &home);
};
} // namespace ct
