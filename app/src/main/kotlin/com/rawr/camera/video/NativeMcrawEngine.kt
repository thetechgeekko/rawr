package com.rawr.camera.video

/**
 * JNI handle for the C++ MCRAW (MotionCam RAW video) writer that runs beside a
 * video recording. Starting it before the HEVC recorder lets that recorder feed
 * it the microphone and IMU; the frame pipeline feeds it every recorded RAW frame.
 */
internal class NativeMcrawEngine {
    companion object {
        init { System.loadLibrary("rawrcam_native") }
    }

    /** Takes ownership of [fd]. Throws when a recording is already active. */
    external fun nativeStart(fd: Int, deviceMake: String, deviceModel: String, software: String, fps: Int,
                             width: Int, height: Int, audioChannels: Int): Long
    external fun nativeStats(handle: Long): String
    /** Finishes the file and frees [handle]; returns the final stats JSON. */
    external fun nativeStop(handle: Long): String
    /** Completes an interrupted .mcraw and closes [fd]; returns its frame count. Throws when nothing is usable. */
    external fun nativeRecover(fd: Int): Int
}
