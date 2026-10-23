// Device-to-camera IMU axes for CAMM, and the reorder buffer that makes the
// sensor stream monotonic.
#include <cmath>
#include <cstdio>

#include "video/MotionAxes.h"

namespace {
using rawrcam::video::CameraMount;
using rawrcam::video::deviceToCameraAxes;
using rawrcam::video::MotionReorderBuffer;
using rawrcam::video::MotionSample;
using Vec = std::array<float, 3>;

int failures = 0;
void check(bool ok, const char* what, int orientation, bool front) {
    if (!ok) {
        ++failures;
        std::printf("FAIL %s (orientation %d, %s)\n", what, orientation, front ? "front" : "back");
    }
}
bool near(const Vec& a, const Vec& b) {
    for (int i = 0; i < 3; ++i)
        if (std::fabs(a[i] - b[i]) > 1e-6f) return false;
    return true;
}
}  // namespace

int main() {
    // Typical back camera, sensor at 90: the buffer's +x is the phone's -Y (towards the
    // bottom edge), +y is the phone's -X, and the lens looks along -Z.
    {
        const CameraMount mount{90, false};
        check(near(deviceToCameraAxes(mount, {0, -1, 0}), {1, 0, 0}), "back 90: -Y is image right", 90, false);
        check(near(deviceToCameraAxes(mount, {-1, 0, 0}), {0, 1, 0}), "back 90: -X is image down", 90, false);
        check(near(deviceToCameraAxes(mount, {0, 0, -1}), {0, 0, 1}), "back 90: -Z is forward", 90, false);
    }
    // Typical front camera, sensor at 270, looking at the user (+Z).
    {
        const CameraMount mount{270, true};
        check(near(deviceToCameraAxes(mount, {0, 0, 1}), {0, 0, 1}), "front 270: +Z is forward", 270, true);
        // Upright right for a front camera is -X; rotated 270 clockwise it becomes up (+Y).
        check(near(deviceToCameraAxes(mount, {0, 1, 0}), {1, 0, 0}), "front 270: +Y is image right", 270, true);
    }
    // Every mount maps device axes to a right-handed orthonormal camera basis, and an
    // unrotated sensor (0) keeps the upright frame.
    for (int orientation : {0, 90, 180, 270, -90, 450}) {
        for (bool front : {false, true}) {
            const CameraMount mount{orientation, front};
            const Vec x = deviceToCameraAxes(mount, {1, 0, 0});
            const Vec y = deviceToCameraAxes(mount, {0, 1, 0});
            const Vec z = deviceToCameraAxes(mount, {0, 0, 1});
            // Rows of the device->camera matrix are the camera axes in device space; columns are x, y, z.
            const float det = x[0] * (y[1] * z[2] - y[2] * z[1]) - y[0] * (x[1] * z[2] - x[2] * z[1]) +
                              z[0] * (x[1] * y[2] - x[2] * y[1]);
            check(std::fabs(det - 1.0f) < 1e-6f, "basis is a proper rotation", orientation, front);
            float norm = 0;
            for (const Vec& v : {x, y, z}) norm += v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
            check(std::fabs(norm - 3.0f) < 1e-6f, "basis is orthonormal", orientation, front);
        }
    }
    check(near(deviceToCameraAxes({0, false}, {1, 0, 0}), {1, 0, 0}), "back 0: +X is right", 0, false);
    check(near(deviceToCameraAxes({0, false}, {0, 1, 0}), {0, -1, 0}), "back 0: +Y is up", 0, false);
    check(near(deviceToCameraAxes({0, true}, {1, 0, 0}), {-1, 0, 0}), "front 0: +X is left", 0, true);

    // Reorder: two sensors with interleaved batches come out sorted, held back by the window.
    {
        MotionReorderBuffer buffer(50'000'000);
        check(buffer.release().empty(), "empty release", 0, false);
        const auto push = [&](int64_t ms, uint16_t type) {
            MotionSample s;
            s.bootNs = ms * 1'000'000;
            s.type = type;
            buffer.push(s);
        };
        for (int ms = 0; ms <= 60; ms += 5) push(ms, 2);    // gyro batch first
        for (int ms = 0; ms <= 40; ms += 10) push(ms, 3);   // late accelerometer batch
        const auto first = buffer.release();                 // newest 60 ms -> up to 10 ms
        check(!first.empty() && first.back().bootNs <= 10'000'000, "window holds recent samples", 0, false);
        bool sorted = true;
        for (size_t i = 1; i < first.size(); ++i) sorted &= first[i - 1].bootNs <= first[i].bootNs;
        check(sorted, "release is sorted", 0, false);
        check(first.size() == 5 && first[0].type == 2 && first[1].type == 3, "ties keep arrival order", 0, false);
        const auto rest = buffer.releaseAll();
        sorted = !rest.empty() && rest.front().bootNs >= first.back().bootNs;
        for (size_t i = 1; i < rest.size(); ++i) sorted &= rest[i - 1].bootNs <= rest[i].bootNs;
        check(sorted, "releaseAll continues monotonic", 0, false);
        check(first.size() + rest.size() == 13 + 5 && buffer.pending() == 0, "nothing lost", 0, false);
    }

    if (failures == 0) std::printf("motion_axes_test ok\n");
    return failures == 0 ? 0 : 1;
}
