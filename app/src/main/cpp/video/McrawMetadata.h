#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "metadata/CameraContextMetadata.h"
#include "metadata/FrameMetadataSnapshot.h"

namespace rawrcam::video {

// The part of each RAW frame an MCRAW clip stores: the recorded video's framing
// (videoSourceRect), either at sensor resolution or 2x2 same-colour binned when
// the video itself is a 2x reduction. Origins are even, so the CFA order of the
// stored frame is the RAW buffer's; the stored height is a multiple of 4, as the
// MCRAW codec requires.
struct McrawCrop {
    uint32_t rawWidth = 0, rawHeight = 0;  // RAW buffer
    uint32_t x = 0, y = 0;                 // source origin in the RAW buffer
    uint32_t width = 0, height = 0;        // stored frame
    bool binned = false;                   // source spans 2 * width x 2 * height

    [[nodiscard]] bool valid() const noexcept { return width > 0 && height > 0; }
};

// Crop for a recording of videoWidth x videoHeight from a rawWidth x rawHeight buffer.
// Invalid (empty) when the video does not fit.
[[nodiscard]] McrawCrop mcrawCropFor(uint32_t rawWidth, uint32_t rawHeight, uint32_t videoWidth, uint32_t videoHeight);

// Copies (or bins) the crop out of a packed RAW16 buffer into width * height samples.
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
//   apertures, uniqueCameraModel, deviceSpecificProfile{cameraId, deviceModel},
//   extraData{audioSampleRate, audioChannels, ...}.
[[nodiscard]] std::string mcrawContainerJson(const metadata::CameraContextMetadata& camera, const McrawCrop& crop,
                                             const McrawClipInfo& clip);

// Frame JSON: timestamp (ns, the container key), width, height, originalWidth/Height,
// compressionType 7, pixelFormat "raw16", isBinned, isCompressed, needRemosaic,
// dynamicBlackLevel[4], dynamicWhiteLevel, asShotNeutral[3], iso, exposureTime (ns),
// and the Camera2 lens shading map as four planes in Camera2 channel order
// (R, G_even, G_odd, B) over the full active array.
[[nodiscard]] std::string mcrawFrameJson(const metadata::FrameMetadataSnapshot& frame, int64_t timestampNs,
                                         const McrawCrop& crop);

}  // namespace rawrcam::video
