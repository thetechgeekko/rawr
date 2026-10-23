package com.rawr.camera.model

import com.rawr.camera.settings.model.RawrMetering
import com.rawr.camera.settings.model.SettingsValues

/** Highlight reconstruction as the viewfinder rail presents it: off, or one of the two methods. */
enum class HighlightQuickMode { Off, Propagation, Opposed }

/** Photo or video highlight reconstruction, whichever the current capture mode renders with. */
data class HighlightQuickState(
    val mode: HighlightQuickMode = HighlightQuickMode.Off,
    val threshold: Float = HIGHLIGHT_THRESHOLD_DEFAULT,
    val compression: Float = PHOTO_HIGHLIGHT_COMPRESSION_DEFAULT,
    val compressionDefault: Float = PHOTO_HIGHLIGHT_COMPRESSION_DEFAULT
)

/** Capture-screen projection of the settings the compact viewfinder rail edits. */
data class ViewfinderQuickState(
    val rawrAeEnabled: Boolean = false,
    val metering: RawrMetering = RawrMetering.Matrix,
    val highlight: HighlightQuickState = HighlightQuickState(),
    /** Video mode shows OIS on the rail: gyro stabilization in post wants it off. */
    val video: Boolean = false,
    val oisEnabled: Boolean = true,
    /** Recording locks the rail like it locks the render profile. */
    val locked: Boolean = false
)

internal const val HIGHLIGHT_THRESHOLD_DEFAULT = 1f
internal const val PHOTO_HIGHLIGHT_COMPRESSION_DEFAULT = 45f
internal const val VIDEO_HIGHLIGHT_COMPRESSION_DEFAULT = 100f

internal fun SettingsValues.toViewfinderQuickState(): ViewfinderQuickState {
    val enabled = if (isVideo) videoHighlightEnabled else photoHighlightEnabled
    val method = if (isVideo) videoHighlightMethod else photoHighlightMethod
    val compressionDefault =
        if (isVideo) VIDEO_HIGHLIGHT_COMPRESSION_DEFAULT else PHOTO_HIGHLIGHT_COMPRESSION_DEFAULT
    return ViewfinderQuickState(
        rawrAeEnabled = rawrAe.enabled,
        metering = rawrAe.metering,
        highlight = HighlightQuickState(
            mode = when {
                !enabled -> HighlightQuickMode.Off
                method == 1 -> HighlightQuickMode.Opposed
                else -> HighlightQuickMode.Propagation
            },
            threshold = if (isVideo) videoHighlightThreshold else photoHighlightThreshold,
            compression = if (isVideo) videoHighlightCompression else photoHighlightCompression,
            compressionDefault = compressionDefault
        ),
        video = isVideo,
        oisEnabled = oisEnabledPreference
    )
}
