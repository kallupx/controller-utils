#pragma once
#include <openvr.h>
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <unistd.h>

namespace ct_test {
using namespace vr;
inline std::atomic<unsigned> identified{0}, startup_calls{0}, pose_calls{0};
inline bool startup_enabled = false;
struct System : IVRSystem {
    void GetRecommendedRenderTargetSize(uint32_t *pnWidth, uint32_t *pnHeight) override {}
    HmdMatrix44_t GetProjectionMatrix(EVREye eEye, float fNearZ, float fFarZ) override {
        return {};
    }
    void GetProjectionRaw(EVREye eEye, float *pfLeft, float *pfRight, float *pfTop,
                          float *pfBottom) override {}
    bool ComputeDistortion(EVREye eEye, float fU, float fV,
                           DistortionCoordinates_t *pDistortionCoordinates) override {
        return {};
    }
    bool ComputeDistortionSet(EVREye eEye, EVRDistortionChannel eChannel,
                              bool bAsNormalizedDeviceCoordinates, uint32_t nNumCoordinates,
                              const DistortionCoordinate_t *pInput,
                              DistortionCoordinate_t *pOutput) override {
        return {};
    }
    HmdMatrix34_t GetEyeToHeadTransform(EVREye eEye) override {
        HmdMatrix34_t pose{};
        for (int i = 0; i < 3; ++i)
            pose.m[i][i] = 1;
        pose.m[0][3] = eEye == Eye_Left ? -.0325f : .0325f;
        return pose;
    }
    bool GetTimeSinceLastVsync(float *pfSecondsSinceLastVsync, uint64_t *pulFrameCounter) override {
        return {};
    }
    int32_t GetD3D9AdapterIndex() override {
        return {};
    }
    void GetDXGIOutputInfo(int32_t *pnAdapterIndex) override {}
    void GetOutputDevice(uint64_t *pnDevice, ETextureType textureType,
                         VkInstance_T *pInstance = nullptr) override {}
    bool IsDisplayOnDesktop() override {
        return {};
    }
    bool SetDisplayVisibility(bool bIsVisibleOnDesktop) override {
        return {};
    }
    void GetDeviceToAbsoluteTrackingPose(ETrackingUniverseOrigin eOrigin,
                                         float fPredictedSecondsToPhotonsFromNow,
                                         VR_ARRAY_COUNT(unTrackedDevicePoseArrayCount)
                                             TrackedDevicePose_t *pTrackedDevicePoseArray,
                                         uint32_t unTrackedDevicePoseArrayCount) override {
        ++pose_calls;
        std::memset(pTrackedDevicePoseArray, 0,
                    sizeof(*pTrackedDevicePoseArray) * unTrackedDevicePoseArrayCount);
        if (std::getenv("CT_TEST_OPENVR_HEAD_VALID") && unTrackedDevicePoseArrayCount) {
            auto &pose = pTrackedDevicePoseArray[0];
            pose.bDeviceIsConnected = pose.bPoseIsValid = true;
            pose.eTrackingResult = TrackingResult_Running_OK;
            for (int i = 0; i < 3; ++i)
                pose.mDeviceToAbsoluteTracking.m[i][i] = 1;
        }
    }
    HmdMatrix34_t GetSeatedZeroPoseToStandingAbsoluteTrackingPose() override {
        return {};
    }
    HmdMatrix34_t GetRawZeroPoseToStandingAbsoluteTrackingPose() override {
        HmdMatrix34_t pose{};
        for (int i = 0; i < 3; ++i)
            pose.m[i][i] = 1;
        return pose;
    }
    uint32_t
    GetSortedTrackedDeviceIndicesOfClass(ETrackedDeviceClass eTrackedDeviceClass,
                                         VR_ARRAY_COUNT(unTrackedDeviceIndexArrayCount)
                                             vr::TrackedDeviceIndex_t *punTrackedDeviceIndexArray,
                                         uint32_t unTrackedDeviceIndexArrayCount,
                                         vr::TrackedDeviceIndex_t unRelativeToTrackedDeviceIndex =
                                             k_unTrackedDeviceIndex_Hmd) override {
        return {};
    }
    EDeviceActivityLevel
    GetTrackedDeviceActivityLevel(vr::TrackedDeviceIndex_t unDeviceId) override {
        return {};
    }
    void ApplyTransform(TrackedDevicePose_t *pOutputPose,
                        const TrackedDevicePose_t *pTrackedDevicePose,
                        const HmdMatrix34_t *pTransform) override {}
    vr::TrackedDeviceIndex_t
    GetTrackedDeviceIndexForControllerRole(vr::ETrackedControllerRole unDeviceType) override {
        return {};
    }
    vr::ETrackedControllerRole
    GetControllerRoleForTrackedDeviceIndex(vr::TrackedDeviceIndex_t unDeviceIndex) override {
        return {};
    }
    ETrackedDeviceClass GetTrackedDeviceClass(vr::TrackedDeviceIndex_t unDeviceIndex) override {
        return {};
    }
    bool IsTrackedDeviceConnected(vr::TrackedDeviceIndex_t unDeviceIndex) override {
        return {};
    }
    bool GetBoolTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex,
                                      ETrackedDeviceProperty prop,
                                      ETrackedPropertyError *pError = 0L) override {
        return {};
    }
    float GetFloatTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex,
                                        ETrackedDeviceProperty prop,
                                        ETrackedPropertyError *pError = 0L) override {
        return {};
    }
    int32_t GetInt32TrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex,
                                          ETrackedDeviceProperty prop,
                                          ETrackedPropertyError *pError = 0L) override {
        return {};
    }
    uint64_t GetUint64TrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex,
                                            ETrackedDeviceProperty prop,
                                            ETrackedPropertyError *pError = 0L) override {
        return {};
    }
    HmdMatrix34_t GetMatrix34TrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex,
                                                   ETrackedDeviceProperty prop,
                                                   ETrackedPropertyError *pError = 0L) override {
        return {};
    }
    uint32_t GetArrayTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex,
                                           ETrackedDeviceProperty prop, PropertyTypeTag_t propType,
                                           void *pBuffer, uint32_t unBufferSize,
                                           ETrackedPropertyError *pError = 0L) override {
        return {};
    }
    uint32_t GetStringTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex,
                                            ETrackedDeviceProperty prop,
                                            VR_OUT_STRING() char *pchValue, uint32_t unBufferSize,
                                            ETrackedPropertyError *pError = 0L) override {
        return {};
    }
    const char *GetPropErrorNameFromEnum(ETrackedPropertyError error) override {
        return {};
    }
    bool PollNextEvent(VREvent_t *pEvent, uint32_t uncbVREvent) override {
        return {};
    }
    bool PollNextEventWithPose(ETrackingUniverseOrigin eOrigin, VREvent_t *pEvent,
                               uint32_t uncbVREvent,
                               vr::TrackedDevicePose_t *pTrackedDevicePose) override {
        return {};
    }
    bool PollNextEventWithPoseAndOverlays(vr::ETrackingUniverseOrigin eOrigin, VREvent_t *pEvent,
                                          uint32_t uncbVREvent,
                                          TrackedDevicePose_t *pTrackedDevicePose,
                                          VROverlayHandle_t *pulOverlayHandle) override {
        return {};
    }
    const char *GetEventTypeNameFromEnum(EVREventType eType) override {
        return {};
    }
    HiddenAreaMesh_t
    GetHiddenAreaMesh(EVREye eEye, EHiddenAreaMeshType type = k_eHiddenAreaMesh_Standard) override {
        return {};
    }
    bool GetEyeTrackedFoveationCenter(HmdVector2_t *pNdcLeft, HmdVector2_t *pNdcRight) override {
        return {};
    }
    bool GetEyeTrackedFoveationCenterForProjection(const HmdMatrix44_t *pProjMat,
                                                   HmdVector2_t *pNdc) override {
        return {};
    }
    bool GetControllerState(vr::TrackedDeviceIndex_t unControllerDeviceIndex,
                            vr::VRControllerState_t *pControllerState,
                            uint32_t unControllerStateSize) override {
        return {};
    }
    bool GetControllerStateWithPose(ETrackingUniverseOrigin eOrigin,
                                    vr::TrackedDeviceIndex_t unControllerDeviceIndex,
                                    vr::VRControllerState_t *pControllerState,
                                    uint32_t unControllerStateSize,
                                    TrackedDevicePose_t *pTrackedDevicePose) override {
        return {};
    }
    void TriggerHapticPulse(vr::TrackedDeviceIndex_t unControllerDeviceIndex, uint32_t unAxisId,
                            unsigned short usDurationMicroSec) override {}
    const char *GetButtonIdNameFromEnum(EVRButtonId eButtonId) override {
        return {};
    }
    const char *GetControllerAxisTypeNameFromEnum(EVRControllerAxisType eAxisType) override {
        return {};
    }
    bool IsInputAvailable() override {
        return {};
    }
    bool IsSteamVRDrawingControllers() override {
        return {};
    }
    bool ShouldApplicationPause() override {
        return {};
    }
    bool ShouldApplicationReduceRenderingWork() override {
        return {};
    }
    vr::EVRFirmwareError PerformFirmwareUpdate(vr::TrackedDeviceIndex_t unDeviceIndex) override {
        return {};
    }
    void AcknowledgeQuit_Exiting() override {}
    uint32_t GetAppContainerFilePaths(VR_OUT_STRING() char *pchBuffer,
                                      uint32_t unBufferSize) override {
        return {};
    }
    const char *GetRuntimeVersion() override {
        return {};
    }
    vr::EVRInitError SetSDKVersion(uint32_t nVersionMajor, uint32_t nVersionMinor,
                                   uint32_t nVersionBuild) override {
        return {};
    }
};
struct Applications : IVRApplications {
    EVRApplicationError AddApplicationManifest(const char *pchApplicationManifestFullPath,
                                               bool bTemporary = false) override {
        return {};
    }
    EVRApplicationError
    RemoveApplicationManifest(const char *pchApplicationManifestFullPath) override {
        return {};
    }
    bool IsApplicationInstalled(const char *pchAppKey) override {
        return {};
    }
    uint32_t GetApplicationCount() override {
        return {};
    }
    EVRApplicationError GetApplicationKeyByIndex(uint32_t unApplicationIndex,
                                                 VR_OUT_STRING() char *pchAppKeyBuffer,
                                                 uint32_t unAppKeyBufferLen) override {
        return {};
    }
    EVRApplicationError GetApplicationKeyByProcessId(uint32_t unProcessId,
                                                     VR_OUT_STRING() char *pchAppKeyBuffer,
                                                     uint32_t unAppKeyBufferLen) override {
        return {};
    }
    EVRApplicationError LaunchApplication(const char *pchAppKey) override {
        return {};
    }
    EVRApplicationError LaunchTemplateApplication(const char *pchTemplateAppKey,
                                                  const char *pchNewAppKey,
                                                  VR_ARRAY_COUNT(unKeys)
                                                      const AppOverrideKeys_t *pKeys,
                                                  uint32_t unKeys) override {
        return {};
    }
    vr::EVRApplicationError LaunchApplicationFromMimeType(const char *pchMimeType,
                                                          const char *pchArgs) override {
        return {};
    }
    EVRApplicationError LaunchDashboardOverlay(const char *pchAppKey) override {
        return {};
    }
    bool CancelApplicationLaunch(const char *pchAppKey) override {
        return {};
    }
    EVRApplicationError IdentifyApplication(uint32_t unProcessId, const char *pchAppKey) override {
        if (unProcessId != uint32_t(getpid()) ||
            std::strcmp(pchAppKey, "org.controllerutils.frame") ||
            std::getenv("CT_TEST_OPENVR_IDENTIFY_FAILURE"))
            return VRApplicationError_UnknownApplication;
        ++identified;
        return VRApplicationError_None;
    }
    uint32_t GetApplicationProcessId(const char *pchAppKey) override {
        return {};
    }
    const char *GetApplicationsErrorNameFromEnum(EVRApplicationError error) override {
        return {};
    }
    uint32_t GetApplicationPropertyString(const char *pchAppKey, EVRApplicationProperty eProperty,
                                          VR_OUT_STRING() char *pchPropertyValueBuffer,
                                          uint32_t unPropertyValueBufferLen,
                                          EVRApplicationError *peError = nullptr) override {
        return {};
    }
    bool GetApplicationPropertyBool(const char *pchAppKey, EVRApplicationProperty eProperty,
                                    EVRApplicationError *peError = nullptr) override {
        return {};
    }
    uint64_t GetApplicationPropertyUint64(const char *pchAppKey, EVRApplicationProperty eProperty,
                                          EVRApplicationError *peError = nullptr) override {
        return {};
    }
    EVRApplicationError SetApplicationAutoLaunch(const char *pchAppKey, bool bAutoLaunch) override {
        ++startup_calls;
        if (std::getenv("CT_TEST_OPENVR_STARTUP_FAILURE"))
            return VRApplicationError_UnknownApplication;
        startup_enabled = bAutoLaunch;
        return VRApplicationError_None;
    }
    bool GetApplicationAutoLaunch(const char *pchAppKey) override {
        return startup_enabled;
    }
    EVRApplicationError SetDefaultApplicationForMimeType(const char *pchAppKey,
                                                         const char *pchMimeType) override {
        return {};
    }
    bool GetDefaultApplicationForMimeType(const char *pchMimeType,
                                          VR_OUT_STRING() char *pchAppKeyBuffer,
                                          uint32_t unAppKeyBufferLen) override {
        return {};
    }
    bool GetApplicationSupportedMimeTypes(const char *pchAppKey,
                                          VR_OUT_STRING() char *pchMimeTypesBuffer,
                                          uint32_t unMimeTypesBuffer) override {
        return {};
    }
    uint32_t GetApplicationsThatSupportMimeType(const char *pchMimeType,
                                                VR_OUT_STRING() char *pchAppKeysThatSupportBuffer,
                                                uint32_t unAppKeysThatSupportBuffer) override {
        return {};
    }
    uint32_t GetApplicationLaunchArguments(uint32_t unHandle, VR_OUT_STRING() char *pchArgs,
                                           uint32_t unArgs) override {
        return {};
    }
    EVRApplicationError GetStartingApplication(VR_OUT_STRING() char *pchAppKeyBuffer,
                                               uint32_t unAppKeyBufferLen) override {
        return {};
    }
    EVRSceneApplicationState GetSceneApplicationState() override {
        return {};
    }
    EVRApplicationError PerformApplicationPrelaunchCheck(const char *pchAppKey) override {
        return {};
    }
    const char *GetSceneApplicationStateNameFromEnum(EVRSceneApplicationState state) override {
        return {};
    }
    EVRApplicationError LaunchInternalProcess(const char *pchBinaryPath, const char *pchArguments,
                                              const char *pchWorkingDirectory) override {
        return {};
    }
    EVRApplicationError RegisterSubprocess(uint32_t nPid) override {
        return {};
    }
    uint32_t GetCurrentSceneProcessId() override {
        return std::getenv("CT_TEST_OPENVR_SCENE") ? 42 : 0;
    }
};
} // namespace ct_test
