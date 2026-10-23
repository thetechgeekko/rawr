#include "camera/CameraRequestControls.h"

#include <camera/NdkCameraMetadataTags.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "camera/Camera2PriorityCompat.h"
#include "camera/CameraAePriority.h"
#include "camera/CameraRequestCadence.h"
#include "camera/StillFocus.h"
#include "camera/ViewfinderExposure.h"
#include "camera/WhiteBalanceMath.h"

namespace rawrcam::camera {
namespace {

float clampGain(float value) { return std::clamp(value, 1.0f, 4.0f); }

bool awbModeAdvertised(const CameraControlState& state, uint8_t awb) {
    const auto& modes = state.capabilities.supportedAwbModes;
    return std::find(modes.begin(), modes.end(), awb) != modes.end();
}

}  // namespace

void applyPreviewRequestDefaults(ACaptureRequest* request, const metadata::CameraContextMetadata& context) {
    if (!request) return;
    const uint8_t shadingMap = context.lensShadingMapWidth >= 2 && context.lensShadingMapHeight >= 2
                                   ? ACAMERA_STATISTICS_LENS_SHADING_MAP_MODE_ON
                                   : ACAMERA_STATISTICS_LENS_SHADING_MAP_MODE_OFF;
    const uint8_t distortion = ACAMERA_DISTORTION_CORRECTION_MODE_OFF;
    const uint8_t hotPixel = ACAMERA_HOT_PIXEL_MODE_OFF;
    const uint8_t hotPixelMap = ACAMERA_STATISTICS_HOT_PIXEL_MAP_MODE_ON;
    // Keep metadata enabled for every RAW frame without request swaps during bursts.
    (void)ACaptureRequest_setEntry_u8(request, ACAMERA_STATISTICS_LENS_SHADING_MAP_MODE, 1, &shadingMap);
    (void)ACaptureRequest_setEntry_u8(request, ACAMERA_DISTORTION_CORRECTION_MODE, 1, &distortion);
    (void)ACaptureRequest_setEntry_u8(request, ACAMERA_HOT_PIXEL_MODE, 1, &hotPixel);
    (void)ACaptureRequest_setEntry_u8(request, ACAMERA_STATISTICS_HOT_PIXEL_MAP_MODE, 1, &hotPixelMap);
}

CameraSessionCadenceApplyResult applyCameraSessionCadence(ACaptureRequest* request, const CameraControlState& state) {
    CameraSessionCadenceApplyResult result;
    if (!request) {
        result.status = ACAMERA_ERROR_INVALID_PARAMETER;
        return result;
    }
    const bool priority = state.exposureMode == ExposureControlMode::ShutterPriority ||
                          state.exposureMode == ExposureControlMode::IsoPriority;
    result.fpsRange = cameraTargetFpsRange(state, priority);
    if (result.fpsRange) {
        result.status =
            ACaptureRequest_setEntry_i32(request, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2, result.fpsRange->data());
    }
    return result;
}

CameraControlApplyResult applyCameraControlState(ACaptureRequest* request, const CameraControlState& state,
                                                 const CameraMeteringRequest& metering) {
    CameraControlApplyResult result{};
    if (!request) return result;

    const uint8_t controlMode = ACAMERA_CONTROL_MODE_AUTO;
    (void)ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_MODE, 1, &controlMode);

    const bool rawrAe = state.exposureMode != ExposureControlMode::Manual && state.rawrAeActive &&
                        state.rawrAeExposureTimeNs > 0 && state.rawrAeSensitivity > 0;
    // rawr AE is AE OFF with the app-solved pair: the same sensor ownership as M.
    const bool manual = state.exposureMode == ExposureControlMode::Manual || rawrAe;
    const uint8_t ae = manual ? ACAMERA_CONTROL_AE_MODE_OFF : ACAMERA_CONTROL_AE_MODE_ON;
    (void)ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AE_MODE, 1, &ae);

    // AE priority is a distinct Camera2 mode (API 36). Explicitly write OFF for
    // ordinary Auto/Manual requests because this ACaptureRequest object is reused.
    uint8_t aePriority = camera2_priority::kOff;
    // rawr AE owns S/I itself (AE OFF); otherwise only the HAL's own modes.
    // S/I with neither owner (rawr ineligible, e.g. video) runs as Auto.
    if (!rawrAe && state.exposureMode == ExposureControlMode::ShutterPriority &&
        state.capabilities.halShutterPrioritySupported) {
        aePriority = camera2_priority::kSensorExposureTimePriority;
    } else if (!rawrAe && state.exposureMode == ExposureControlMode::IsoPriority &&
               state.capabilities.halIsoPrioritySupported) {
        aePriority = camera2_priority::kSensorSensitivityPriority;
    }
    result.halPriorityMode = aePriority != camera2_priority::kOff;
    if (state.capabilities.aePriorityTagAvailable) {
        result.aePriorityRequested = true;
        result.aePriorityStatus =
            ACaptureRequest_setEntry_u8(request, camera2_priority::kAePriorityModeTag, 1, &aePriority);
    }

    // Priority AE still obeys CONTROL_AE_TARGET_FPS_RANGE. TEMPLATE_PREVIEW can
    // default to a 30/30 range, which prevents exposure-time priority from ever
    // going longer than ~33 ms. Give S/I the broadest advertised <=30 fps range.
    // Auto gets the user-configured floor (default 15) so the HAL can prefer
    // longer exposure over post-raw digital boost in low light; 30 == off and
    // restores template behavior. Manual removes the entry.
    if (const auto fps = cameraTargetFpsRange(state, aePriority != camera2_priority::kOff)) {
        result.aeTargetFpsRequested = true;
        result.aeTargetFpsStatus =
            ACaptureRequest_setEntry_i32(request, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2, fps->data());
    } else {
        (void)ACaptureRequest_setEntry_i32(request, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 0, nullptr);
    }

    // rawr AE in video mode holds the video preview cadence, as recording does.
    const int64_t framePeriodNs = state.recordingFps > 0 ? 1'000'000'000LL / state.recordingFps
                                  : rawrAe && state.videoMode && state.videoPreviewFps > 0
                                      ? 1'000'000'000LL / state.videoPreviewFps
                                      : 0;
    const int64_t exposureMaximum = framePeriodNs > 0
        ? std::min(state.capabilities.exposureTimeMaxNs, framePeriodNs)
        : state.capabilities.exposureTimeMaxNs;
    // M: shutters past the viewfinder cap run only on capture (ViewfinderExposure.h).
    const auto manualPair = viewfinderManualPair(state);
    const bool manualMode = state.exposureMode == ExposureControlMode::Manual;
    const int64_t exposure = std::clamp(rawrAe       ? state.rawrAeExposureTimeNs
                                        : manualMode ? manualPair.exposureTimeNs
                                                     : state.requestedExposureTimeNs,
                                        state.capabilities.exposureTimeMinNs,
                                        std::max(state.capabilities.exposureTimeMinNs, exposureMaximum));
    const int32_t sensitivity = std::clamp(rawrAe       ? state.rawrAeSensitivity
                                           : manualMode ? manualPair.sensitivity
                                                        : state.requestedSensitivity,
                                           state.capabilities.sensitivityMin, state.capabilities.sensitivityMax);

    // The repeating ACaptureRequest is reused. Normalize SENSOR_* ownership on
    // every mode so stale values from M/S/I cannot leak into a different AE mode.
    // Camera2 NDK removes an entry when count=0 and data=nullptr.
    if (manual) {
        (void)ACaptureRequest_setEntry_i64(request, ACAMERA_SENSOR_EXPOSURE_TIME, 1, &exposure);
        (void)ACaptureRequest_setEntry_i32(request, ACAMERA_SENSOR_SENSITIVITY, 1, &sensitivity);

        if (state.capabilities.maxFrameDurationNs > 0) {
            constexpr int64_t kThirtyFpsFrameNs = 33333333LL;
            const int64_t frameDuration = framePeriodNs > 0 ? framePeriodNs :
                std::min(state.capabilities.maxFrameDurationNs, std::max(kThirtyFpsFrameNs, exposure));
            (void)ACaptureRequest_setEntry_i64(request, ACAMERA_SENSOR_FRAME_DURATION, 1, &frameDuration);
        }
    } else if (aePriority == camera2_priority::kSensorExposureTimePriority) {
        // S: app owns shutter; AE owns ISO + frame duration.
        (void)ACaptureRequest_setEntry_i64(request, ACAMERA_SENSOR_EXPOSURE_TIME, 1, &exposure);
        (void)ACaptureRequest_setEntry_i32(request, ACAMERA_SENSOR_SENSITIVITY, 0, nullptr);
        (void)ACaptureRequest_setEntry_i64(request, ACAMERA_SENSOR_FRAME_DURATION, 0, nullptr);
    } else if (aePriority == camera2_priority::kSensorSensitivityPriority) {
        // I: app owns ISO; AE owns shutter + frame duration.
        (void)ACaptureRequest_setEntry_i32(request, ACAMERA_SENSOR_SENSITIVITY, 1, &sensitivity);
        (void)ACaptureRequest_setEntry_i64(request, ACAMERA_SENSOR_EXPOSURE_TIME, 0, nullptr);
        (void)ACaptureRequest_setEntry_i64(request, ACAMERA_SENSOR_FRAME_DURATION, 0, nullptr);
    } else {
        // A: return all sensor exposure ownership to Camera2 AE.
        (void)ACaptureRequest_setEntry_i64(request, ACAMERA_SENSOR_EXPOSURE_TIME, 0, nullptr);
        (void)ACaptureRequest_setEntry_i32(request, ACAMERA_SENSOR_SENSITIVITY, 0, nullptr);
        (void)ACaptureRequest_setEntry_i64(request, ACAMERA_SENSOR_FRAME_DURATION, 0, nullptr);
    }

    // AE antibanding. The repeating request is reused so normalize every
    // time; unsupported values fall back to AUTO (or the first advertised).
    // No effect in Manual (AE OFF) per Camera2 docs, but harmless to write.
    {
        uint8_t banding = state.requestedAntibandingMode;
        const auto& supported = state.capabilities.supportedAntibandingModes;
        if (!supported.empty() &&
            std::find(supported.begin(), supported.end(), banding) == supported.end()) {
            const auto autoIt = std::find(supported.begin(), supported.end(),
                                          static_cast<uint8_t>(ACAMERA_CONTROL_AE_ANTIBANDING_MODE_AUTO));
            banding = autoIt != supported.end() ? *autoIt : supported.front();
        }
        (void)ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AE_ANTIBANDING_MODE, 1, &banding);
    }

    if (!manual) {
        const uint8_t aeLock = ACAMERA_CONTROL_AE_LOCK_OFF;
        (void)ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AE_LOCK, 1, &aeLock);

        const int32_t ev = std::clamp(state.requestedEvSteps + std::min(0, state.hdrHeadroomSteps),
                                      state.capabilities.evMinSteps, state.capabilities.evMaxSteps);
        (void)ACaptureRequest_setEntry_i32(request, ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION, 1, &ev);
    }

    // White balance. The repeating ACaptureRequest is reused, so every branch
    // normalizes full ownership: presets write AWB + HIGH_QUALITY and remove
    // stale manual GAINS/TRANSFORM; manual writes AWB OFF + TRANSFORM_MATRIX +
    // relative GAINS + the frozen HAL transform. Removal uses count=0/data=null.
    //
    // Manual gains are relative to the entry baseline captured on
    // AUTO/preset->MANUAL (Camera2-endorsed: TRANSFORM_MATRIX with the
    // HAL-provided gains+transform reproduces the previous white point).
    // Rendering = entry x forward(requested)/forward(entry), so entering
    // manual is seamless and returning sliders to the entry values restores
    // the preset/auto look exactly. Absolute Helland gains are the fallback
    // when no entry baseline exists (e.g. entered before any HAL result).
    result.wbManualRequested = state.whiteBalanceMode == WhiteBalanceControlMode::ManualTempTint;
    if (result.wbManualRequested && state.capabilities.manualGainsSupported &&
        state.hasManualWhiteBalanceTransformSeed) {
        const uint8_t awb = ACAMERA_CONTROL_AWB_MODE_OFF;
        const uint8_t color = ACAMERA_COLOR_CORRECTION_MODE_TRANSFORM_MATRIX;
        result.wbStatus = ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AWB_MODE, 1, &awb);
        (void)ACaptureRequest_setEntry_u8(request, ACAMERA_COLOR_CORRECTION_MODE, 1, &color);
        const std::array<float, 4> fwdNew =
            whiteBalanceGainsForTempTint(state.requestedWhiteBalanceTemperatureK, state.requestedWhiteBalanceTint);
        std::array<float, 4> gains = fwdNew;
        if (state.hasManualWhiteBalanceEntryGains) {
            const std::array<float, 4> fwdEntry = whiteBalanceGainsForTempTint(
                state.manualWhiteBalanceEntryTempK, state.manualWhiteBalanceEntryTint);
            const float entryR = state.manualWhiteBalanceEntryGains[0];
            const float entryG = 0.5f * (state.manualWhiteBalanceEntryGains[1] +
                                         state.manualWhiteBalanceEntryGains[2]);
            const float entryB = state.manualWhiteBalanceEntryGains[3];
            if (fwdEntry[0] > 1.0e-6f && fwdEntry[1] > 1.0e-6f && fwdEntry[3] > 1.0e-6f &&
                std::isfinite(entryR) && std::isfinite(entryG) && std::isfinite(entryB)) {
                gains = {clampGain(entryR * fwdNew[0] / fwdEntry[0]),
                         clampGain(entryG * fwdNew[1] / fwdEntry[1]),
                         clampGain(entryG * fwdNew[1] / fwdEntry[1]),
                         clampGain(entryB * fwdNew[3] / fwdEntry[3])};
            }
        }
        (void)ACaptureRequest_setEntry_float(request, ACAMERA_COLOR_CORRECTION_GAINS, 4, gains.data());
        // COLOR_CORRECTION_TRANSFORM is rational[9]; carry the HAL seed at
        // 1e-4 precision (magnitude stays within [-1.5, 3.0], fitting int32).
        ACameraMetadata_rational transform[9];
        for (size_t i = 0; i < 9; ++i) {
            transform[i].numerator = static_cast<int32_t>(std::lround(state.manualWhiteBalanceTransformSeed[i] * 10000.0f));
            transform[i].denominator = 10000;
        }
        (void)ACaptureRequest_setEntry_rational(request, ACAMERA_COLOR_CORRECTION_TRANSFORM, 9, transform);
    } else {
        if (result.wbManualRequested) result.wbManualFallbackToAuto = true;
        uint8_t awb = static_cast<uint8_t>(state.whiteBalanceMode);
        if (state.whiteBalanceMode == WhiteBalanceControlMode::ManualTempTint || !awbModeAdvertised(state, awb)) {
            awb = ACAMERA_CONTROL_AWB_MODE_AUTO;
        }
        const uint8_t color = ACAMERA_COLOR_CORRECTION_MODE_HIGH_QUALITY;
        result.wbStatus = ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AWB_MODE, 1, &awb);
        (void)ACaptureRequest_setEntry_u8(request, ACAMERA_COLOR_CORRECTION_MODE, 1, &color);
        (void)ACaptureRequest_setEntry_float(request, ACAMERA_COLOR_CORRECTION_GAINS, 0, nullptr);
        (void)ACaptureRequest_setEntry_rational(request, ACAMERA_COLOR_CORRECTION_TRANSFORM, 0, nullptr);
    }

    uint8_t af = ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE;
    if (state.focusMode == FocusControlMode::Manual) {
        af = ACAMERA_CONTROL_AF_MODE_OFF;
    } else if (state.focusMode == FocusControlMode::LockedAuto || state.tapAfActive) {
        af = ACAMERA_CONTROL_AF_MODE_AUTO;
    }
    result.focusStatus = ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AF_MODE, 1, &af);

    // Face detection feeds the face boxes + face-steered continuous AF. FULL
    // carries per-face scores; fall back to SIMPLE. The repeating request is
    // reused, so explicitly write OFF when unsupported.
    const uint8_t faceDetect = !state.capabilities.faceDetectSupported
                                   ? ACAMERA_STATISTICS_FACE_DETECT_MODE_OFF
                                   : (state.capabilities.faceDetectFullMode
                                          ? ACAMERA_STATISTICS_FACE_DETECT_MODE_FULL
                                          : ACAMERA_STATISTICS_FACE_DETECT_MODE_SIMPLE);
    (void)ACaptureRequest_setEntry_u8(request, ACAMERA_STATISTICS_FACE_DETECT_MODE, 1, &faceDetect);

    const uint8_t ois = (state.capabilities.oisSupported && state.oisEnabled)
                            ? ACAMERA_LENS_OPTICAL_STABILIZATION_MODE_ON
                            : ACAMERA_LENS_OPTICAL_STABILIZATION_MODE_OFF;
    (void)ACaptureRequest_setEntry_u8(request, ACAMERA_LENS_OPTICAL_STABILIZATION_MODE, 1, &ois);
    // Electronic stabilization is never wanted: RAW output is not stabilized by the HAL,
    // and a HAL that crops or shifts anyway would break gyro stabilization in post.
    // OFF is always a supported mode.
    const uint8_t eis = ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE_OFF;
    (void)ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE, 1, &eis);

    if ((state.focusMode == FocusControlMode::Manual) && state.capabilities.manualFocusSupported) {
        const float diopters = requestedFocusDiopters(state);
        const auto status = ACaptureRequest_setEntry_float(request, ACAMERA_LENS_FOCUS_DISTANCE, 1, &diopters);
        if (status != ACAMERA_OK) result.focusStatus = status;
    }
    (void)ACaptureRequest_setEntry_i32(request, ACAMERA_CONTROL_AF_REGIONS, metering.afRegion ? 5 : 0,
                                       metering.afRegion ? metering.afRegion->data() : nullptr);
    (void)ACaptureRequest_setEntry_i32(request, ACAMERA_CONTROL_AE_REGIONS, metering.aeRegion ? 5 : 0,
                                       metering.aeRegion ? metering.aeRegion->data() : nullptr);
    const uint8_t trigger =
        metering.trigger == CameraFocusTrigger::Start ? ACAMERA_CONTROL_AF_TRIGGER_START
        : metering.trigger == CameraFocusTrigger::Cancel && !state.capabilities.tapAfHoldsUntilNextTap
            ? ACAMERA_CONTROL_AF_TRIGGER_CANCEL
            : ACAMERA_CONTROL_AF_TRIGGER_IDLE;
    (void)ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AF_TRIGGER, 1, &trigger);
    return result;
}

}  // namespace rawrcam::camera
