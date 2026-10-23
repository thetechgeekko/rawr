package com.rawr.camera.video

import android.app.Application
import com.rawr.camera.architecture.CaptureScreenController
import com.rawr.camera.integration.RawPreviewCoordinator
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject

/** Values translated from diagnostic intent extras by the platform host. */
data class RecordingOptions(
    val shutterNs: Long? = null,
    val iso: Int? = null,
    val encoderOverrides: Map<String, Int> = emptyMap(),
    val inspectOutput: Boolean = false
)

internal class CaptureRecordingBackend(
    private val application: Application,
    private val controller: CaptureScreenController,
    private val preview: RawPreviewCoordinator,
    private val settingsSnapshot: () -> com.rawr.camera.settings.model.SettingsValues
) {
    suspend fun start(options: RecordingOptions): VideoRecording {
        val captureSnapshot = controller.state.value
        val selectedMode = captureSnapshot.videoResolution
        val selectedFps = captureSnapshot.videoFps
        val imageSettings = settingsSnapshot()
        val encoder = imageSettings.videoEncoder.sanitized()
        val log = imageSettings.videoLogEnabled
        // Queue the exact selection before the synchronous recording-start barrier.
        preview.setColorRenderProfile(imageSettings.effectiveRenderProfileId(), imageSettings.regularLutProfileId())
        preview.setTonemapParameters(imageSettings.activeImageTone())
        preview.setAgxParameters(imageSettings.activeAgxValues())
        // The Activity stays portrait-locked; freeze the physical posture at shutter press.
        val deviceRotationDegrees = preview.captureDeviceRotationDegrees
        val recorder = NativeVideoRecorder(application, preview)
        return withContext(Dispatchers.IO) {
            try {
                if (options.shutterNs != null) {
                    preview.setExposureMode(1)
                    preview.setManualExposureTimeNs(
                        requireNotNull(options.shutterNs))
                    if (options.iso != null)
                        preview.setManualSensitivity(
                            requireNotNull(options.iso))
                }
                val frameSize = selectedMode.resolve(
                    JSONObject(preview.cameraControlSnapshot()))
                // Settings → Video Encoder; debug builds let scripted runs override via extras.
                fun extra(name: String, value: Int) =
                    options.encoderOverrides[name] ?: value
                // Never ask for more than the encoder advertises.
                val bitrate = extra("raw_video_bitrate", minOf(encoder.bitrateMbps(selectedMode),
                    com.rawr.camera.video.VideoEncoderCapabilities.maxBitrateMbps) * 1_000_000)
                val intraFrames = extra("raw_video_intra_frames", encoder.keyframeInterval.frames(selectedFps))
                val bitrateMode = extra("raw_video_bitrate_mode", encoder.bitrateMode.wireValue)
                val maxBFrames = extra("raw_video_max_b_frames", encoder.maxBFrames)
                val audioChannels = extra("raw_video_audio_channels", encoder.audioChannels)
                val audioBitrate = extra("raw_video_audio_bitrate", encoder.audioBitrateKbps * 1_000)
                val bitDepth = if (log) 10 else extra("raw_video_bit_depth", encoder.bitDepth)
                if (log) check(VideoEncoderCapabilities.supportsMain10(frameSize.width, frameSize.height, selectedFps)) {
                    "LOG requires hardware HEVC 10-bit at ${frameSize.width}×${frameSize.height} / $selectedFps fps"
                }
                recorder.start(NativeVideoRecorder.Settings(
                    saveLocationId = imageSettings.saveLocationId,
                    fps = selectedFps, bitrate = bitrate, intraFrames = intraFrames,
                    width = frameSize.width, height = frameSize.height, mode = selectedMode.label,
                    deviceRotationDegrees = deviceRotationDegrees, bitrateMode = bitrateMode,
                    maxBFrames = maxBFrames, audioChannels = audioChannels, audioBitrate = audioBitrate,
                    audio = VideoAudioInput.of(encoder),
                    audioPcmBits = extra("raw_video_audio_pcm_bits", encoder.audioCodec.pcmBits),
                    bitDepth = bitDepth,
                    renderProfile = imageSettings.activeProfileLabel(),
                    logProfile = imageSettings.videoLogProfile.takeIf { log },
                    renderProfileId = imageSettings.effectiveRenderProfileId(),
                    embedGyro = encoder.embedGyro,
                    recordMcraw = encoder.recordMcraw,
                    mcrawFullFrame = encoder.mcrawFullFrame))
                recorder
            } catch (e: Exception) {
                runCatching { recorder.close() }
                throw e
            }
        }
    }

    suspend fun recoverInterrupted() = withContext(Dispatchers.IO) {
        NativeVideoRecorder.recoverInterrupted(application)
    }

    suspend fun inspectSaved(recorder: VideoRecording) = withContext(Dispatchers.IO) {
        // Give the media provider time to expose the completed output to diagnostic inspection.
        kotlinx.coroutines.delay(500)
        runCatching {
            val report = VideoFileInspector.inspect(application, requireNotNull(recorder.outputUri))
            report.put("postStopCamera", JSONObject(preview.cameraControlSnapshot()))
            java.io.File(application.filesDir, "video_live_inspect.json").writeText(report.toString(2))
        }.onFailure { android.util.Log.e("RawrVideo", "On-device inspection failed", it) }
    }
}
