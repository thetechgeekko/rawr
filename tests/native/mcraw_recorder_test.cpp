// McrawRecorder threading on the host: frames offered from the pipeline thread
// are copied, encoded on several workers and written in order with audio and
// IMU samples; refused frames stay with the caller; every camera image is
// released exactly once. The camera buffer copy is replaced by a deterministic
// fake keyed by the buffer pointer, so each written frame can be checked.
#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "imaging/Raw16CpuSnapshot.h"
#include "video/McrawRecorder.h"

#ifdef RAWR_WITH_MOTIONCAM_DECODER
#include <motioncam/Decoder.hpp>
#endif

namespace {
std::atomic<int> gDeleted{0};
std::atomic<int> gCopies{0};
constexpr uint32_t kRawWidth = 256, kRawHeight = 96;

uint16_t fakePixel(uintptr_t id, size_t i) { return static_cast<uint16_t>(64 + (id * 131 + i * 7) % 900); }
}  // namespace

extern "C" void AImage_delete(AImage* image) {
    ++gDeleted;
    delete reinterpret_cast<int*>(image);
}

namespace rawrcam::imaging {
Raw16CpuSnapshotResult copyCompletedRaw16AhbToPacked(AHardwareBuffer* ahb, uint32_t width, uint32_t height,
                                                     std::vector<uint8_t>& destination, int, AImage*) noexcept {
    ++gCopies;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));  // a camera buffer read takes time
    const auto id = reinterpret_cast<uintptr_t>(ahb);
    auto* out = reinterpret_cast<uint16_t*>(destination.data());
    for (size_t i = 0; i < size_t(width) * height; ++i) out[i] = fakePixel(id, i);
    Raw16CpuSnapshotResult result;
    result.success = true;
    return result;
}
}  // namespace rawrcam::imaging

namespace {
int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

struct Run {
    std::string path;
    std::vector<uintptr_t> writtenIds;  // frames the recorder accepted, in order
    std::vector<int64_t> timestamps;
    std::string stats;
};

Run record(rawrcam::video::McrawFraming framing, uint32_t videoWidth, uint32_t videoHeight, int frames, bool flood) {
    using namespace rawrcam;
    Run run;
    const char* tmp = std::getenv("TMPDIR");
    run.path = std::string(tmp ? tmp : "/tmp") + "/mcraw-recorder-" + std::to_string(videoWidth) + "-" +
               std::to_string(frames) + ".mcraw";
    const int fd = open(run.path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
    video::McrawRecorder::Settings settings;
    settings.deviceMake = "test";
    settings.deviceModel = "host";
    settings.software = "mcraw_recorder_test";
    settings.framing = framing;
    settings.videoWidth = videoWidth;
    settings.videoHeight = videoHeight;
    settings.audioChannels = 2;
    settings.workers = 3;
    auto recorder = video::McrawRecorder::start(fd, settings);
    check(video::McrawRecorder::active() == recorder, "recorder is active");
    bool threw = false;
    try {
        (void)video::McrawRecorder::start(open("/dev/null", O_RDWR), settings);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw, "a second recording is refused");

    auto camera = std::make_shared<metadata::CameraContextMetadata>();
    camera->cameraId = "0";
    camera->baselineWhiteLevel = 1023;
    camera->color.referenceIlluminant1 = 21;
    camera->color.colorTransform1 = {{1, 0, 0, 0, 1, 0, 0, 0, 1}, true};
    std::vector<int16_t> pcm(1024 * 2, 7);
    for (int i = 0; i < frames; ++i) {
        const int64_t ts = 1'000'000'000 + int64_t(i) * 33'333'333;
        metadata::FrameMetadataSnapshot metadata;
        metadata.cameraContext = camera;
        metadata.timestampNs = static_cast<uint64_t>(ts);
        metadata.effectiveWhiteLevel = 1023;
        const auto id = static_cast<uintptr_t>(i + 1);
        imaging::RawFrameLease lease(imaging::AcquiredRawFrame{reinterpret_cast<AImage*>(new int(i)),
                                                               reinterpret_cast<AHardwareBuffer*>(id), -1,
                                                               static_cast<uint64_t>(ts), 1});
        if (recorder->offerFrame(lease, metadata, kRawWidth, kRawHeight)) {
            check(!lease, "an accepted frame takes the image");
            run.writtenIds.push_back(id);
            run.timestamps.push_back(ts);
        } else {
            check(static_cast<bool>(lease), "a refused frame leaves the image with the caller");
        }
        recorder->addAudio(ts - 5'000'000, pcm.data(), 1024, 2);
        std::vector<video::MotionSample> motion;
        for (int k = 0; k < 6; ++k) {
            video::MotionSample s;
            s.bootNs = ts + k * 5'000'000;
            s.type = k % 2 == 0 ? 2 : 3;
            s.device = {float(i), float(k), 1.0f};
            motion.push_back(s);
        }
        recorder->addMotion(motion);
        if (!flood) std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    run.stats = recorder->stop();
    check(video::McrawRecorder::active() == nullptr, "stop deactivates");
    return run;
}

void verify(const Run& run, rawrcam::video::McrawFraming framing, uint32_t videoWidth, uint32_t videoHeight) {
    using namespace rawrcam;
    check(run.stats.find("\"complete\":true") != std::string::npos, "complete: " + run.stats);
    const std::string written = "\"framesWritten\":" + std::to_string(run.writtenIds.size()) + ",";
    check(run.stats.find(written) != std::string::npos, "every accepted frame written: " + run.stats);
#ifdef RAWR_WITH_MOTIONCAM_DECODER
    motioncam::Decoder decoder(run.path);
    check(decoder.getFrames() == run.timestamps, "frames in offer order");
    const video::McrawCrop crop = video::mcrawCropFor(kRawWidth, kRawHeight, framing, videoWidth, videoHeight);
    check(crop.valid(), "crop");
    std::vector<uint16_t> raw(size_t(kRawWidth) * kRawHeight), expected(size_t(crop.width) * crop.height);
    for (size_t f = 0; f < run.writtenIds.size(); ++f) {
        for (size_t i = 0; i < raw.size(); ++i) raw[i] = fakePixel(run.writtenIds[f], i);
        video::extractMcrawFrame(raw.data(), kRawWidth, crop, expected.data());
        std::vector<uint8_t> pixels;
        nlohmann::json metadata;
        decoder.loadFrame(run.timestamps[f], pixels, metadata);
        check(pixels.size() == expected.size() * 2 && std::memcmp(pixels.data(), expected.data(), pixels.size()) == 0,
              "frame " + std::to_string(f) + " pixels");
        check(metadata["timestamp"] == run.timestamps[f] && metadata["isBinned"] == false &&
                  metadata["width"] == crop.width,
              "frame metadata");
    }
    std::vector<motioncam::AudioChunk> audio;
    decoder.loadAudio(audio);
    check(!audio.empty() && decoder.numAudioChannels() == 2, "audio written");
    std::vector<motioncam::MotionSample> gyro, accelerometer;
    decoder.loadGyroData(gyro);
    decoder.loadAccelerometerData(accelerometer);
    check(!gyro.empty() && !accelerometer.empty(), "IMU written");
    bool ordered = true;
    for (size_t i = 1; i < gyro.size(); ++i) ordered &= gyro[i - 1].timestampNs <= gyro[i].timestampNs;
    check(ordered, "gyro time-ordered");
#else
    (void)framing;
    (void)videoWidth;
    (void)videoHeight;
#endif
    unlink(run.path.c_str());
}
}  // namespace

int main() {
    using rawrcam::video::McrawFraming;
    // Paced like a camera, full RAW frames: every frame is taken.
    const Run paced = record(McrawFraming::FullSensor, 0, 0, 20, false);
    check(paced.writtenIds.size() == 20, "paced: all frames accepted (" + std::to_string(paced.writtenIds.size()) + ")");
    verify(paced, McrawFraming::FullSensor, 0, 0);
    // Offered faster than the copy thread drains: some are refused, none lost or reordered.
    const Run flooded = record(McrawFraming::FullSensor, 0, 0, 60, true);
    check(!flooded.writtenIds.empty() && flooded.writtenIds.size() < 60, "flood: queue bound refuses frames");
    verify(flooded, McrawFraming::FullSensor, 0, 0);
    // Video framing: the region the video records from, at sensor resolution.
    const Run cropped = record(McrawFraming::Video, 200, 80, 8, false);
    verify(cropped, McrawFraming::Video, 200, 80);
    // Every image the recorder took was released once; the test released the refused ones.
    const int total = 20 + 60 + 8;
    const int taken = int(paced.writtenIds.size() + flooded.writtenIds.size() + cropped.writtenIds.size());
    check(gCopies.load() == taken, "one copy per accepted frame");
    check(gDeleted.load() == total, "every image released exactly once (" + std::to_string(gDeleted.load()) + ")");
    if (failures == 0) std::printf("mcraw_recorder_test ok\n");
    return failures == 0 ? 0 : 1;
}
