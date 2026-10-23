#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "video/MotionAxes.h"

struct ALooper;
struct ASensorEventQueue;
struct ASensor;

namespace rawrcam::video {

// Gyroscope and accelerometer for a recording, read with the NDK sensor queue on
// its own looper thread. Samples reach the sink sorted by CLOCK_BOOTTIME (the
// sensor event clock, shared with RAW frame timestamps when the camera reports a
// REALTIME timestamp source), in device axes, a few tens of ms after capture.
class MotionInput final {
   public:
    struct Config {
        // Context.getPackageName(); the sensor service checks it against the caller.
        std::string packageName;
        bool accelerometer = true;
        // Requested sensor period. The fastest the device offers is used when
        // shorter; Android caps unprivileged apps at 200 Hz without
        // HIGH_SAMPLING_RATE_SENSORS, which the manifest declares.
        int32_t samplingPeriodUs = 2'500;
    };
    using Sink = std::function<void(const std::vector<MotionSample>&)>;

    struct Stats {
        std::string gyroName;
        uint64_t gyroSamples = 0;
        uint64_t accelerometerSamples = 0;
        int64_t firstGyroNs = -1;
        int64_t lastGyroNs = -1;
        int32_t gyroPeriodUs = 0;
        bool accelerometerActive = false;

        [[nodiscard]] double gyroHz() const noexcept {
            return gyroSamples > 1 && lastGyroNs > firstGyroNs
                       ? (gyroSamples - 1) * 1e9 / static_cast<double>(lastGyroNs - firstGyroNs)
                       : 0.0;
        }
    };

    // Throws when the device has no gyroscope or the sensor queue cannot start.
    MotionInput(const Config& config, Sink sink);
    // Stops the sensors and delivers every held sample before returning.
    ~MotionInput();
    MotionInput(const MotionInput&) = delete;
    MotionInput& operator=(const MotionInput&) = delete;

    [[nodiscard]] Stats stats() const;

   private:
    void run() noexcept;
    void deliver(std::vector<MotionSample>&& samples);

    Config config_;
    Sink sink_;
    std::thread thread_;
    std::atomic<bool> running_{true};
    std::atomic<ALooper*> looper_{nullptr};
    std::mutex startMutex_;
    std::string startError_;
    bool started_ = false;
    std::condition_variable startedCv_;
    mutable std::mutex statsMutex_;
    Stats stats_;
};

}  // namespace rawrcam::video
