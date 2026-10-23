#include <jni.h>
#include <unistd.h>

#include <cstdint>
#include <exception>
#include <memory>
#include <string>

#include "video/McrawRecorder.h"

namespace {
using rawrcam::video::McrawRecorder;

// The Java handle owns one strong reference to the recorder.
std::shared_ptr<McrawRecorder>* holder(jlong handle) noexcept {
    return reinterpret_cast<std::shared_ptr<McrawRecorder>*>(static_cast<intptr_t>(handle));
}

std::string javaString(JNIEnv* env, jstring value) {
    if (!value) return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string out = chars ? chars : "";
    if (chars) env->ReleaseStringUTFChars(value, chars);
    return out;
}

void throwJava(JNIEnv* env, const char* message) {
    jclass type = env->FindClass("java/lang/IllegalStateException");
    if (type) env->ThrowNew(type, message);
}
}  // namespace

// Takes ownership of fd, also on failure.
extern "C" JNIEXPORT jlong JNICALL Java_com_rawr_camera_video_NativeMcrawEngine_nativeStart(
    JNIEnv* env, jobject, jint fd, jstring deviceMake, jstring deviceModel, jstring software, jint fps, jint width,
    jint height, jint audioChannels, jboolean fullFrame) {
    try {
        McrawRecorder::Settings settings;
        settings.deviceMake = javaString(env, deviceMake);
        settings.deviceModel = javaString(env, deviceModel);
        settings.software = javaString(env, software);
        settings.fps = fps;
        settings.videoWidth = width > 0 ? static_cast<uint32_t>(width) : 0u;
        settings.videoHeight = height > 0 ? static_cast<uint32_t>(height) : 0u;
        settings.audioChannels = audioChannels;
        settings.framing = fullFrame ? rawrcam::video::McrawFraming::FullSensor : rawrcam::video::McrawFraming::Video;
        auto recorder = McrawRecorder::start(fd, settings);  // owns fd from here
        return static_cast<jlong>(reinterpret_cast<intptr_t>(new std::shared_ptr<McrawRecorder>(std::move(recorder))));
    } catch (const std::exception& error) {
        throwJava(env, error.what());
        return 0;
    }
}

extern "C" JNIEXPORT jstring JNICALL Java_com_rawr_camera_video_NativeMcrawEngine_nativeStats(JNIEnv* env, jobject,
                                                                                              jlong handle) {
    auto* recorder = holder(handle);
    return env->NewStringUTF(recorder && *recorder ? (*recorder)->statsJson().c_str() : "{}");
}

// Finishes the file and frees the handle. Returns the final stats JSON.
extern "C" JNIEXPORT jstring JNICALL Java_com_rawr_camera_video_NativeMcrawEngine_nativeStop(JNIEnv* env, jobject,
                                                                                             jlong handle) {
    std::unique_ptr<std::shared_ptr<McrawRecorder>> owned(holder(handle));
    std::string stats = "{}";
    if (owned && *owned) {
        try {
            stats = (*owned)->stop();
        } catch (const std::exception&) {
            stats = std::string("{\"status\":\"failed: stop\",\"complete\":false}");
        }
    }
    return env->NewStringUTF(stats.c_str());
}

// Completes an interrupted .mcraw; closes fd. Returns the frame count.
extern "C" JNIEXPORT jint JNICALL Java_com_rawr_camera_video_NativeMcrawEngine_nativeRecover(JNIEnv* env, jobject,
                                                                                            jint fd) {
    try {
        const size_t frames = McrawRecorder::recover(fd);
        (void)fsync(fd);
        close(fd);
        return static_cast<jint>(frames);
    } catch (const std::exception& error) {
        close(fd);
        throwJava(env, error.what());
        return 0;
    }
}
