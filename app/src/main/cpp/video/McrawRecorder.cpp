#include "video/McrawRecorder.h"

#include <android/log.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <stdexcept>

#include "imaging/Raw16CpuSnapshot.h"
#include "mcraw/McrawEncoder.h"

namespace rawrcam::video {
namespace {
[[maybe_unused]] constexpr char kTag[] = "RawrMcraw";
// Camera images held by the copy thread. The RAW reader has few buffers; the
// pipeline keeps the rest, so more than this would stall the camera.
constexpr size_t kMaxPendingImages = 2;
// Frames copied but not yet written. Bounds memory (one stored frame each) and
// turns a storage stall into dropped frames instead of a stalled camera.
constexpr size_t kMaxInFlight = 6;
// Audio/IMU held while no frame has been written yet (about 10 s).
constexpr size_t kMaxQueuedAudioChunks = 480;
constexpr size_t kMaxQueuedMotion = 8'000;

std::mutex gActiveMutex;
std::shared_ptr<McrawRecorder> gActive;

double msSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
}  // namespace

std::shared_ptr<McrawRecorder> McrawRecorder::start(int fd, const Settings& settings) {
    if (fd < 0) throw std::invalid_argument("MCRAW recording needs a file descriptor");
    std::shared_ptr<McrawRecorder> recorder(new McrawRecorder(fd, settings));
    std::lock_guard<std::mutex> lock(gActiveMutex);
    if (gActive) throw std::runtime_error("an MCRAW recording is already active");
    gActive = recorder;
    return recorder;
}

std::shared_ptr<McrawRecorder> McrawRecorder::active() {
    std::lock_guard<std::mutex> lock(gActiveMutex);
    return gActive;
}

size_t McrawRecorder::recover(int fd) { return rawr::mcraw::ContainerWriter::recover(fd); }

McrawRecorder::McrawRecorder(int fd, const Settings& settings) : fd_(fd), settings_(settings) {
    if (settings_.videoWidth == 0 || settings_.videoHeight == 0) {
        ::close(fd_);
        throw std::invalid_argument("MCRAW recording needs the video size");
    }
    const unsigned cores = std::max(2u, std::thread::hardware_concurrency());
    const int workers = settings_.workers > 0 ? settings_.workers : static_cast<int>(std::clamp(cores / 2, 2u, 4u));
    encodersRunning_ = static_cast<size_t>(workers);
    copyThread_ = std::thread([this] { copyLoop(); });
    for (int i = 0; i < workers; ++i) encoders_.emplace_back([this] { encodeLoop(); });
    writerThread_ = std::thread([this] { writeLoop(); });
}

McrawRecorder::~McrawRecorder() {
    if (writerThread_.joinable()) (void)stop();
}

void McrawRecorder::fail(const std::string& message) {
    accepting_.store(false, std::memory_order_release);
    if (status_ == "recording") status_ = message;
    __android_log_print(ANDROID_LOG_ERROR, kTag, "MCRAW_FAILED %s", message.c_str());
}

bool McrawRecorder::offerFrame(imaging::RawFrameLease& lease, const metadata::FrameMetadataSnapshot& metadata,
                               uint32_t rawWidth, uint32_t rawHeight) {
    if (!accepting() || !lease || !lease.ahb()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return false;
    if (!crop_) {
        const auto& camera = metadata.cameraContext;
        if (!camera) return false;
        if (camera->cfaBlock > 1) {
            fail("unsupported: quad-Bayer RAW is remosaiced on the GPU only");
            return false;
        }
        const McrawCrop crop = mcrawCropFor(rawWidth, rawHeight, settings_.videoWidth, settings_.videoHeight);
        if (!crop.valid()) {
            fail("unsupported: " + std::to_string(settings_.videoWidth) + "x" + std::to_string(settings_.videoHeight) +
                 " does not fit the " + std::to_string(rawWidth) + "x" + std::to_string(rawHeight) + " RAW stream");
            return false;
        }
        crop_ = crop;
        camera_ = camera;
        __android_log_print(ANDROID_LOG_INFO, kTag, "MCRAW_START raw=%ux%u stored=%ux%u at %u,%u binned=%d", rawWidth,
                            rawHeight, crop.width, crop.height, crop.x, crop.y, crop.binned ? 1 : 0);
    }
    if (rawWidth != crop_->rawWidth || rawHeight != crop_->rawHeight || metadata.cameraContext != camera_) {
        ++framesDropped_;  // The camera changed mid-recording; the clip keeps its first geometry.
        return false;
    }
    if (pendingImages_.size() >= kMaxPendingImages || inFlight_ >= kMaxInFlight) {
        ++framesDropped_;
        return false;
    }
    Pending pending;
    pending.lease = std::move(lease);
    pending.metadata = metadata;
    pending.sequence = nextSequence_++;
    pendingImages_.push_back(std::move(pending));
    ++inFlight_;
    changed_.notify_all();
    return true;
}

void McrawRecorder::addAudio(int64_t firstFrameBootNs, const int16_t* samples, size_t frames, int channels) {
    if (!accepting() || !samples || frames == 0 || channels < 1) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    if (audioChannels_ == 0) audioChannels_ = channels;
    if (channels != audioChannels_ || audio_.size() >= kMaxQueuedAudioChunks) return;
    audio_.push_back({firstFrameBootNs, std::vector<int16_t>(samples, samples + frames * size_t(channels))});
    changed_.notify_all();
}

void McrawRecorder::addMotion(const std::vector<MotionSample>& samples) {
    if (!accepting() || samples.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || motion_.size() + samples.size() > kMaxQueuedMotion) return;
    motion_.insert(motion_.end(), samples.begin(), samples.end());
}

void McrawRecorder::copyLoop() noexcept {
    std::vector<uint8_t> packed;
    for (;;) {
        Pending pending;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait(lock, [this] { return !pendingImages_.empty() || stopping_; });
            if (pendingImages_.empty()) break;
            pending = std::move(pendingImages_.front());
            pendingImages_.pop_front();
        }
        const McrawCrop crop = *crop_;  // settled before the first frame was queued
        const auto started = std::chrono::steady_clock::now();
        packed.resize(size_t(crop.rawWidth) * crop.rawHeight * sizeof(uint16_t));
        // The frame's GPU work has retired, so there is no fence to wait on.
        const auto copied = imaging::copyCompletedRaw16AhbToPacked(pending.lease.ahb(), crop.rawWidth, crop.rawHeight,
                                                                   packed, -1, pending.lease.image());
        pending.lease.reset();  // Back to the camera as soon as the pixels are ours.
        Raw raw;
        raw.sequence = pending.sequence;
        raw.timestampNs = static_cast<int64_t>(pending.metadata.timestampNs);
        if (copied.success) {
            raw.pixels.resize(size_t(crop.width) * crop.height);
            extractMcrawFrame(reinterpret_cast<const uint16_t*>(packed.data()), crop.rawWidth, crop, raw.pixels.data());
        }
        raw.metadata = std::move(pending.metadata);
        std::lock_guard<std::mutex> lock(mutex_);
        if (!copied.success) {
            ++framesFailed_;
            encoded_[raw.sequence].skip = true;  // keeps the writer's sequence whole
            __android_log_print(ANDROID_LOG_WARN, kTag, "MCRAW_COPY_FAILED %s", copied.error.c_str());
        } else {
            copyMsTotal_ += msSince(started);
            raw_.push_back(std::move(raw));
        }
        changed_.notify_all();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    copyDone_ = true;
    changed_.notify_all();
}

void McrawRecorder::encodeLoop() noexcept {
    for (;;) {
        Raw raw;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait(lock, [this] { return !raw_.empty() || copyDone_; });
            if (raw_.empty()) break;
            raw = std::move(raw_.front());
            raw_.pop_front();
        }
        const McrawCrop crop = *crop_;
        const auto started = std::chrono::steady_clock::now();
        Encoded encoded;
        encoded.timestampNs = raw.timestampNs;
        try {
            rawr::mcraw::encodeFrame(raw.pixels.data(), crop.width, crop.height, crop.width, encoded.data);
            encoded.json = mcrawFrameJson(raw.metadata, raw.timestampNs, crop);
        } catch (const std::exception& error) {
            encoded.skip = true;
            __android_log_print(ANDROID_LOG_WARN, kTag, "MCRAW_ENCODE_FAILED %s", error.what());
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (encoded.skip) ++framesFailed_;
        else encodeMsTotal_ += msSince(started);
        encoded_[raw.sequence] = std::move(encoded);
        changed_.notify_all();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (--encodersRunning_ == 0) encodeDone_ = true;
    changed_.notify_all();
}

void McrawRecorder::writeLoop() noexcept {
    rawr::mcraw::ContainerWriter writer(fd_);
    bool begun = false, broken = false;
    uint64_t next = 0;
    int64_t lastTimestamp = -1;
    std::deque<Audio> heldAudio;  // audio from before the first frame
    std::vector<MotionSample> heldMotion;
    const auto writeSide = [&](std::deque<Audio>& audio, std::vector<MotionSample>& motion) {
        for (const Audio& chunk : audio) writer.writeAudio(chunk.bootNs, chunk.samples.data(), chunk.samples.size());
        {
            std::lock_guard<std::mutex> lock(mutex_);
            audioChunks_ += audio.size();
            motionSamples_ += motion.size();
        }
        for (const MotionSample& s : motion) {
            const rawr::mcraw::MotionSample sample{s.bootNs, s.device[0], s.device[1], s.device[2], 0};
            if (s.type == 2) writer.addGyro(sample);
            else if (s.type == 3) writer.addAccelerometer(sample);
        }
        audio.clear();
        motion.clear();
    };
    for (;;) {
        Encoded encoded;
        bool haveFrame = false, finished = false;
        std::deque<Audio> audio;
        std::vector<MotionSample> motion;
        McrawClipInfo clip;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait(lock, [&] { return encoded_.count(next) != 0 || (encodeDone_ && copyDone_); });
            if (encoded_.count(next) == 0 && !encoded_.empty()) next = encoded_.begin()->first;
            if (auto it = encoded_.find(next); it != encoded_.end()) {
                encoded = std::move(it->second);
                encoded_.erase(it);
                haveFrame = true;
                ++next;
                --inFlight_;
            } else {
                finished = true;
            }
            audio.swap(audio_);
            motion.swap(motion_);
            clip = {settings_.deviceMake, settings_.deviceModel, settings_.software, settings_.fps, 48'000,
                    audioChannels_ ? audioChannels_ : settings_.audioChannels};
            changed_.notify_all();
        }
        if (broken) {
            if (finished) break;
            continue;  // keep draining so the producers never block on a dead file
        }
        try {
            if (!begun) {
                for (auto& chunk : audio) heldAudio.push_back(std::move(chunk));
                heldMotion.insert(heldMotion.end(), motion.begin(), motion.end());
                audio.clear();
                motion.clear();
                if (haveFrame && !encoded.skip) {
                    writer.begin(mcrawContainerJson(*camera_, *crop_, clip));
                    begun = true;
                    writeSide(heldAudio, heldMotion);
                }
            } else {
                writeSide(audio, motion);
            }
            if (haveFrame && !encoded.skip && encoded.timestampNs > lastTimestamp) {
                writer.writeFrame(encoded.timestampNs, encoded.data.data(), encoded.data.size(), encoded.json);
                lastTimestamp = encoded.timestampNs;
                std::lock_guard<std::mutex> lock(mutex_);
                ++framesWritten_;
                if (firstFrameNs_ < 0) firstFrameNs_ = encoded.timestampNs;
                lastFrameNs_ = encoded.timestampNs;
                bytesWritten_ = writer.bytesWritten();
            }
            if (finished) {
                if (begun) {
                    writer.finish();
                    (void)::fsync(fd_);
                }
                std::lock_guard<std::mutex> lock(mutex_);
                bytesWritten_ = writer.bytesWritten();
                complete_ = begun;
                if (!begun && status_ == "recording") status_ = "no frames recorded";
                break;
            }
        } catch (const std::exception& error) {
            broken = true;
            std::lock_guard<std::mutex> lock(mutex_);
            fail(std::string("failed: ") + error.what());
        }
        if (finished) break;
    }
}

std::string McrawRecorder::stop() {
    {
        std::lock_guard<std::mutex> lock(gActiveMutex);
        if (gActive.get() == this) gActive.reset();
    }
    accepting_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        changed_.notify_all();
    }
    if (copyThread_.joinable()) copyThread_.join();
    for (auto& encoder : encoders_)
        if (encoder.joinable()) encoder.join();
    if (writerThread_.joinable()) writerThread_.join();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (complete_ && status_ == "recording") status_ = "complete";
    }
    return statsJson();
}

std::string McrawRecorder::statsJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string status;
    for (const char c : status_) {
        if (c == '"' || c == '\\') status.push_back('\\');
        if (static_cast<unsigned char>(c) >= 0x20) status.push_back(c);
    }
    const double fps = framesWritten_ > 1 && lastFrameNs_ > firstFrameNs_
                           ? (framesWritten_ - 1) * 1e9 / double(lastFrameNs_ - firstFrameNs_)
                           : 0.0;
    char buffer[1024];
    std::snprintf(buffer, sizeof(buffer),
                  "{\"status\":\"%s\",\"complete\":%s,\"framesWritten\":%llu,\"framesDropped\":%llu,"
                  "\"framesFailed\":%llu,\"audioChunks\":%llu,\"motionSamples\":%llu,\"bytesWritten\":%llu,"
                  "\"inFlight\":%zu,\"averageFps\":%.3f,"
                  "\"copyMsAvg\":%.2f,\"encodeMsAvg\":%.2f,\"width\":%u,\"height\":%u,\"binned\":%s}",
                  status.c_str(), complete_ ? "true" : "false", static_cast<unsigned long long>(framesWritten_),
                  static_cast<unsigned long long>(framesDropped_), static_cast<unsigned long long>(framesFailed_),
                  static_cast<unsigned long long>(audioChunks_), static_cast<unsigned long long>(motionSamples_),
                  static_cast<unsigned long long>(bytesWritten_), inFlight_, fps,
                  framesWritten_ ? copyMsTotal_ / double(framesWritten_ + framesFailed_) : 0.0,
                  framesWritten_ ? encodeMsTotal_ / double(framesWritten_) : 0.0, crop_ ? crop_->width : 0u,
                  crop_ ? crop_->height : 0u, crop_ && crop_->binned ? "true" : "false");
    return buffer;
}

}  // namespace rawrcam::video
