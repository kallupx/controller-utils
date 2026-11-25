#include "fake_openvr_interfaces.h"
#include "fake_openvr_overlay.h"
#include <ivrmailbox_001.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <array>
#include <cassert>
#include <unistd.h>
#include <openvr.h>

namespace {
unsigned init_count = 0, shutdown_count = 0, last_type = 0;
ct_test::System system_token;
ct_test::Applications applications_token;
ct_test::Overlay overlay_token;
struct Settings final : vr::IVRSettings {
    bool room_view = false;
    const char *GetSettingsErrorNameFromEnum(vr::EVRSettingsError) override {
        return "error";
    }
    bool GetBool(const char *section, const char *key, vr::EVRSettingsError *error) override {
        assert(std::strcmp(section, "camera") == 0 &&
               std::strcmp(key, "enableConstructRoomView") == 0);
        *error = std::getenv("CT_TEST_NO_HOME_CAMERA_SETTING")
                     ? vr::VRSettingsError_UnsetSettingHasNoDefault
                 : std::getenv("CT_TEST_CAMERA_SETTING_DENIED") ? vr::VRSettingsError_AccessDenied
                                                                : vr::VRSettingsError_None;
        return room_view;
    }
    void SetBool(const char *, const char *, bool, vr::EVRSettingsError *) override {}
    void SetInt32(const char *, const char *, int32_t, vr::EVRSettingsError *) override {}
    void SetFloat(const char *, const char *, float, vr::EVRSettingsError *) override {}
    void SetString(const char *, const char *, const char *, vr::EVRSettingsError *) override {}
    int32_t GetInt32(const char *, const char *, vr::EVRSettingsError *) override {
        return 0;
    }
    float GetFloat(const char *, const char *, vr::EVRSettingsError *) override {
        return 0;
    }
    void GetString(const char *, const char *, char *, uint32_t, vr::EVRSettingsError *) override {}
    void RemoveSection(const char *, vr::EVRSettingsError *) override {}
    void RemoveKeyInSection(const char *, const char *, vr::EVRSettingsError *) override {}
} settings_token;
struct Chaperone final : vr::IVRChaperone {
    unsigned resets = 0;
    vr::ChaperoneCalibrationState GetCalibrationState() override {
        return vr::ChaperoneCalibrationState_OK;
    }
    bool GetPlayAreaSize(float *, float *) override {
        return false;
    }
    bool GetPlayAreaRect(vr::HmdQuad_t *) override {
        return false;
    }
    void ReloadInfo() override {}
    void SetSceneColor(vr::HmdColor_t) override {}
    void GetBoundsColor(vr::HmdColor_t *, int, float, vr::HmdColor_t *) override {}
    bool AreBoundsVisible() override {
        return false;
    }
    void ForceBoundsVisible(bool) override {}
    void ResetZeroPose(vr::ETrackingUniverseOrigin origin) override {
        assert(origin == (resets % 2 ? vr::TrackingUniverseStanding : vr::TrackingUniverseSeated));
        ++resets;
    }
} chaperone;
struct Mailbox final : vr::IVRMailbox {
    bool open = false, scene_view = false;
    vr::vrmb_typeb undoc1(const char *name, vr::vrmb_typea *handle) override {
        assert(!open && std::strncmp(name, "controllerutils.", 16) == 0);
        open = true;
        *handle = 42;
        return vr::valuea;
    }
    vr::vrmb_typeb undoc2(vr::vrmb_typea handle) override {
        assert(open && handle == 42);
        open = false;
        return vr::valuea;
    }
    vr::vrmb_typeb undoc3(vr::vrmb_typea handle, const char *destination,
                          const char *message) override {
        assert(open && handle == 42);
        if (std::getenv("CT_TEST_MAILBOX_SEND_FAILURE"))
            return vr::vrmb_typeb(1);
        assert(!std::strcmp(destination, "vrcompositor_systemlayer"));
        if (!std::strcmp(message, R"({"type":"camera_room_view_toggle"})")) {
            scene_view = !scene_view;
        } else {
            if (!std::strcmp(message, R"({"type":"set_camera_roomview_enabled","enabled":true})"))
                settings_token.room_view = true;
            else {
                assert(!std::strcmp(message,
                                    R"({"type":"set_camera_roomview_enabled","enabled":false})"));
                settings_token.room_view = false;
            }
        }
        return vr::valuea;
    }
    vr::vrmb_typeb undoc4(vr::vrmb_typea, char *, uint32_t, uint32_t *) override {
        return vr::vrmb_typeb(1);
    }
} mailbox;
} // namespace
extern "C" uint32_t VR_InitInternal(vr::EVRInitError *error, vr::EVRApplicationType application) {
    ++init_count;
    last_type = unsigned(application);
    *error =
        std::getenv("CT_TEST_OPENVR_INIT_FAILURE") ? vr::EVRInitError(100) : vr::VRInitError_None;
    return 1;
}
extern "C" void VR_ShutdownInternal() {
    ++shutdown_count;
}
extern "C" void *VR_GetGenericInterface(const char *name, vr::EVRInitError *error) {
    if (std::getenv("CT_TEST_OPENVR_INTERFACE_FAILURE")) {
        *error = vr::EVRInitError(105);
        return nullptr;
    }
    *error = vr::VRInitError_None;
    if (!std::strcmp(name, "IVRSystem_026"))
        return &system_token;
    if (!std::strcmp(name, "IVROverlay_028"))
        return &overlay_token;
    if (!std::strcmp(name, "IVRApplications_008"))
        return &applications_token;
    if (!std::strcmp(name, vr::IVRSettings_Version))
        return &settings_token;
    if (!std::strcmp(name, "IVRMailbox_001"))
        return &mailbox;
    if (!std::strcmp(name, vr::IVRChaperone_Version))
        return &chaperone;
    *error = vr::EVRInitError(105);
    return nullptr;
}
extern "C" void ct_test_reset() {
    assert(!mailbox.open);
    mailbox.scene_view = false;
    chaperone.resets = 0;
    init_count = shutdown_count = last_type = 0;
    ct_test::identified = ct_test::startup_calls = ct_test::pose_calls = 0;
    ct_test::startup_enabled = false;
    settings_token.room_view = false;
    ct_test::keyboard_shown = ct_test::keyboard_positioned = 0;
    ct_test::next_overlay = ct_test::overlays_created = ct_test::overlays_destroyed =
        ct_test::textures_submitted = ct_test::overlays_shown = 0;
}
extern "C" bool ct_test_home_passthrough() {
    return settings_token.room_view;
}
extern "C" bool ct_test_scene_passthrough() {
    return mailbox.scene_view;
}
extern "C" unsigned ct_test_zero_pose_resets() {
    return chaperone.resets;
}
extern "C" unsigned ct_test_initialized() {
    return init_count;
}
extern "C" unsigned ct_test_shutdown() {
    return shutdown_count;
}
extern "C" unsigned ct_test_application_type() {
    return last_type;
}
extern "C" unsigned ct_test_identified() {
    return ct_test::identified;
}
extern "C" unsigned ct_test_startup_calls() {
    return ct_test::startup_calls;
}
extern "C" unsigned ct_test_startup_enabled() {
    return ct_test::startup_enabled;
}
extern "C" unsigned ct_test_pose_calls() {
    return ct_test::pose_calls;
}

extern "C" unsigned ct_test_overlays_created() {
    return ct_test::overlays_created;
}
extern "C" unsigned ct_test_overlays_destroyed() {
    return ct_test::overlays_destroyed;
}
extern "C" unsigned ct_test_textures_submitted() {
    return ct_test::textures_submitted;
}
extern "C" unsigned ct_test_overlays_shown() {
    return ct_test::overlays_shown;
}
extern "C" unsigned ct_test_keyboard_shown() {
    return ct_test::keyboard_shown;
}
extern "C" unsigned ct_test_keyboard_positioned() {
    return ct_test::keyboard_positioned;
}
