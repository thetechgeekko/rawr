#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rawr::mcraw {

// Writes MotionCam's .mcraw container (version 3), the layout the Apache-2.0
// reference decoder (github.com/mirsadm/motioncam-decoder) reads:
//
//   "MOTION " + version 3
//   METADATA item: container JSON
//   per frame: BUFFER item (encoded frame) + METADATA item (frame JSON)
//   interleaved: AUDIO_DATA (int16) + AUDIO_DATA_METADATA (timestamp),
//                GYRO_DATA / ACCELEROMETER_DATA chunks
//   tail: AUDIO_INDEX, GYRO_INDEX, ACCELEROMETER_INDEX, BUFFER_INDEX_DATA, BUFFER_INDEX
//
// Items are {u32 type, u32 size} then the payload; all little-endian. The
// decoder locates the tail from the end of the file, so an interrupted file
// is unreadable until recover() appends one.
enum class ItemType : uint32_t {
    BufferIndex = 0,
    BufferIndexData = 1,
    Buffer = 2,
    Metadata = 3,
    AudioIndex = 4,
    AudioData = 5,
    AudioDataMetadata = 6,
    GyroIndex = 8,
    GyroData = 9,
    AccelerometerIndex = 12,
    AccelerometerData = 13,
};

inline constexpr uint8_t kContainerVersion = 3;
inline constexpr uint32_t kIndexMagic = 0x8A905612;

// One IMU sample in the container's 24-byte layout.
struct MotionSample {
    int64_t timestampNs = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    uint32_t reserved = 0;
};
static_assert(sizeof(MotionSample) == 24);

// Writes sequentially to a seekable file descriptor it does not own. Not
// thread safe: one owner thread calls every method.
class ContainerWriter final {
   public:
    explicit ContainerWriter(int fd);
    ContainerWriter(const ContainerWriter&) = delete;
    ContainerWriter& operator=(const ContainerWriter&) = delete;

    // Header and container JSON. Must come first.
    void begin(const std::string& containerJson);
    // One encoded frame and its JSON. Timestamps must increase; the decoder keys
    // frames by them. The JSON must carry the same value as an integer
    // "timestamp" member, which recover() reads back. Buffered motion is written after each frame, so the gyro
    // and accelerometer indexes never exceed frames + 1 chunks (a decoder limit).
    void writeFrame(int64_t timestampNs, const uint8_t* data, size_t size, const std::string& frameJson);
    // Interleaved signed 16-bit PCM; timestampNs is the first sample's time.
    void writeAudio(int64_t timestampNs, const int16_t* samples, size_t sampleCount);
    // Buffered until the next frame (or finish()); must be in time order per kind.
    void addGyro(const MotionSample& sample) { gyroPending_.push_back(sample); }
    void addAccelerometer(const MotionSample& sample) { accelerometerPending_.push_back(sample); }
    // Flushes motion, appends the indexes. No method may be called afterwards.
    void finish();

    [[nodiscard]] uint64_t bytesWritten() const noexcept { return position_; }
    [[nodiscard]] size_t frames() const noexcept { return frameIndex_.size(); }

    // Completes a file whose recording stopped before finish(): scans its items,
    // truncates a partial trailing item, and appends the tail. Returns the frame
    // count. Throws if the file has no valid header or no complete frame.
    static size_t recover(int fd);

   private:
    struct Offset {
        int64_t offset;
        int64_t timestamp;
    };
    void write(const void* data, size_t size);
    void writeItem(ItemType type, const void* payload, size_t size);
    void flushMotion();
    void writeMotionChunk(ItemType type, std::vector<MotionSample>& samples, std::vector<Offset>& index);
    void writeTail();

    int fd_;
    uint64_t position_ = 0;
    bool begun_ = false;
    bool finished_ = false;
    std::vector<Offset> frameIndex_;
    std::vector<Offset> audioIndex_;
    std::vector<Offset> gyroIndex_;
    std::vector<Offset> accelerometerIndex_;
    std::vector<MotionSample> gyroPending_;
    std::vector<MotionSample> accelerometerPending_;
    int64_t firstAudioTimestampNs_ = -1;
};

}  // namespace rawr::mcraw
