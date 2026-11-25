#pragma once
#include <openvr.h>
#include <atomic>
#include <array>
#include <cstdlib>
#include <cstring>
namespace ct_test {
using namespace vr;
inline std::atomic<unsigned> next_overlay{0}, overlays_created{0}, overlays_destroyed{0},
    textures_submitted{0}, overlays_shown{0};
inline std::array<float, 512> overlay_widths{};
inline std::atomic<unsigned> keyboard_shown{0}, keyboard_positioned{0};
struct Overlay : IVROverlay {
    EVROverlayError FindOverlay(const char *pchOverlayKey,
                                VROverlayHandle_t *pOverlayHandle) override {
        const auto target = std::getenv("CT_TEST_OPENVR_KEYBOARD_TARGET");
        if (target && !std::strcmp(target, pchOverlayKey)) {
            *pOverlayHandle = 42;
            return VROverlayError_None;
        }
        return VROverlayError_UnknownOverlay;
    }
    EVROverlayError CreateOverlay(const char *pchOverlayKey, const char *pchOverlayName,
                                  VROverlayHandle_t *pOverlayHandle) override {
        if (std::getenv("CT_TEST_OPENVR_OVERLAY_FAILURE"))
            return VROverlayError_RequestFailed;
        *pOverlayHandle = ++next_overlay;
        ++overlays_created;
        return VROverlayError_None;
    }
    EVROverlayError CreateSubviewOverlay(VROverlayHandle_t parentOverlayHandle,
                                         const char *pchSubviewOverlayKey,
                                         const char *pchSubviewOverlayName,
                                         VROverlayHandle_t *pSubviewOverlayHandle) override {
        return {};
    }
    EVROverlayError DestroyOverlay(VROverlayHandle_t ulOverlayHandle) override {
        ++overlays_destroyed;
        return VROverlayError_None;
    }
    uint32_t GetOverlayKey(VROverlayHandle_t ulOverlayHandle, VR_OUT_STRING() char *pchValue,
                           uint32_t unBufferSize, EVROverlayError *pError = 0L) override {
        return {};
    }
    uint32_t GetOverlayName(VROverlayHandle_t ulOverlayHandle, VR_OUT_STRING() char *pchValue,
                            uint32_t unBufferSize, EVROverlayError *pError = 0L) override {
        return {};
    }
    EVROverlayError SetOverlayName(VROverlayHandle_t ulOverlayHandle,
                                   const char *pchName) override {
        return {};
    }
    EVROverlayError GetOverlayImageData(VROverlayHandle_t ulOverlayHandle, void *pvBuffer,
                                        uint32_t unBufferSize, uint32_t *punWidth,
                                        uint32_t *punHeight) override {
        return {};
    }
    const char *GetOverlayErrorNameFromEnum(EVROverlayError error) override {
        return {};
    }
    EVROverlayError SetOverlayRenderingPid(VROverlayHandle_t ulOverlayHandle,
                                           uint32_t unPID) override {
        return {};
    }
    uint32_t GetOverlayRenderingPid(VROverlayHandle_t ulOverlayHandle) override {
        return {};
    }
    EVROverlayError SetOverlayFlag(VROverlayHandle_t ulOverlayHandle, VROverlayFlags eOverlayFlag,
                                   bool bEnabled) override {
        return {};
    }
    EVROverlayError GetOverlayFlag(VROverlayHandle_t ulOverlayHandle, VROverlayFlags eOverlayFlag,
                                   bool *pbEnabled) override {
        return {};
    }
    EVROverlayError GetOverlayFlags(VROverlayHandle_t ulOverlayHandle, uint32_t *pFlags) override {
        return {};
    }
    EVROverlayError SetOverlayColor(VROverlayHandle_t ulOverlayHandle, float fRed, float fGreen,
                                    float fBlue) override {
        return {};
    }
    EVROverlayError GetOverlayColor(VROverlayHandle_t ulOverlayHandle, float *pfRed, float *pfGreen,
                                    float *pfBlue) override {
        return {};
    }
    EVROverlayError SetOverlayAlpha(VROverlayHandle_t ulOverlayHandle, float fAlpha) override {
        return {};
    }
    EVROverlayError GetOverlayAlpha(VROverlayHandle_t ulOverlayHandle, float *pfAlpha) override {
        return {};
    }
    EVROverlayError SetOverlayTexelAspect(VROverlayHandle_t ulOverlayHandle,
                                          float fTexelAspect) override {
        return {};
    }
    EVROverlayError GetOverlayTexelAspect(VROverlayHandle_t ulOverlayHandle,
                                          float *pfTexelAspect) override {
        return {};
    }
    EVROverlayError SetOverlaySortOrder(VROverlayHandle_t ulOverlayHandle,
                                        uint32_t unSortOrder) override {
        return {};
    }
    EVROverlayError GetOverlaySortOrder(VROverlayHandle_t ulOverlayHandle,
                                        uint32_t *punSortOrder) override {
        return {};
    }
    EVROverlayError SetOverlayWidthInMeters(VROverlayHandle_t ulOverlayHandle,
                                            float fWidthInMeters) override {
        overlay_widths.at(ulOverlayHandle) = fWidthInMeters;
        return VROverlayError_None;
    }
    EVROverlayError GetOverlayWidthInMeters(VROverlayHandle_t ulOverlayHandle,
                                            float *pfWidthInMeters) override {
        return {};
    }
    EVROverlayError SetOverlayCurvature(VROverlayHandle_t ulOverlayHandle,
                                        float fCurvature) override {
        return {};
    }
    EVROverlayError GetOverlayCurvature(VROverlayHandle_t ulOverlayHandle,
                                        float *pfCurvature) override {
        return {};
    }
    EVROverlayError SetOverlayPreCurvePitch(VROverlayHandle_t ulOverlayHandle,
                                            float fRadians) override {
        return {};
    }
    EVROverlayError GetOverlayPreCurvePitch(VROverlayHandle_t ulOverlayHandle,
                                            float *pfRadians) override {
        return {};
    }
    EVROverlayError SetOverlayTextureColorSpace(VROverlayHandle_t ulOverlayHandle,
                                                EColorSpace eTextureColorSpace) override {
        return {};
    }
    EVROverlayError GetOverlayTextureColorSpace(VROverlayHandle_t ulOverlayHandle,
                                                EColorSpace *peTextureColorSpace) override {
        return {};
    }
    EVROverlayError
    SetOverlayTextureBounds(VROverlayHandle_t ulOverlayHandle,
                            const VRTextureBounds_t *pOverlayTextureBounds) override {
        return {};
    }
    EVROverlayError GetOverlayTextureBounds(VROverlayHandle_t ulOverlayHandle,
                                            VRTextureBounds_t *pOverlayTextureBounds) override {
        return {};
    }
    EVROverlayError GetOverlayTransformType(VROverlayHandle_t ulOverlayHandle,
                                            VROverlayTransformType *peTransformType) override {
        return {};
    }
    EVROverlayError SetOverlayTransformAbsolute(
        VROverlayHandle_t ulOverlayHandle, ETrackingUniverseOrigin eTrackingOrigin,
        const HmdMatrix34_t *pmatTrackingOriginToOverlayTransform) override {
        return {};
    }
    EVROverlayError
    GetOverlayTransformAbsolute(VROverlayHandle_t ulOverlayHandle,
                                ETrackingUniverseOrigin *peTrackingOrigin,
                                HmdMatrix34_t *pmatTrackingOriginToOverlayTransform) override {
        return {};
    }
    EVROverlayError SetOverlayTransformTrackedDeviceRelative(
        VROverlayHandle_t ulOverlayHandle, TrackedDeviceIndex_t unTrackedDevice,
        const HmdMatrix34_t *pmatTrackedDeviceToOverlayTransform) override {
        return {};
    }
    EVROverlayError GetOverlayTransformTrackedDeviceRelative(
        VROverlayHandle_t ulOverlayHandle, TrackedDeviceIndex_t *punTrackedDevice,
        HmdMatrix34_t *pmatTrackedDeviceToOverlayTransform) override {
        return {};
    }
    EVROverlayError
    SetOverlayTransformTrackedDeviceComponent(VROverlayHandle_t ulOverlayHandle,
                                              TrackedDeviceIndex_t unDeviceIndex,
                                              const char *pchComponentName) override {
        return {};
    }
    EVROverlayError GetOverlayTransformTrackedDeviceComponent(
        VROverlayHandle_t ulOverlayHandle, TrackedDeviceIndex_t *punDeviceIndex,
        VR_OUT_STRING() char *pchComponentName, uint32_t unComponentNameSize) override {
        return {};
    }
    EVROverlayError SetOverlayTransformCursor(VROverlayHandle_t ulCursorOverlayHandle,
                                              const HmdVector2_t *pvHotspot) override {
        return {};
    }
    vr::EVROverlayError GetOverlayTransformCursor(VROverlayHandle_t ulOverlayHandle,
                                                  HmdVector2_t *pvHotspot) override {
        return {};
    }
    vr::EVROverlayError SetOverlayTransformProjection(
        VROverlayHandle_t ulOverlayHandle, ETrackingUniverseOrigin eTrackingOrigin,
        const HmdMatrix34_t *pmatTrackingOriginToOverlayTransform,
        const VROverlayProjection_t *pProjection, vr::EVREye eEye) override {
        return {};
    }
    EVROverlayError SetSubviewPosition(VROverlayHandle_t ulOverlayHandle, float fX,
                                       float fY) override {
        return {};
    }
    EVROverlayError ShowOverlay(VROverlayHandle_t ulOverlayHandle) override {
        ++overlays_shown;
        return VROverlayError_None;
    }
    EVROverlayError HideOverlay(VROverlayHandle_t ulOverlayHandle) override {
        return {};
    }
    bool IsOverlayVisible(VROverlayHandle_t ulOverlayHandle) override {
        return {};
    }
    EVROverlayError GetTransformForOverlayCoordinates(VROverlayHandle_t ulOverlayHandle,
                                                      ETrackingUniverseOrigin eTrackingOrigin,
                                                      HmdVector2_t coordinatesInOverlay,
                                                      HmdMatrix34_t *pmatTransform) override {
        *pmatTransform = {};
        for (int i = 0; i < 3; ++i)
            pmatTransform->m[i][i] = 1;
        pmatTransform->m[1][3] = coordinatesInOverlay.v[1] * overlay_widths.at(ulOverlayHandle);
        return VROverlayError_None;
    }
    EVROverlayError WaitFrameSync(uint32_t nTimeoutMs) override {
        return {};
    }
    bool PollNextOverlayEvent(VROverlayHandle_t ulOverlayHandle, VREvent_t *pEvent,
                              uint32_t uncbVREvent) override {
        return {};
    }
    EVROverlayError GetOverlayInputMethod(VROverlayHandle_t ulOverlayHandle,
                                          VROverlayInputMethod *peInputMethod) override {
        return {};
    }
    EVROverlayError SetOverlayInputMethod(VROverlayHandle_t ulOverlayHandle,
                                          VROverlayInputMethod eInputMethod) override {
        return {};
    }
    EVROverlayError GetOverlayMouseScale(VROverlayHandle_t ulOverlayHandle,
                                         HmdVector2_t *pvecMouseScale) override {
        return {};
    }
    EVROverlayError SetOverlayMouseScale(VROverlayHandle_t ulOverlayHandle,
                                         const HmdVector2_t *pvecMouseScale) override {
        return {};
    }
    bool ComputeOverlayIntersection(VROverlayHandle_t ulOverlayHandle,
                                    const VROverlayIntersectionParams_t *pParams,
                                    VROverlayIntersectionResults_t *pResults) override {
        return {};
    }
    bool IsHoverTargetOverlay(VROverlayHandle_t ulOverlayHandle) override {
        return {};
    }
    EVROverlayError SetOverlayIntersectionMask(
        VROverlayHandle_t ulOverlayHandle, VROverlayIntersectionMaskPrimitive_t *pMaskPrimitives,
        uint32_t unNumMaskPrimitives,
        uint32_t unPrimitiveSize = sizeof(VROverlayIntersectionMaskPrimitive_t)) override {
        return {};
    }
    EVROverlayError TriggerLaserMouseHapticVibration(VROverlayHandle_t ulOverlayHandle,
                                                     float fDurationSeconds, float fFrequency,
                                                     float fAmplitude) override {
        return {};
    }
    EVROverlayError SetOverlayCursor(VROverlayHandle_t ulOverlayHandle,
                                     VROverlayHandle_t ulCursorHandle) override {
        return {};
    }
    EVROverlayError SetOverlayCursorPositionOverride(VROverlayHandle_t ulOverlayHandle,
                                                     const HmdVector2_t *pvCursor) override {
        return {};
    }
    EVROverlayError ClearOverlayCursorPositionOverride(VROverlayHandle_t ulOverlayHandle) override {
        return {};
    }
    EVROverlayError SetOverlayTexture(VROverlayHandle_t ulOverlayHandle,
                                      const Texture_t *pTexture) override {
        ++textures_submitted;
        return VROverlayError_None;
    }
    EVROverlayError ClearOverlayTexture(VROverlayHandle_t ulOverlayHandle) override {
        return {};
    }
    EVROverlayError SetOverlayRaw(VROverlayHandle_t ulOverlayHandle, void *pvBuffer,
                                  uint32_t unWidth, uint32_t unHeight,
                                  uint32_t unBytesPerPixel) override {
        return {};
    }
    EVROverlayError SetOverlayFromFile(VROverlayHandle_t ulOverlayHandle,
                                       const char *pchFilePath) override {
        return {};
    }
    EVROverlayError GetOverlayTexture(VROverlayHandle_t ulOverlayHandle,
                                      void **pNativeTextureHandle, void *pNativeTextureRef,
                                      uint32_t *pWidth, uint32_t *pHeight, uint32_t *pNativeFormat,
                                      ETextureType *pAPIType, EColorSpace *pColorSpace,
                                      VRTextureBounds_t *pTextureBounds) override {
        return {};
    }
    EVROverlayError ReleaseNativeOverlayHandle(VROverlayHandle_t ulOverlayHandle,
                                               void *pNativeTextureHandle) override {
        return {};
    }
    EVROverlayError GetOverlayTextureSize(VROverlayHandle_t ulOverlayHandle, uint32_t *pWidth,
                                          uint32_t *pHeight) override {
        *pWidth = *pHeight = 128;
        return VROverlayError_None;
    }
    EVROverlayError CreateDashboardOverlay(const char *pchOverlayKey,
                                           const char *pchOverlayFriendlyName,
                                           VROverlayHandle_t *pMainHandle,
                                           VROverlayHandle_t *pThumbnailHandle) override {
        *pMainHandle = ++next_overlay;
        *pThumbnailHandle = ++next_overlay;
        overlays_created += 2;
        return VROverlayError_None;
    }
    bool IsDashboardVisible() override {
        return {};
    }
    bool IsActiveDashboardOverlay(VROverlayHandle_t ulOverlayHandle) override {
        return {};
    }
    EVROverlayError SetDashboardOverlaySceneProcess(VROverlayHandle_t ulOverlayHandle,
                                                    uint32_t unProcessId) override {
        return {};
    }
    EVROverlayError GetDashboardOverlaySceneProcess(VROverlayHandle_t ulOverlayHandle,
                                                    uint32_t *punProcessId) override {
        return {};
    }
    void ShowDashboard(const char *pchOverlayToShow) override {}
    vr::TrackedDeviceIndex_t GetPrimaryDashboardDevice() override {
        return {};
    }
    EVROverlayError ShowKeyboard(EGamepadTextInputMode eInputMode,
                                 EGamepadTextInputLineMode eLineInputMode, uint32_t unFlags,
                                 const char *pchDescription, uint32_t unCharMax,
                                 const char *pchExistingText, uint64_t uUserValue) override {
        return {};
    }
    EVROverlayError ShowKeyboardForOverlay(VROverlayHandle_t ulOverlayHandle,
                                           EGamepadTextInputMode eInputMode,
                                           EGamepadTextInputLineMode eLineInputMode,
                                           uint32_t unFlags, const char *pchDescription,
                                           uint32_t unCharMax, const char *pchExistingText,
                                           uint64_t uUserValue) override {
        if (ulOverlayHandle != 42 || eInputMode != k_EGamepadTextInputModeNormal ||
            eLineInputMode != k_EGamepadTextInputLineModeSingleLine ||
            unFlags !=
                (KeyboardFlag_Minimal | KeyboardFlag_HideDoneKey | KeyboardFlag_ShowArrowKeys) ||
            *pchDescription || unCharMax != 1024 || *pchExistingText || uUserValue)
            return VROverlayError_InvalidParameter;
        ++keyboard_shown;
        if (std::getenv("CT_TEST_OPENVR_KEYBOARD_FAILURE"))
            return VROverlayError_RequestFailed;
        return {};
    }
    uint32_t GetKeyboardText(VR_OUT_STRING() char *pchText, uint32_t cchText) override {
        return {};
    }
    void HideKeyboard() override {}
    void SetKeyboardTransformAbsolute(
        ETrackingUniverseOrigin eTrackingOrigin,
        const HmdMatrix34_t *pmatTrackingOriginToKeyboardTransform) override {}
    void SetKeyboardPositionForOverlay(VROverlayHandle_t ulOverlayHandle,
                                       HmdRect2_t avoidRect) override {
        if (ulOverlayHandle == 42 && avoidRect.vTopLeft.v[0] == 0 && avoidRect.vTopLeft.v[1] == 0 &&
            avoidRect.vBottomRight.v[0] == 0 && avoidRect.vBottomRight.v[1] == 0)
            ++keyboard_positioned;
    }
    VRMessageOverlayResponse ShowMessageOverlay(const char *pchText, const char *pchCaption,
                                                const char *pchButton0Text,
                                                const char *pchButton1Text = nullptr,
                                                const char *pchButton2Text = nullptr,
                                                const char *pchButton3Text = nullptr) override {
        return {};
    }
    void CloseMessageOverlay() override {}
};
} // namespace ct_test
