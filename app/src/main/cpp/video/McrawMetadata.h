#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "metadata/CameraContextMetadata.h"
#include "metadata/FrameMetadataSnapshot.h"

namespace rawrcam::video {

// The region of each RAW frame an MCRAW clip stores. Samples are never altered:
// the clip holds the sensor values exactly as the camera delivered them, either
// the whole RAW buffer or the recorded video's framing at sensor resolution.
// Origins are even, so the stored frame keeps the RAW buffer's CFA order; the
// stored height is a multiple of 4 (the MCRAW codec's tile), dropping at most
// three bottom rows of the full frame.
struct McrawCrop {
    uint32_t rawWidth = 0, rawHeight = 0;  // RAW buffer
    uint32_t x = 0, y = 0;                 // origin in the RAW buffer
    uint32_t width = 0, height = 0;        // stored frame

    [[nodiscard]] bool valid() const noexcept { return width > 0 && height > 0; }
};

enum class McrawFraming {
    FullSensor,  // the whole RAW buffer
    Video,       // the recorded video's framing (videoSourceRect), unbinned
};

// Region to store from a rawWidth x rawHeight buffer. With Video framing it is
// the area the video records from (for a 2x-reduced video, the 2x source region),
// and is invalid when the video does not fit the buffer.
[[nodiscard]] McrawCrop mcrawCropFor(uint32_t rawWidth, uint32_t rawHeight, McrawFraming framing,
                                     uint32_t videoWidth = 0, uint32_t videoHeight = 0);

// Copies the crop out of a packed RAW16 buffer into width * height samples, unchanged.
void extractMcrawFrame(const uint16_t* raw, size_t rawStridePixels, const McrawCrop& crop, uint16_t* out) noexcept;

struct McrawClipInfo {
    std::string deviceMake;
    std::string deviceModel;
    std::string software;
    int fps = 30;
    int audioSampleRate = 48'000;
    int audioChannels = 0;  // 0 = no audio
};

// Container JSON. Key names follow MotionCam's own files as read by its
// reference decoder and by third-party MCRAW tools (mcraw4vulkan,
// mcraw-mounter-fuse), including MotionCam's "sensorArrangment" spelling:
//   blackLevel[4] (stored CFA order), whiteLevel, sensorArrangment ("rggb"...),
//   sensorOrientation, colorIlluminant1/2, colorMatrix1/2, forwardMatrix1/2,
//   calibrationMatrix1/2 (row-major 3x3, Camera2 SENSOR_* tags), focalLengths,
//   apertures, uniqueCameraModel, deviceSpecificProfile{cameraId, deviceModel,
//   disableShadingMap}, extraData{audioSampleRate, audioChannels, ...}.
// Tools that convert MCRAW to DNG (e.g. mcraw-mounter-fuse) require both colour
// matrices and both forward matrices; a camera that reports only one calibration
// has it repeated for the second illuminant, which DNG readers treat as a single
// calibration. Forward matrices the camera does not report are left out.
[[nodiscard]] std::string mcrawContainerJson(const metadata::CameraContextMetadata& camera, const McrawCrop& crop,
                                             const McrawClipInfo& clip);

// Frame JSON: timestamp (ns, the container key), width, height, originalWidth/Height,
// compressionType 7, pixelFormat "raw16", isBinned (always false), isCompressed
// (lossless), needRemosaic, dynamicBlackLevel[4], dynamicWhiteLevel, asShotNeutral[3],
// iso, exposureTime (ns), and the Camera2 lens shading map as four planes in Camera2
// channel order (R, G_even, G_odd, B) over the full active array. The map is left
// out when the camera reports its RAW output as already shading-corrected.
[[nodiscard]] std::string mcrawFrameJson(const metadata::FrameMetadataSnapshot& frame, int64_t timestampNs,
                                         const McrawCrop& crop);

}  // namespace rawrcam::video
