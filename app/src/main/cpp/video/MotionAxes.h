#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace rawrcam::video {

// How the camera sensor is mounted, as Camera2 reports it. Video frames are coded
// in the sensor's native buffer orientation (the container carries the display
// rotation), so motion is expressed in that buffer's axes.
struct CameraMount {
    int sensorOrientationDegrees = 90;  // SENSOR_ORIENTATION, clockwise
    bool frontFacing = false;
};

// Android device axes (natural orientation: X right, Y up, Z out of the screen)
// to CAMM camera axes for the coded image (X right, Y down, Z forward).
//
// An upright, unmirrored image of the native buffer, rotated clockwise by the
// sensor orientation, has right = +X and down = -Y for a back camera; a front
// camera faces the user, so its right is -X. Forward is -Z (back) or +Z (front).
// The buffer's +x axis is that upright frame's right rotated clockwise by the
// sensor orientation, and +y is its down rotated the same way.
inline std::array<float, 3> deviceToCameraAxes(const CameraMount& mount, const std::array<float, 3>& device) noexcept {
    const int quarter = ((mount.sensorOrientationDegrees % 360 + 360) % 360) / 90;
    static constexpr int kCos[4] = {1, 0, -1, 0};
    static constexpr int kSin[4] = {0, 1, 0, -1};
    const float c = static_cast<float>(kCos[quarter]);
    const float s = static_cast<float>(kSin[quarter]);
    const std::array<float, 3> right{mount.frontFacing ? -1.0f : 1.0f, 0.0f, 0.0f};
    const std::array<float, 3> down{0.0f, -1.0f, 0.0f};
    const std::array<float, 3> forward{0.0f, 0.0f, mount.frontFacing ? 1.0f : -1.0f};
    std::array<float, 3> axisX{}, axisY{};
    for (int i = 0; i < 3; ++i) {
        axisX[i] = c * right[i] + s * down[i];
        axisY[i] = -s * right[i] + c * down[i];
    }
    const auto dot = [&device](const std::array<float, 3>& axis) {
        return axis[0] * device[0] + axis[1] * device[1] + axis[2] * device[2];
    };
    return {dot(axisX), dot(axisY), dot(forward)};
}

// One IMU reading on CLOCK_BOOTTIME in device axes. type uses the CAMM ids
// (2 gyroscope rad/s, 3 accelerometer m/s^2).
struct MotionSample {
    int64_t bootNs = 0;
    uint16_t type = 0;
    std::array<float, 3> device{0.0f, 0.0f, 0.0f};
};

// Sensor FIFOs deliver each sensor in order, but batches from different sensors
// can interleave out of order. Holding samples for a short window and releasing
// them sorted gives the monotonic stream CAMM needs.
class MotionReorderBuffer {
   public:
    explicit MotionReorderBuffer(int64_t windowNs) : windowNs_(windowNs) {}

    void push(const MotionSample& sample) {
        pending_.push_back(sample);
        newestNs_ = std::max(newestNs_, sample.bootNs);
    }

    // Samples older than the newest minus the window, sorted by time (stable for ties).
    std::vector<MotionSample> release() {
        if (pending_.empty()) return {};
        return take(newestNs_ - windowNs_);
    }
    // Everything still held, sorted; for the end of a recording.
    std::vector<MotionSample> releaseAll() { return take(INT64_MAX); }
    [[nodiscard]] size_t pending() const noexcept { return pending_.size(); }

   private:
    std::vector<MotionSample> take(int64_t limitNs) {
        std::stable_sort(pending_.begin(), pending_.end(),
                         [](const MotionSample& a, const MotionSample& b) { return a.bootNs < b.bootNs; });
        const auto split = std::find_if(pending_.begin(), pending_.end(),
                                        [limitNs](const MotionSample& s) { return s.bootNs > limitNs; });
        std::vector<MotionSample> out(pending_.begin(), split);
        pending_.erase(pending_.begin(), split);
        return out;
    }

    int64_t windowNs_;
    int64_t newestNs_ = INT64_MIN;
    std::vector<MotionSample> pending_;
};

}  // namespace rawrcam::video
