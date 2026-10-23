#include "video/MotionInput.h"

#include <android/log.h>
#include <android/looper.h>
#include <android/sensor.h>

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

namespace rawrcam::video {
namespace {
constexpr char kTag[] = "RawrMotion";
constexpr int kLooperIdent = 1;
// Sensor HALs batch up to maxBatchReportLatency; the window covers that plus
// cross-sensor FIFO skew before samples are released in time order.
constexpr int64_t kMaxBatchLatencyUs = 20'000;
constexpr int64_t kReorderWindowNs = 60'000'000;
// Rate an app without HIGH_SAMPLING_RATE_SENSORS is limited to (200 Hz).
constexpr int32_t kCappedPeriodUs = 5'000;
constexpr int kEventBatch = 64;

int32_t periodFor(const ASensor* sensor, int32_t requestedUs) {
    return std::max(requestedUs, std::max<int32_t>(0, ASensor_getMinDelay(sensor)));
}

// Registers at the requested period, falling back to the unprivileged cap.
int32_t registerSensor(ASensorEventQueue* queue, const ASensor* sensor, int32_t requestedUs) {
    int32_t period = periodFor(sensor, requestedUs);
    if (ASensorEventQueue_registerSensor(queue, sensor, period, kMaxBatchLatencyUs) == 0) return period;
    period = std::max(period, kCappedPeriodUs);
    if (ASensorEventQueue_registerSensor(queue, sensor, period, kMaxBatchLatencyUs) == 0) return period;
    return -1;
}
}  // namespace

MotionInput::MotionInput(const Config& config, Sink sink) : config_(config), sink_(std::move(sink)) {
    thread_ = std::thread([this] { run(); });
    std::unique_lock<std::mutex> lock(startMutex_);
    startedCv_.wait(lock, [this] { return started_; });
    if (!startError_.empty()) {
        const std::string error = startError_;
        lock.unlock();
        running_.store(false);
        if (thread_.joinable()) thread_.join();
        if (ALooper* looper = looper_.exchange(nullptr)) ALooper_release(looper);
        throw std::runtime_error(error);
    }
}

MotionInput::~MotionInput() {
    running_.store(false);
    if (ALooper* looper = looper_.load()) ALooper_wake(looper);
    if (thread_.joinable()) thread_.join();
    if (ALooper* looper = looper_.exchange(nullptr)) ALooper_release(looper);
}

MotionInput::Stats MotionInput::stats() const {
    std::lock_guard<std::mutex> lock(statsMutex_);
    return stats_;
}

void MotionInput::deliver(std::vector<MotionSample>&& samples) {
    if (samples.empty() || !sink_) return;
    try {
        sink_(samples);
    } catch (const std::exception& error) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "motion sink failed: %s", error.what());
    }
}

void MotionInput::run() noexcept {
    ALooper* looper = ALooper_prepare(0);
    ASensorEventQueue* queue = nullptr;
    ASensorManager* manager = nullptr;
    const ASensor* gyro = nullptr;
    const ASensor* accelerometer = nullptr;
    const auto finishStart = [this](const std::string& error) {
        std::lock_guard<std::mutex> lock(startMutex_);
        startError_ = error;
        started_ = true;
        startedCv_.notify_all();
    };
    if (!looper) {
        finishStart("motion looper unavailable");
        return;
    }
    // Held until the destructor so ALooper_wake never races the thread's exit.
    ALooper_acquire(looper);
    looper_.store(looper);
    manager = ASensorManager_getInstanceForPackage(config_.packageName.empty() ? nullptr : config_.packageName.c_str());
    if (manager) gyro = ASensorManager_getDefaultSensor(manager, ASENSOR_TYPE_GYROSCOPE);
    if (!gyro) {
        finishStart("device has no gyroscope");
        return;
    }
    queue = ASensorManager_createEventQueue(manager, looper, kLooperIdent, nullptr, nullptr);
    if (!queue) {
        finishStart("sensor event queue unavailable");
        return;
    }
    const int32_t gyroPeriod = registerSensor(queue, gyro, config_.samplingPeriodUs);
    if (gyroPeriod < 0) {
        ASensorManager_destroyEventQueue(manager, queue);
        finishStart("gyroscope registration failed");
        return;
    }
    if (config_.accelerometer) {
        accelerometer = ASensorManager_getDefaultSensor(manager, ASENSOR_TYPE_ACCELEROMETER);
        if (accelerometer && registerSensor(queue, accelerometer, config_.samplingPeriodUs) < 0) accelerometer = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        const char* name = ASensor_getName(gyro);
        stats_.gyroName = name ? name : "";
        stats_.gyroPeriodUs = gyroPeriod;
        stats_.accelerometerActive = accelerometer != nullptr;
    }
    __android_log_print(ANDROID_LOG_INFO, kTag, "MOTION_START gyro=%s periodUs=%d accelerometer=%d",
                        ASensor_getName(gyro), gyroPeriod, accelerometer ? 1 : 0);
    finishStart({});

    MotionReorderBuffer reorder(kReorderWindowNs);
    ASensorEvent events[kEventBatch];
    const auto readEvents = [&] {
        ssize_t count = 0;
        while ((count = ASensorEventQueue_getEvents(queue, events, kEventBatch)) > 0) {
            std::lock_guard<std::mutex> lock(statsMutex_);
            for (ssize_t i = 0; i < count; ++i) {
                const ASensorEvent& event = events[i];
                MotionSample sample;
                sample.bootNs = event.timestamp;
                sample.device = {event.data[0], event.data[1], event.data[2]};
                if (event.type == ASENSOR_TYPE_GYROSCOPE) {
                    sample.type = 2;
                    if (stats_.firstGyroNs < 0) stats_.firstGyroNs = event.timestamp;
                    stats_.lastGyroNs = event.timestamp;
                    ++stats_.gyroSamples;
                } else if (event.type == ASENSOR_TYPE_ACCELEROMETER) {
                    sample.type = 3;
                    ++stats_.accelerometerSamples;
                } else {
                    continue;
                }
                reorder.push(sample);
            }
        }
    };
    while (running_.load()) {
        (void)ALooper_pollOnce(100, nullptr, nullptr, nullptr);
        readEvents();
        deliver(reorder.release());
    }
    (void)ASensorEventQueue_disableSensor(queue, gyro);
    if (accelerometer) (void)ASensorEventQueue_disableSensor(queue, accelerometer);
    readEvents();
    deliver(reorder.releaseAll());
    ASensorManager_destroyEventQueue(manager, queue);
}

}  // namespace rawrcam::video
