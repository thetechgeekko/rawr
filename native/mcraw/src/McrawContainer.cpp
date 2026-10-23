#include "mcraw/McrawContainer.h"

#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace rawr::mcraw {
namespace {
constexpr uint8_t kIdent[7] = {'M', 'O', 'T', 'I', 'O', 'N', ' '};
constexpr uint32_t kMotionVersion = 1;

struct Item {
    uint32_t type;
    uint32_t size;
};
struct BufferIndex {
    int32_t magicNumber;
    int32_t numOffsets;
    int64_t indexDataOffset;
};
struct AudioIndex {
    int64_t numOffsets;
    int64_t startTimestampMs;
};
struct MotionHeader {
    uint32_t version;
    uint32_t count;
};
static_assert(sizeof(Item) == 8 && sizeof(BufferIndex) == 16 && sizeof(AudioIndex) == 16 && sizeof(MotionHeader) == 8);

void fail(const char* what) { throw std::runtime_error(std::string(what) + " errno=" + std::to_string(errno)); }

bool readAt(int fd, uint64_t offset, void* out, size_t size) {
    auto* bytes = static_cast<uint8_t*>(out);
    size_t done = 0;
    while (done < size) {
        const ssize_t n = ::pread(fd, bytes + done, size - done, static_cast<off_t>(offset + done));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

// The integer "timestamp" member of a frame's JSON.
bool jsonTimestamp(const std::string& json, int64_t& out) {
    const char key[] = "\"timestamp\"";
    const size_t at = json.find(key);
    if (at == std::string::npos) return false;
    size_t pos = json.find(':', at + sizeof(key) - 1);
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '"')) ++pos;
    char* end = nullptr;
    const long long value = std::strtoll(json.c_str() + pos, &end, 10);
    if (end == json.c_str() + pos) return false;
    out = value;
    return true;
}
}  // namespace

ContainerWriter::ContainerWriter(int fd) : fd_(fd) {
    if (fd_ < 0) throw std::invalid_argument("MCRAW writer needs a writable fd");
}

void ContainerWriter::write(const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t done = 0;
    while (done < size) {
        const ssize_t n = ::write(fd_, bytes + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) fail("MCRAW write failed");
        done += static_cast<size_t>(n);
    }
    position_ += size;
}

void ContainerWriter::writeItem(ItemType type, const void* payload, size_t size) {
    if (size > UINT32_MAX) throw std::length_error("MCRAW item too large");
    const Item item{static_cast<uint32_t>(type), static_cast<uint32_t>(size)};
    write(&item, sizeof(item));
    if (size) write(payload, size);
}

void ContainerWriter::begin(const std::string& containerJson) {
    if (begun_) throw std::logic_error("MCRAW container already begun");
    uint8_t header[8];
    std::memcpy(header, kIdent, sizeof(kIdent));
    header[7] = kContainerVersion;
    write(header, sizeof(header));
    writeItem(ItemType::Metadata, containerJson.data(), containerJson.size());
    begun_ = true;
}

void ContainerWriter::writeFrame(int64_t timestampNs, const uint8_t* data, size_t size, const std::string& frameJson) {
    if (!begun_ || finished_) throw std::logic_error("MCRAW container not writable");
    if (!frameIndex_.empty() && timestampNs <= frameIndex_.back().timestamp)
        throw std::invalid_argument("MCRAW frame timestamps must increase");
    const auto offset = static_cast<int64_t>(position_);
    writeItem(ItemType::Buffer, data, size);
    writeItem(ItemType::Metadata, frameJson.data(), frameJson.size());
    frameIndex_.push_back({offset, timestampNs});
    flushMotion();
}

void ContainerWriter::writeAudio(int64_t timestampNs, const int16_t* samples, size_t sampleCount) {
    if (!begun_ || finished_) throw std::logic_error("MCRAW container not writable");
    if (sampleCount == 0) return;
    const auto offset = static_cast<int64_t>(position_);
    writeItem(ItemType::AudioData, samples, sampleCount * sizeof(int16_t));
    writeItem(ItemType::AudioDataMetadata, &timestampNs, sizeof(timestampNs));
    audioIndex_.push_back({offset, timestampNs});
    if (firstAudioTimestampNs_ < 0) firstAudioTimestampNs_ = timestampNs;
}

void ContainerWriter::writeMotionChunk(ItemType type, std::vector<MotionSample>& samples, std::vector<Offset>& index) {
    if (samples.empty()) return;
    const auto offset = static_cast<int64_t>(position_);
    const MotionHeader header{kMotionVersion, static_cast<uint32_t>(samples.size())};
    const Item item{static_cast<uint32_t>(type),
                    static_cast<uint32_t>(sizeof(header) + samples.size() * sizeof(MotionSample))};
    write(&item, sizeof(item));
    write(&header, sizeof(header));
    write(samples.data(), samples.size() * sizeof(MotionSample));
    index.push_back({offset, samples.front().timestampNs});
    samples.clear();
}

void ContainerWriter::flushMotion() {
    writeMotionChunk(ItemType::GyroData, gyroPending_, gyroIndex_);
    writeMotionChunk(ItemType::AccelerometerData, accelerometerPending_, accelerometerIndex_);
}

void ContainerWriter::writeTail() {
    // readExtra() walks forward from the last frame and stops at the first item it
    // does not know, so every index precedes BUFFER_INDEX_DATA.
    if (!audioIndex_.empty()) {
        const AudioIndex header{static_cast<int64_t>(audioIndex_.size()),
                                firstAudioTimestampNs_ < 0 ? 0 : firstAudioTimestampNs_ / 1'000'000};
        const Item item{static_cast<uint32_t>(ItemType::AudioIndex),
                        static_cast<uint32_t>(sizeof(header) + audioIndex_.size() * sizeof(Offset))};
        write(&item, sizeof(item));
        write(&header, sizeof(header));
        write(audioIndex_.data(), audioIndex_.size() * sizeof(Offset));
    }
    for (auto [type, index] : {std::pair{ItemType::GyroIndex, &gyroIndex_},
                               std::pair{ItemType::AccelerometerIndex, &accelerometerIndex_}}) {
        if (index->empty()) continue;
        const MotionHeader header{kMotionVersion, static_cast<uint32_t>(index->size())};
        const Item item{static_cast<uint32_t>(type),
                        static_cast<uint32_t>(sizeof(header) + index->size() * sizeof(Offset))};
        write(&item, sizeof(item));
        write(&header, sizeof(header));
        write(index->data(), index->size() * sizeof(Offset));
    }
    const Item data{static_cast<uint32_t>(ItemType::BufferIndexData),
                    static_cast<uint32_t>(frameIndex_.size() * sizeof(Offset))};
    write(&data, sizeof(data));
    const auto indexDataOffset = static_cast<int64_t>(position_);
    write(frameIndex_.data(), frameIndex_.size() * sizeof(Offset));
    const BufferIndex index{static_cast<int32_t>(kIndexMagic), static_cast<int32_t>(frameIndex_.size()),
                            indexDataOffset};
    writeItem(ItemType::BufferIndex, &index, sizeof(index));
}

void ContainerWriter::finish() {
    if (!begun_ || finished_) throw std::logic_error("MCRAW container not writable");
    flushMotion();
    writeTail();
    finished_ = true;
}

size_t ContainerWriter::recover(int fd) {
    uint8_t header[8];
    Item item{};
    if (!readAt(fd, 0, header, sizeof(header)) || std::memcmp(header, kIdent, sizeof(kIdent)) != 0 ||
        header[7] != kContainerVersion)
        throw std::runtime_error("not an MCRAW file");
    if (!readAt(fd, 8, &item, sizeof(item)) || item.type != static_cast<uint32_t>(ItemType::Metadata))
        throw std::runtime_error("MCRAW file has no container metadata");
    const off_t fileSize = ::lseek(fd, 0, SEEK_END);
    if (fileSize < 0) fail("MCRAW seek failed");
    const auto size = static_cast<uint64_t>(fileSize);

    ContainerWriter writer(fd);
    writer.begun_ = true;
    uint64_t pos = 16 + uint64_t(item.size);
    uint64_t good = pos;
    if (good > size) throw std::runtime_error("MCRAW container metadata is truncated");
    const auto fits = [size](uint64_t at, const Item& it) { return at + sizeof(Item) + it.size <= size; };
    while (pos + sizeof(Item) <= size && readAt(fd, pos, &item, sizeof(item)) && fits(pos, item)) {
        const auto type = static_cast<ItemType>(item.type);
        const uint64_t next = pos + sizeof(Item) + item.size;
        Item second{};
        if (type == ItemType::Buffer) {
            if (next + sizeof(Item) > size || !readAt(fd, next, &second, sizeof(second)) ||
                second.type != static_cast<uint32_t>(ItemType::Metadata) || !fits(next, second))
                break;
            std::string json(second.size, '\0');
            int64_t timestamp = 0;
            if (!readAt(fd, next + sizeof(Item), json.data(), json.size()) || !jsonTimestamp(json, timestamp) ||
                (!writer.frameIndex_.empty() && timestamp <= writer.frameIndex_.back().timestamp))
                break;
            writer.frameIndex_.push_back({static_cast<int64_t>(pos), timestamp});
            pos = next + sizeof(Item) + second.size;
        } else if (type == ItemType::AudioData) {
            int64_t timestamp = 0;
            if (next + sizeof(Item) + sizeof(timestamp) > size || !readAt(fd, next, &second, sizeof(second)) ||
                second.type != static_cast<uint32_t>(ItemType::AudioDataMetadata) || second.size != sizeof(timestamp) ||
                !readAt(fd, next + sizeof(Item), &timestamp, sizeof(timestamp)))
                break;
            writer.audioIndex_.push_back({static_cast<int64_t>(pos), timestamp});
            if (writer.firstAudioTimestampNs_ < 0) writer.firstAudioTimestampNs_ = timestamp;
            pos = next + sizeof(Item) + sizeof(timestamp);
        } else if (type == ItemType::GyroData || type == ItemType::AccelerometerData) {
            MotionHeader motion{};
            MotionSample first{};
            if (item.size < sizeof(motion) + sizeof(first) || !readAt(fd, pos + sizeof(Item), &motion, sizeof(motion)) ||
                motion.version != kMotionVersion || motion.count == 0 ||
                sizeof(motion) + uint64_t(motion.count) * sizeof(MotionSample) != item.size ||
                !readAt(fd, pos + sizeof(Item) + sizeof(motion), &first, sizeof(first)))
                break;
            (type == ItemType::GyroData ? writer.gyroIndex_ : writer.accelerometerIndex_)
                .push_back({static_cast<int64_t>(pos), first.timestampNs});
            pos = next;
        } else {
            break;  // An index (a finished file) or damage: rebuild the tail from here.
        }
        good = pos;
    }
    if (writer.frameIndex_.empty()) throw std::runtime_error("MCRAW file has no complete frame");
    // At most one motion chunk follows each frame; a chunk past the last frame is the +1 the decoder allows.
    if (::ftruncate(fd, static_cast<off_t>(good)) != 0 || ::lseek(fd, static_cast<off_t>(good), SEEK_SET) < 0)
        fail("MCRAW truncate failed");
    writer.position_ = good;
    writer.writeTail();
    writer.finished_ = true;
    return writer.frameIndex_.size();
}

}  // namespace rawr::mcraw
