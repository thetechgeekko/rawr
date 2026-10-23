#include "video/McrawMetadata.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <vector>

#include "geometry/CfaPattern.h"
#include "video_pipeline/VideoCrop.h"

namespace rawrcam::video {
namespace {

// Minimal JSON builder: members are emitted in call order.
class Json {
   public:
    Json() { out_.push_back('{'); }
    Json& key(const char* name) {
        if (out_.size() > 1) out_.push_back(',');
        string(name);
        out_.push_back(':');
        return *this;
    }
    Json& str(const char* name, const std::string& value) {
        key(name);
        string(value);
        return *this;
    }
    Json& num(const char* name, double value) {
        key(name);
        number(value);
        return *this;
    }
    Json& integer(const char* name, int64_t value) {
        key(name);
        out_ += std::to_string(value);
        return *this;
    }
    Json& boolean(const char* name, bool value) {
        key(name);
        out_ += value ? "true" : "false";
        return *this;
    }
    template <typename Values>
    Json& numbers(const char* name, const Values& values) {
        key(name);
        array(values);
        return *this;
    }
    Json& raw(const char* name, const std::string& json) {
        key(name);
        out_ += json;
        return *this;
    }
    std::string done() {
        out_.push_back('}');
        return std::move(out_);
    }

    template <typename Values>
    void array(const Values& values) {
        out_.push_back('[');
        bool first = true;
        for (const auto& v : values) {
            if (!first) out_.push_back(',');
            first = false;
            number(static_cast<double>(v));
        }
        out_.push_back(']');
    }
    std::string& text() { return out_; }

   private:
    void number(double value) {
        if (!std::isfinite(value)) value = 0.0;
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.9g", value);
        out_ += buffer;
    }
    void string(const std::string& value) {
        out_.push_back('"');
        for (const char c : value) {
            if (c == '"' || c == '\\') {
                out_.push_back('\\');
                out_.push_back(c);
            } else if (static_cast<unsigned char>(c) < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(c));
                out_ += buffer;
            } else {
                out_.push_back(c);
            }
        }
        out_.push_back('"');
    }
    std::string out_;
};

const char* arrangement(uint32_t cfa) {
    switch (cfa) {
        case 1: return "grbg";
        case 2: return "gbrg";
        case 3: return "bggr";
        default: return "rggb";
    }
}

// Camera2 SENSOR_REFERENCE_ILLUMINANT uses the EXIF LightSource codes. MCRAW
// readers accept the common ones by name and anything else as the integer.
void illuminant(Json& json, const char* name, int32_t code) {
    switch (code) {
        case 17: json.str(name, "standarda"); return;
        case 20: json.str(name, "d55"); return;
        case 21: json.str(name, "d65"); return;
        case 23: json.str(name, "d50"); return;
        default: json.integer(name, code);
    }
}

void matrix(Json& json, const char* name, const metadata::Matrix3x3& m) {
    if (m.valid) json.numbers(name, m.rowMajor);
}
}  // namespace

McrawCrop mcrawCropFor(uint32_t rawWidth, uint32_t rawHeight, uint32_t videoWidth, uint32_t videoHeight) {
    McrawCrop crop;
    const VideoSourceRect rect = videoSourceRect(rawWidth, rawHeight, videoWidth, videoHeight);
    if (rect.width == 0 || rect.height == 0) return crop;
    crop.rawWidth = rawWidth;
    crop.rawHeight = rawHeight;
    crop.x = rect.x;
    crop.y = rect.y;
    crop.binned = rect.reduceCfa;
    crop.width = (crop.binned ? rect.width / 2 : rect.width) & ~1u;
    crop.height = (crop.binned ? rect.height / 2 : rect.height) & ~3u;  // whole MCRAW tiles
    if (crop.width < 2 || crop.height < 4) return McrawCrop{};
    return crop;
}

void extractMcrawFrame(const uint16_t* raw, size_t rawStridePixels, const McrawCrop& crop, uint16_t* out) noexcept {
    if (!crop.binned) {
        for (uint32_t y = 0; y < crop.height; ++y)
            std::copy_n(raw + size_t(crop.y + y) * rawStridePixels + crop.x, crop.width, out + size_t(y) * crop.width);
        return;
    }
    // Each output sample averages the four same-colour samples of a 4x4 source block:
    // output (y, x) has CFA phase (y & 1, x & 1) and reads phase-matched rows and
    // columns 0 and 2 apart, so the stored frame keeps the buffer's CFA order.
    for (uint32_t y = 0; y < crop.height; ++y) {
        const uint32_t sy = crop.y + 4 * (y >> 1) + (y & 1u);
        const uint16_t* r0 = raw + size_t(sy) * rawStridePixels;
        const uint16_t* r1 = r0 + 2 * rawStridePixels;
        uint16_t* row = out + size_t(y) * crop.width;
        for (uint32_t x = 0; x < crop.width; ++x) {
            const uint32_t sx = crop.x + 4 * (x >> 1) + (x & 1u);
            const uint32_t sum = uint32_t(r0[sx]) + r0[sx + 2] + r1[sx] + r1[sx + 2];
            row[x] = static_cast<uint16_t>((sum + 2) >> 2);
        }
    }
}

std::string mcrawContainerJson(const metadata::CameraContextMetadata& camera, const McrawCrop& crop,
                               const McrawClipInfo& clip) {
    Json json;
    const auto black = geometry::reorderRggbByCode(camera.baselineBlackLevelPhysicalRggb, camera.rawPreviewCfa);
    json.numbers("blackLevel", black);
    json.num("whiteLevel", std::max(1.0f, camera.baselineWhiteLevel));
    json.str("sensorArrangment", arrangement(camera.rawPreviewCfa));
    json.integer("sensorOrientation", camera.geometry.sensorOrientationDegrees);
    const auto& color = camera.color;
    illuminant(json, "colorIlluminant1", color.referenceIlluminant1);
    illuminant(json, "colorIlluminant2", color.referenceIlluminant2);
    matrix(json, "colorMatrix1", color.colorTransform1);
    matrix(json, "colorMatrix2", color.colorTransform2);
    matrix(json, "forwardMatrix1", color.forwardMatrix1);
    matrix(json, "forwardMatrix2", color.forwardMatrix2);
    matrix(json, "calibrationMatrix1", color.calibrationTransform1);
    matrix(json, "calibrationMatrix2", color.calibrationTransform2);
    if (!camera.availableFocalLengthsMm.empty()) json.numbers("focalLengths", camera.availableFocalLengthsMm);
    if (!camera.availableApertures.empty()) json.numbers("apertures", camera.availableApertures);
    json.str("uniqueCameraModel", clip.deviceMake + " " + clip.deviceModel + " / camera " + camera.cameraId);
    Json profile;
    profile.str("cameraId", camera.cameraId).str("deviceModel", clip.deviceModel);
    json.raw("deviceSpecificProfile", profile.done());
    Json extra;
    if (clip.audioChannels > 0)
        extra.integer("audioSampleRate", clip.audioSampleRate).integer("audioChannels", clip.audioChannels);
    extra.integer("frameRate", clip.fps)
        .str("software", clip.software)
        .str("deviceMake", clip.deviceMake)
        .integer("originalWidth", crop.rawWidth)
        .integer("originalHeight", crop.rawHeight)
        .integer("cropX", crop.x)
        .integer("cropY", crop.y)
        .boolean("binned", crop.binned)
        .boolean("timestampsRealtime", camera.sensorTimestampRealtime);
    json.raw("extraData", extra.done());
    return json.done();
}

std::string mcrawFrameJson(const metadata::FrameMetadataSnapshot& frame, int64_t timestampNs, const McrawCrop& crop) {
    Json json;
    json.integer("timestamp", timestampNs);  // first: ContainerWriter::recover() reads it back
    json.integer("width", crop.width).integer("height", crop.height);
    json.integer("originalWidth", crop.rawWidth).integer("originalHeight", crop.rawHeight);
    json.integer("compressionType", 7).str("pixelFormat", "raw16");
    json.boolean("isBinned", crop.binned).boolean("isCompressed", true).boolean("needRemosaic", false);
    const uint32_t cfa = frame.cameraContext ? frame.cameraContext->rawPreviewCfa : 0;
    json.numbers("dynamicBlackLevel", geometry::reorderRggbByCode(frame.blackLevelPhysicalRggb, cfa));
    json.num("dynamicWhiteLevel", frame.effectiveWhiteLevel);
    // AsShotNeutral as the DNG writer derives it: manual WB from the inverse gains,
    // otherwise the camera's neutral point.
    std::vector<float> neutral;
    const auto& g = frame.colorCorrectionGainsRggb;
    const float green = 0.5f * (g[1] + g[2]);
    if (frame.awbMode == 0 && g[0] > 1e-6f && green > 1e-6f && g[3] > 1e-6f) {
        const float r = 1.0f / g[0], gg = 1.0f / green, b = 1.0f / g[3];
        const float peak = std::max({r, gg, b});
        neutral = {r / peak, gg / peak, b / peak};
    } else if (frame.hasNeutralColorPoint) {
        neutral.assign(frame.neutralColorPoint.begin(), frame.neutralColorPoint.end());
    }
    if (!neutral.empty()) json.numbers("asShotNeutral", neutral);
    json.integer("iso", frame.sensitivity).integer("exposureTime", frame.exposureTimeNs);
    const size_t points = size_t(frame.lensShadingMapWidth) * frame.lensShadingMapHeight;
    if (points > 0 && frame.lensShadingMap.size() == points * 4) {
        json.integer("lensShadingMapWidth", frame.lensShadingMapWidth)
            .integer("lensShadingMapHeight", frame.lensShadingMapHeight);
        json.key("lensShadingMap");
        std::string& text = json.text();
        text.push_back('[');
        for (size_t channel = 0; channel < 4; ++channel) {
            if (channel) text.push_back(',');
            std::vector<float> plane(points);
            for (size_t i = 0; i < points; ++i) plane[i] = frame.lensShadingMap[i * 4 + channel];
            json.array(plane);
        }
        text.push_back(']');
    }
    return json.done();
}

}  // namespace rawrcam::video
