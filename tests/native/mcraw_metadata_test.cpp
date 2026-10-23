// MCRAW crop/bin and metadata JSON. With the reference decoder available the
// JSON is parsed by the same nlohmann::json the MotionCam decoder uses, and a
// clip built from it is decoded end to end.
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "mcraw/McrawContainer.h"
#include "mcraw/McrawEncoder.h"
#include "video/McrawMetadata.h"

#ifdef RAWR_WITH_MOTIONCAM_DECODER
#include <motioncam/Decoder.hpp>
#endif

namespace {
using rawrcam::video::McrawCrop;
int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}
bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
}  // namespace

int main() {
    using namespace rawrcam;
    using video::McrawFraming;
    // --- Region: never resampled ---
    {
        const auto full = video::mcrawCropFor(4000, 3000, McrawFraming::FullSensor);
        check(full.valid() && full.x == 0 && full.y == 0 && full.width == 4000 && full.height == 3000,
              "full frame is the whole RAW buffer");
        const auto odd = video::mcrawCropFor(4000, 2998, McrawFraming::FullSensor);
        check(odd.width == 4000 && odd.height == 2996, "full frame drops only the rows past whole 4-row tiles");
        const auto uhd = video::mcrawCropFor(4000, 3000, McrawFraming::Video, 3840, 2160);
        check(uhd.valid() && uhd.width == 3840 && uhd.height == 2160 && uhd.x == 80 && uhd.y == 420,
              "4K video framing is a centred sensor-resolution crop");
        const auto hd = video::mcrawCropFor(4000, 3000, McrawFraming::Video, 1920, 1080);
        check(hd.valid() && hd.width == 3840 && hd.height == 2160 && hd.x == 80 && hd.y == 420,
              "1080p video framing stores its 3840x2160 source region, unbinned");
        check(!video::mcrawCropFor(1920, 1080, McrawFraming::Video, 3840, 2160).valid(), "oversized video has no crop");
    }
    // --- Extraction copies samples unchanged, keeping CFA phase ---
    {
        const uint32_t w = 16, h = 12;
        std::vector<uint16_t> raw(w * h);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) raw[y * w + x] = static_cast<uint16_t>(1000 * ((y & 1) * 2 + (x & 1)) + y * w + x);
        McrawCrop crop{w, h, 2, 2, 8, 8};
        std::vector<uint16_t> out(64);
        video::extractMcrawFrame(raw.data(), w, crop, out.data());
        bool same = true;
        for (uint32_t y = 0; y < 8; ++y)
            for (uint32_t x = 0; x < 8; ++x) same &= out[y * 8 + x] == raw[(y + 2) * w + x + 2];
        check(same, "crop copies every sample unchanged");
    }

    // --- Metadata JSON ---
    auto camera = std::make_shared<metadata::CameraContextMetadata>();
    camera->cameraId = "0";
    camera->rawPreviewCfa = 3;  // BGGR
    camera->baselineBlackLevelPhysicalRggb = {64, 65, 66, 67};
    camera->baselineWhiteLevel = 1023;
    camera->geometry.sensorOrientationDegrees = 90;
    camera->color.referenceIlluminant1 = 17;
    camera->color.referenceIlluminant2 = 21;
    camera->color.colorTransform1 = {{1, 0, 0, 0, 1, 0, 0, 0, 1}, true};
    camera->color.colorTransform2 = {{0.5f, 0, 0, 0, 0.5f, 0, 0, 0, 0.5f}, true};
    camera->color.forwardMatrix1 = {{0.7f, 0.1f, 0.1f, 0.2f, 0.7f, 0.1f, 0, 0, 0.8f}, true};
    camera->availableFocalLengthsMm = {6.9f};
    camera->availableApertures = {1.8f};
    video::McrawClipInfo clip{"vivo", "V2408A", "RAWR test", 30, 48000, 2};
    const McrawCrop crop = video::mcrawCropFor(256, 64, McrawFraming::FullSensor);
    const std::string container = video::mcrawContainerJson(*camera, crop, clip);
    check(contains(container, "\"sensorArrangment\":\"bggr\""), "CFA name");
    check(contains(container, "\"blackLevel\":[67,66,65,64]"), "black level in stored (BGGR) order");
    check(contains(container, "\"colorIlluminant1\":\"standarda\"") && contains(container, "\"colorIlluminant2\":\"d65\""),
          "illuminant names");
    check(!contains(container, "calibrationMatrix1"), "invalid matrices are omitted");
    check(contains(container, "\"forwardMatrix2\":[0.699999988"), "a missing second forward matrix repeats the first");
    check(contains(container, "\"disableShadingMap\":false"), "shading map in use");
    {
        auto single = *camera;
        single.color.referenceIlluminant2 = -1;
        single.color.colorTransform2 = {};
        const std::string one = video::mcrawContainerJson(single, crop, clip);
        check(contains(one, "\"colorIlluminant2\":\"standarda\"") && contains(one, "\"colorMatrix2\":[1,0,0,0,1,0,0,0,1]"),
              "a single calibration is repeated for the second illuminant");
    }

    metadata::FrameMetadataSnapshot frame;
    frame.cameraContext = camera;
    frame.blackLevelPhysicalRggb = {63, 64, 65, 66};
    frame.effectiveWhiteLevel = 1023;
    frame.sensitivity = 400;
    frame.exposureTimeNs = 16'666'667;
    frame.hasNeutralColorPoint = true;
    frame.neutralColorPoint = {0.5f, 1.0f, 0.6f};
    frame.lensShadingMapWidth = 3;
    frame.lensShadingMapHeight = 2;
    for (int i = 0; i < 6; ++i)
        for (int c = 0; c < 4; ++c) frame.lensShadingMap.push_back(1.0f + c + i * 0.01f);
    const std::string frameJson = video::mcrawFrameJson(frame, 123'456'789, crop);
    check(frameJson.rfind("{\"timestamp\":123456789,", 0) == 0, "timestamp leads the frame JSON");
    check(contains(frameJson, "\"dynamicBlackLevel\":[66,65,64,63]"), "dynamic black level in stored order");
    check(contains(frameJson, "\"lensShadingMap\":[[1,1.00999999,"), "lens shading plane 0 is the R channel");
    check(contains(frameJson, "\"asShotNeutral\":[0.5,1,0.600000024]"), "neutral point");
    check(contains(frameJson, "\"isBinned\":false"), "never binned");
    {
        auto corrected = std::make_shared<metadata::CameraContextMetadata>(*camera);
        corrected->hasLensShadingApplied = true;
        corrected->lensShadingApplied = true;
        auto f = frame;
        f.cameraContext = corrected;
        check(!contains(video::mcrawFrameJson(f, 1, crop), "lensShadingMap"),
              "no shading map when the RAW is already shading-corrected");
        check(contains(video::mcrawContainerJson(*corrected, crop, clip), "\"disableShadingMap\":true"),
              "container flags shading-corrected RAW");
    }

#ifdef RAWR_WITH_MOTIONCAM_DECODER
    {
        const auto c = nlohmann::json::parse(container);
        const auto f = nlohmann::json::parse(frameJson);
        check(c["extraData"]["audioChannels"] == 2 && c["whiteLevel"] == 1023, "container JSON parses");
        check(f["width"] == 256 && f["height"] == 64 && f["compressionType"] == 7 && f["isBinned"] == false,
              "frame JSON parses");
        check(f["lensShadingMap"].size() == 4 && f["lensShadingMap"][3].size() == 6, "lens shading planes");

        // End to end: a full-frame clip of a 256x64 buffer decodes to the very same samples.
        std::vector<uint16_t> raw(256 * 64);
        for (size_t i = 0; i < raw.size(); ++i) raw[i] = static_cast<uint16_t>(64 + (i * 7919) % 900);
        std::vector<uint16_t> stored(size_t(crop.width) * crop.height);
        video::extractMcrawFrame(raw.data(), 256, crop, stored.data());
        std::vector<uint8_t> encoded;
        rawr::mcraw::encodeFrame(stored.data(), crop.width, crop.height, crop.width, encoded);
        const std::string path = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/mcraw-meta.mcraw";
        const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
        {
            rawr::mcraw::ContainerWriter writer(fd);
            writer.begin(container);
            writer.writeFrame(123'456'789, encoded.data(), encoded.size(), frameJson);
            writer.finish();
        }
        close(fd);
        motioncam::Decoder decoder(path);
        std::vector<uint8_t> pixels;
        nlohmann::json metadata;
        decoder.loadFrame(decoder.getFrames().at(0), pixels, metadata);
        check(pixels.size() == stored.size() * 2 && std::memcmp(pixels.data(), stored.data(), pixels.size()) == 0,
              "reference decoder returns the stored frame");
        check(stored == raw, "full frame stores the RAW buffer unchanged");
        check(decoder.getContainerMetadata()["sensorArrangment"] == "bggr", "reference decoder reads container JSON");
        unlink(path.c_str());
        std::printf("reference decoder cross-check ran\n");
    }
#endif
    if (failures == 0) std::printf("mcraw_metadata_test ok\n");
    return failures == 0 ? 0 : 1;
}
