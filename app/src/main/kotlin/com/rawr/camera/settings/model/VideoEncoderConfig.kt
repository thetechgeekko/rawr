package com.rawr.camera.settings.model

import com.rawr.camera.video.VideoResolutionMode

/** MediaCodec bitrate modes, with their KEY_BITRATE_MODE wire values. */
enum class VideoBitrateMode(val wireValue: Int, val label: String) {
    Cbr(2, "CBR"),
    Vbr(1, "VBR"),
    Cq(0, "CQ");

    companion object {
        fun of(wireValue: Int): VideoBitrateMode = entries.firstOrNull { it.wireValue == wireValue } ?: Cbr
    }
}

/** Microphone processing preset, with its AAudio input-preset wire value. */
enum class VideoAudioSource(val wireValue: Int, val label: String) {
    /** Device-tuned for video: usually stereo, with AGC and noise suppression. */
    Camcorder(5, "Camcorder"),
    /** No AGC, noise suppression or EQ; only on devices that advertise it. */
    Unprocessed(9, "Raw"),
    /** Tuned for speech; often mono, light processing. */
    Voice(6, "Voice"),
    /** The device default microphone path. */
    Mic(1, "Mic");

    companion object {
        fun of(wireValue: Int): VideoAudioSource = entries.firstOrNull { it.wireValue == wireValue } ?: Camcorder
    }
}

/** Audio track format. PCM records a QuickTime .mov; AAC an .mp4. [pcmBits] 0 = AAC. */
enum class VideoAudioCodec(val pcmBits: Int, val label: String) {
    Aac(0, "AAC"),
    Pcm16(16, "PCM 16"),
    Pcm24(24, "PCM 24");

    val isPcm: Boolean get() = pcmBits > 0

    companion object {
        fun of(pcmBits: Int): VideoAudioCodec = entries.firstOrNull { it.pcmBits == pcmBits } ?: Aac
    }
}

/** Which microphone to record from. Auto follows Android routing, which picks an attached USB or wired mic. */
enum class VideoAudioRoute(val wireValue: Int, val label: String) {
    Auto(0, "Auto"),
    BuiltIn(1, "Built-in"),
    External(2, "External");

    companion object {
        fun of(wireValue: Int): VideoAudioRoute = entries.firstOrNull { it.wireValue == wireValue } ?: Auto
    }
}

/**
 * I-frame interval as a multiple of the recording frame rate, so it means the same
 * at 24 and 30 fps. [wireValue] is the interval in half-seconds; 0 = all-intra.
 */
enum class KeyframeInterval(val wireValue: Int, val label: String) {
    AllIntra(0, "All-I"),
    Half(1, "½ × FPS"),
    One(2, "1 × FPS"),
    Two(4, "2 × FPS");

    /** Frames between I-frames at [fps]; 1 means every frame is an I-frame. */
    fun frames(fps: Int): Int = if (this == AllIntra) 1 else maxOf(1, fps * wireValue / 2)

    companion object {
        fun of(wireValue: Int): KeyframeInterval =
            entries.minBy { kotlin.math.abs(it.wireValue - wireValue) }
    }
}

/**
 * Video encoder settings, applied when a recording starts. The factory
 * clamps every field to what NativeAvRecorder accepts, so a stale or
 * hand-edited preference can never fail recording start.
 */
data class VideoEncoderConfig(
    /** 8 = HEVC Main, 10 = HEVC Main10. */
    val bitDepth: Int = 8,
    val bitrate1080pMbps: Int = 12,
    val bitrate4kMbps: Int = 40,
    val bitrateOpenGateMbps: Int = 60,
    val bitrateMode: VideoBitrateMode = VideoBitrateMode.Cbr,
    val keyframeInterval: KeyframeInterval = KeyframeInterval.One,
    /** -1 lets the codec choose; 0 disables B-frames. */
    val maxBFrames: Int = -1,
    /** 1 = mono, 2 = stereo. */
    val audioChannels: Int = 2,
    val audioBitrateKbps: Int = 192,
    val audioCodec: VideoAudioCodec = VideoAudioCodec.Aac,
    val audioSource: VideoAudioSource = VideoAudioSource.Camcorder,
    val audioRoute: VideoAudioRoute = VideoAudioRoute.Auto,
    /** Digital gain after capture; Android exposes no analog mic gain. */
    val audioGainDb: Int = 0,
    /** High-pass corner in Hz; 0 = off. */
    val audioLowCutHz: Int = 0,
    /** Gyroscope + accelerometer as a CAMM track, for gyro stabilization (e.g. Gyroflow). */
    val embedGyro: Boolean = true,
    /** A MotionCam .mcraw RAW clip (frames, audio, IMU) beside the HEVC video. */
    val recordMcraw: Boolean = false,
    /** MCRAW stores the whole RAW frame; false stores only the video's framing (still unprocessed). */
    val mcrawFullFrame: Boolean = true
) {
    fun sanitized(): VideoEncoderConfig = copy(
        bitDepth = if (bitDepth == 8) 8 else 10,
        bitrate1080pMbps = bitrate1080pMbps.coerceIn(MIN_BITRATE_MBPS, MAX_BITRATE_MBPS),
        bitrate4kMbps = bitrate4kMbps.coerceIn(MIN_BITRATE_MBPS, MAX_BITRATE_MBPS),
        bitrateOpenGateMbps = bitrateOpenGateMbps.coerceIn(MIN_BITRATE_MBPS, MAX_BITRATE_MBPS),
        maxBFrames = maxBFrames.coerceIn(-1, 2),
        audioChannels = if (audioChannels == 1) 1 else 2,
        audioBitrateKbps = AUDIO_BITRATES_KBPS.minBy { kotlin.math.abs(it - audioBitrateKbps) },
        audioGainDb = audioGainDb.coerceIn(MIN_AUDIO_GAIN_DB, MAX_AUDIO_GAIN_DB),
        audioLowCutHz = AUDIO_LOW_CUT_HZ.minBy { kotlin.math.abs(it - audioLowCutHz) }
    )

    fun bitrateMbps(mode: VideoResolutionMode): Int = when (mode) {
        VideoResolutionMode.HD1080 -> bitrate1080pMbps
        VideoResolutionMode.UHD4K -> bitrate4kMbps
        VideoResolutionMode.OPEN_GATE -> bitrateOpenGateMbps
    }

    fun withBitrateMbps(mode: VideoResolutionMode, mbps: Int): VideoEncoderConfig = when (mode) {
        VideoResolutionMode.HD1080 -> copy(bitrate1080pMbps = mbps)
        VideoResolutionMode.UHD4K -> copy(bitrate4kMbps = mbps)
        VideoResolutionMode.OPEN_GATE -> copy(bitrateOpenGateMbps = mbps)
    }.sanitized()

    companion object {
        const val MIN_BITRATE_MBPS = com.rawr.camera.video.VideoEncoderLimits.MIN_BITRATE_MBPS
        const val MAX_BITRATE_MBPS = com.rawr.camera.video.VideoEncoderLimits.MAX_BITRATE_MBPS
        val AUDIO_BITRATES_KBPS = listOf(128, 192, 256, 320)
        val MAX_B_FRAME_CHOICES = listOf(-1, 0, 1, 2)
        const val MIN_AUDIO_GAIN_DB = -12
        const val MAX_AUDIO_GAIN_DB = 24
        val AUDIO_LOW_CUT_HZ = listOf(0, 80, 120)

        /** Requested GOP in display order, e.g. "I B B P B B P … (30)"; all-intra is "I I I …". */
        fun gopPattern(frames: Int, maxBFrames: Int): String {
            if (frames <= 1) return "I I I … (all-intra)"
            val shown = minOf(frames - 1, 6)
            val run = List(maxBFrames.coerceAtLeast(0)) { "B" } + "P"
            val tail = generateSequence { run }.flatten().take(shown).toList()
            val more = if (frames - 1 > shown) " …" else ""
            val auto = if (maxBFrames < 0) " · B-frames auto" else ""
            return (listOf("I") + tail).joinToString(" ") + "$more ($frames)$auto"
        }
    }
}
