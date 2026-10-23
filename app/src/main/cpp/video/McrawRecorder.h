#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "imaging/RawFrameLease.h"
#include "mcraw/McrawContainer.h"
#include "metadata/FrameMetadataSnapshot.h"
#include "video/McrawMetadata.h"
#include "video/MotionAxes.h"

namespace rawrcam::video {

// Writes the RAW frames of a video recording to a MotionCam .mcraw file next to
// the HEVC file. The frame pipeline offers each recorded frame once its GPU work
// has retired; a copy thread reads the camera buffer into CPU memory and returns
// the image to the camera quickly, worker threads compress, and one writer
// thread appends frames in order together with the microphone audio and the IMU
// samples that NativeAvRecorder forwards.
//
// One recording at a time: start() makes the recorder active() so the frame
// pipeline and NativeAvRecorder can find it without plumbing through JNI.
class McrawRecorder final : public std::enable_shared_from_this<McrawRecorder> {
   public:
    struct Settings {
        std::string deviceMake;
        std::string deviceModel;
        std::string software;
        int fps = 30;
        uint32_t videoWidth = 0;
        uint32_t videoHeight = 0;
        // Expected microphone channels, for the container header written at the first
        // frame; the first audio block's channel count wins when it arrives earlier.
        int audioChannels = 2;
        // Encoder threads; 0 picks from the core count.
        int workers = 0;
    };

    // Takes ownership of fd (closed by stop() or the destructor).
    static std::shared_ptr<McrawRecorder> start(int fd, const Settings& settings);
    [[nodiscard]] static std::shared_ptr<McrawRecorder> active();
    // Completes an interrupted .mcraw (see rawr::mcraw::ContainerWriter::recover); fd stays the caller's.
    static size_t recover(int fd);

    ~McrawRecorder();
    McrawRecorder(const McrawRecorder&) = delete;
    McrawRecorder& operator=(const McrawRecorder&) = delete;

    // Stops accepting input, writes everything queued, finishes the file. Returns
    // the final stats JSON. True in "complete" when the file is valid.
    std::string stop();

    // A retired, recorded camera frame. Takes the image when accepted; refuses
    // (leaving it with the caller) when the queues are full, so the camera never
    // waits on storage. rawWidth/rawHeight are the RAW buffer's size.
    bool offerFrame(imaging::RawFrameLease& lease, const metadata::FrameMetadataSnapshot& metadata, uint32_t rawWidth,
                    uint32_t rawHeight);
    // Interleaved 16-bit PCM at 48 kHz on CLOCK_BOOTTIME.
    void addAudio(int64_t firstFrameBootNs, const int16_t* samples, size_t frames, int channels);
    // IMU samples in device axes, time-ordered, on CLOCK_BOOTTIME.
    void addMotion(const std::vector<MotionSample>& samples);

    [[nodiscard]] std::string statsJson() const;

   private:
    McrawRecorder(int fd, const Settings& settings);
    struct Pending {
        imaging::RawFrameLease lease{imaging::AcquiredRawFrame{}};
        metadata::FrameMetadataSnapshot metadata;
        uint64_t sequence = 0;
    };
    struct Raw {
        uint64_t sequence = 0;
        int64_t timestampNs = 0;
        metadata::FrameMetadataSnapshot metadata;
        std::vector<uint16_t> pixels;
    };
    struct Encoded {
        int64_t timestampNs = 0;
        std::vector<uint8_t> data;
        std::string json;
        bool skip = false;
    };
    struct Audio {
        int64_t bootNs = 0;
        std::vector<int16_t> samples;
    };

    void copyLoop() noexcept;
    void encodeLoop() noexcept;
    void writeLoop() noexcept;
    void fail(const std::string& message);
    [[nodiscard]] bool accepting() const noexcept { return accepting_.load(std::memory_order_acquire); }

    int fd_ = -1;
    Settings settings_;
    std::atomic<bool> accepting_{true};

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    bool stopping_ = false, copyDone_ = false, encodeDone_ = false;
    size_t encodersRunning_ = 0;
    std::deque<Pending> pendingImages_;
    std::deque<Raw> raw_;
    std::map<uint64_t, Encoded> encoded_;
    std::deque<Audio> audio_;
    std::vector<MotionSample> motion_;
    uint64_t nextSequence_ = 0;
    size_t inFlight_ = 0;
    int audioChannels_ = 0;

    // Settled once, from the first offered frame.
    std::optional<McrawCrop> crop_;
    metadata::CameraContextMetadataPtr camera_;

    // Stats (mutex_).
    std::string status_ = "recording";
    uint64_t framesWritten_ = 0;
    uint64_t framesDropped_ = 0;
    uint64_t framesFailed_ = 0;
    uint64_t audioChunks_ = 0;
    uint64_t motionSamples_ = 0;
    uint64_t bytesWritten_ = 0;
    double copyMsTotal_ = 0.0, encodeMsTotal_ = 0.0;
    int64_t firstFrameNs_ = -1, lastFrameNs_ = -1;
    bool complete_ = false;

    std::thread copyThread_;
    std::vector<std::thread> encoders_;
    std::thread writerThread_;
};

}  // namespace rawrcam::video
