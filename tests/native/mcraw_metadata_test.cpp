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
    // --- Crop ---
    {
        const auto uhd = video::mcrawCropFor(4000, 3000, 3840, 2160);
        check(uhd.valid() && !uhd.binned && uhd.width == 3840 && uhd.height == 2160 && uhd.x == 80 && uhd.y == 420,
              "4K from 4000x3000 is a centred sensor-resolution crop");
        const auto hd = video::mcrawCropFor(4000, 3000, 1920, 1080);
        check(hd.valid() && hd.binned && hd.width == 1920 && hd.height == 1080 && hd.x == 80 && hd.y == 420,
              "1080p from 4000x3000 is binned from the 3840x2160 region");
        const auto gate = video::mcrawCropFor(4000, 2998, 4000, 2998);
        check(gate.valid() && !gate.binned && gate.width == 4000 && gate.height == 2996 && (gate.x | gate.y) % 2 == 0,
              "open gate trims to whole 4-row tiles");
        check(!video::mcrawCropFor(1920, 1080, 3840, 2160).valid(), "oversized video has no crop");
    }
    // --- Extraction keeps CFA phase; binning averages same-colour samples ---
    {
        const uint32_t w = 16, h = 12;
        std::vector<uint16_t> raw(w * h);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) raw[y * w + x] = static_cast<uint16_t>(1000 * ((y & 1) * 2 + (x & 1)) + y * w + x);
        McrawCrop crop{w, h, 2, 2, 8, 8, false};
        std::vector<uint16_t> out(64);
        video::extractMcrawFrame(raw.data(), w, crop, out.data());
        check(out[0] == raw[2 * w + 2] && out[9] == raw[3 * w + 3], "crop copies from its origin");
        McrawCrop bin{w, h, 0, 0, 4, 4, true};
        std::vector<uint16_t> binned(16);
        video::extractMcrawFrame(raw.data(), w, bin, binned.data());
        bool phases = true;
        for (uint32_t y = 0; y < 4; ++y)
            for (uint32_t x = 0; x < 4; ++x) phases &= binned[y * 4 + x] / 1000 == (y & 1) * 2 + (x & 1);
        check(phases, "binning keeps each sample's CFA colour");
        const uint32_t expect = (raw[0] + raw[2] + raw[2 * w] + raw[2 * w + 2] + 2) / 4;
        check(binned[0] == expect, "binned R averages the four R samples of its 4x4 block");
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
    const McrawCrop crop = video::mcrawCropFor(256, 64, 128, 32);
    const std::string container = video::mcrawContainerJson(*camera, crop, clip);
    check(contains(container, "\"sensorArrangment\":\"bggr\""), "CFA name");
    check(contains(container, "\"blackLevel\":[67,66,65,64]"), "black level in stored (BGGR) order");
    check(contains(container, "\"colorIlluminant1\":\"standarda\"") && contains(container, "\"colorIlluminant2\":\"d65\""),
          "illuminant names");
    check(!contains(container, "calibrationMatrix1"), "invalid matrices are omitted");

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

#ifdef RAWR_WITH_MOTIONCAM_DECODER
    {
        const auto c = nlohmann::json::parse(container);
        const auto f = nlohmann::json::parse(frameJson);
        check(c["extraData"]["audioChannels"] == 2 && c["whiteLevel"] == 1023, "container JSON parses");
        check(f["width"] == 128 && f["height"] == 32 && f["compressionType"] == 7 && f["isBinned"] == true,
              "frame JSON parses");
        check(f["lensShadingMap"].size() == 4 && f["lensShadingMap"][3].size() == 6, "lens shading planes");

        // End to end: a binned clip from a 256x64 buffer decodes to the extracted pixels.
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
        check(decoder.getContainerMetadata()["sensorArrangment"] == "bggr", "reference decoder reads container JSON");
        unlink(path.c_str());
        std::printf("reference decoder cross-check ran\n");
    }
#endif
    if (failures == 0) std::printf("mcraw_metadata_test ok\n");
    return failures == 0 ? 0 : 1;
}
