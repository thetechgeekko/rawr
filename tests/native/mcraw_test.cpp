// MCRAW codec and container. Frames must round-trip bit-exactly through the
// library's decoder and, when the build provides it (RAWR_WITH_MOTIONCAM_DECODER),
// through MotionCam's own reference decoder, which also reads back the
// container's audio, gyroscope and accelerometer streams.
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "mcraw/McrawContainer.h"
#include "mcraw/McrawEncoder.h"

#ifdef RAWR_WITH_MOTIONCAM_DECODER
#include <motioncam/Decoder.hpp>
#endif

namespace {
int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

struct Frame {
    uint32_t width, height;
    size_t stride;
    std::vector<uint16_t> pixels;  // stride * height
};

// A Bayer-like frame: per-CFA-phase offsets, tiles whose spans walk every bit width
// 0..16, and padding past `width` in each row that must never reach the output.
Frame makeFrame(uint32_t width, uint32_t height, size_t stride, uint32_t seed) {
    Frame f{width, height, stride, std::vector<uint16_t>(stride * height, 0xBEEF)};
    std::mt19937 rng(seed);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t tile = (x / 64) + (y / 4) * 7;
            const uint32_t bits = tile % 17;  // 0..16
            const uint32_t span = bits == 0 ? 0 : (bits == 16 ? 0xFFFFu : (1u << bits) - 1u);
            const uint32_t base = bits == 16 ? 0 : 64u * ((x & 1u) + 2u * (y & 1u)) + 17u;
            uint32_t value = base + (span ? rng() % (span + 1) : 0);
            if (value > 0xFFFF) value = 0xFFFF;
            f.pixels[size_t(y) * stride + x] = static_cast<uint16_t>(value);
        }
    }
    return f;
}

bool sameAsSource(const Frame& f, const std::vector<uint16_t>& decoded) {
    if (decoded.size() != size_t(f.width) * f.height) return false;
    for (uint32_t y = 0; y < f.height; ++y)
        for (uint32_t x = 0; x < f.width; ++x)
            if (decoded[size_t(y) * f.width + x] != f.pixels[size_t(y) * f.stride + x]) return false;
    return true;
}

std::string frameJson(const Frame& f, int64_t timestamp) {
    return "{\"timestamp\":" + std::to_string(timestamp) + ",\"width\":" + std::to_string(f.width) +
           ",\"height\":" + std::to_string(f.height) + ",\"compressionType\":7}";
}
}  // namespace

int main() {
    using namespace rawr::mcraw;

    // --- Codec ---
    check(!encodableSize(63, 8) && !encodableSize(64, 6) && encodableSize(2, 4) && encodableSize(130, 12),
          "encodable sizes");
    std::vector<Frame> frames;
    const uint32_t sizes[][3] = {{2, 4, 2}, {64, 4, 64}, {66, 8, 80}, {130, 12, 130}, {640, 48, 700}, {1000, 20, 1000}};
    uint32_t seed = 1;
    for (const auto& s : sizes) {
        const Frame f = makeFrame(s[0], s[1], s[2], seed++);
        std::vector<uint8_t> encoded;
        encodeFrame(f.pixels.data(), f.width, f.height, f.stride, encoded);
        check(encoded.size() <= maxEncodedBytes(f.width, f.height), "encoded size within bound");
        std::vector<uint16_t> decoded;
        const std::string name = std::to_string(f.width) + "x" + std::to_string(f.height);
        check(decodeFrame(encoded.data(), encoded.size(), f.width, f.height, decoded) && sameAsSource(f, decoded),
              "library round-trip " + name);
        frames.push_back(f);
    }
    {
        // A flat frame packs to almost nothing; full-range noise stays bounded.
        Frame flat{128, 8, 128, std::vector<uint16_t>(128 * 8, 1023)};
        std::vector<uint8_t> encoded;
        encodeFrame(flat.pixels.data(), 128, 8, 128, encoded);
        check(encoded.size() < 200, "flat frame compresses (" + std::to_string(encoded.size()) + " bytes)");
        bool threw = false;
        try {
            encodeFrame(flat.pixels.data(), 127, 8, 128, encoded);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        check(threw, "odd width rejected");
    }

    // --- Container ---
    const char* tmp = std::getenv("TMPDIR");
    std::string pattern = std::string(tmp ? tmp : "/tmp") + "/mcraw-test-XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    const int fd = mkstemp(name.data());
    if (fd < 0) {
        std::printf("FAIL mkstemp\n");
        return 1;
    }
    const std::string container =
        "{\"blackLevel\":[64,64,64,64],\"whiteLevel\":1023,\"sensorArrangment\":\"rggb\","
        "\"extraData\":{\"audioSampleRate\":48000,\"audioChannels\":2}}";
    std::vector<std::vector<uint8_t>> encodedFrames;
    std::vector<int64_t> timestamps;
    std::vector<int16_t> audio(2048);
    for (size_t i = 0; i < audio.size(); ++i) audio[i] = static_cast<int16_t>(int(i * 37) - 30000);
    {
        ContainerWriter writer(fd);
        writer.begin(container);
        writer.writeAudio(5'000'000, audio.data(), audio.size());  // audio may precede the first frame
        int64_t ts = 10'000'000;
        for (size_t i = 0; i < frames.size(); ++i, ts += 33'333'333) {
            for (int k = 0; k < 7; ++k) {
                writer.addGyro({ts + k * 5'000'000, float(i), float(k), -1.5f, 0});
                if (k % 2 == 0) writer.addAccelerometer({ts + k * 5'000'000, 0.0f, 9.81f, float(k), 0});
            }
            std::vector<uint8_t> encoded;
            encodeFrame(frames[i].pixels.data(), frames[i].width, frames[i].height, frames[i].stride, encoded);
            writer.writeFrame(ts, encoded.data(), encoded.size(), frameJson(frames[i], ts));
            writer.writeAudio(ts + 1'000'000, audio.data(), audio.size());
            encodedFrames.push_back(encoded);
            timestamps.push_back(ts);
        }
        writer.addGyro({ts, 9.0f, 9.0f, 9.0f, 0});  // after the last frame: the +1 chunk
        bool threw = false;
        try {
            writer.writeFrame(timestamps.back(), encodedFrames[0].data(), encodedFrames[0].size(), "{}");
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        check(threw, "repeated timestamp rejected");
        writer.finish();
        check(writer.frames() == frames.size(), "frame count");
    }
    const off_t finishedSize = lseek(fd, 0, SEEK_END);

    // Recovery: chop the tail and part of the last frame's JSON; recover() keeps the
    // complete frames and rebuilds an index the decoder accepts.
    std::string recoveredPath = std::string(name.data()) + ".recovered";
    {
        std::vector<uint8_t> bytes(static_cast<size_t>(finishedSize));
        check(pread(fd, bytes.data(), bytes.size(), 0) == finishedSize, "read back");
        const int rfd = open(recoveredPath.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
        // Cut 40 bytes into the indexes plus everything after: the last frame stays whole.
        const size_t cut = bytes.size() - 200;
        check(write(rfd, bytes.data(), cut) == ssize_t(cut), "write truncated copy");
        size_t recovered = 0;
        try {
            recovered = ContainerWriter::recover(rfd);
        } catch (const std::exception& e) {
            std::printf("recover threw: %s\n", e.what());
        }
        check(recovered == frames.size(), "recover keeps all whole frames (" + std::to_string(recovered) + ")");
        close(rfd);
    }

#ifdef RAWR_WITH_MOTIONCAM_DECODER
    for (const std::string& file : {std::string(name.data()), recoveredPath}) {
        try {
            motioncam::Decoder decoder(file);
            const auto& list = decoder.getFrames();
            check(list.size() == frames.size(), file + ": reference decoder frame count");
            for (size_t i = 0; i < list.size() && i < frames.size(); ++i) {
                std::vector<uint8_t> pixels;
                nlohmann::json metadata;
                decoder.loadFrame(list[i], pixels, metadata);
                check(list[i] == timestamps[i], "reference timestamp " + std::to_string(i));
                std::vector<uint16_t> values(pixels.size() / 2);
                std::memcpy(values.data(), pixels.data(), pixels.size());
                check(sameAsSource(frames[i], values), file + ": reference decoder pixels, frame " + std::to_string(i));
            }
            check(decoder.audioSampleRateHz() == 48000 && decoder.numAudioChannels() == 2, "audio format");
            std::vector<motioncam::AudioChunk> chunks;
            decoder.loadAudio(chunks);
            check(chunks.size() == frames.size() + 1, file + ": audio chunk count " + std::to_string(chunks.size()));
            if (!chunks.empty())
                check(chunks[0].first == 5'000'000 && chunks[0].second == audio, file + ": audio samples");
            std::vector<motioncam::MotionSample> gyro, accelerometer;
            check(decoder.hasGyroData() && decoder.hasAccelerometerData(), file + ": motion streams present");
            decoder.loadGyroData(gyro);
            decoder.loadAccelerometerData(accelerometer);
            const size_t gyroWant = frames.size() * 7 + 1;  // the cut lands in the indexes, after the +1 chunk
            check(gyro.size() == gyroWant, file + ": gyro samples " + std::to_string(gyro.size()));
            check(accelerometer.size() == frames.size() * 4, file + ": accelerometer samples");
            if (gyro.size() > 3) check(gyro[3].y == 3.0f && gyro[3].z == -1.5f, file + ": gyro values");
        } catch (const std::exception& e) {
            check(false, file + ": reference decoder threw: " + e.what());
        }
    }
    std::printf("reference decoder cross-check ran\n");
#endif

    close(fd);
    unlink(name.data());
    unlink(recoveredPath.c_str());
    if (failures == 0) std::printf("mcraw_test ok\n");
    return failures == 0 ? 0 : 1;
}
