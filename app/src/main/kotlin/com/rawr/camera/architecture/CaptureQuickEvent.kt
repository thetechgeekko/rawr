package com.rawr.camera.architecture

import com.rawr.camera.model.HighlightQuickMode
import com.rawr.camera.settings.model.RawrMetering

/**
 * Typed commands for the compact viewfinder rail. Highlight events edit the
 * photo or video set, whichever the current capture mode renders with.
 */
sealed interface CaptureQuickEvent {
    data object ToggleRawrAe : CaptureQuickEvent
    data object ToggleOis : CaptureQuickEvent
    data class SetMetering(val metering: RawrMetering) : CaptureQuickEvent
    data class SetHighlightMode(val mode: HighlightQuickMode) : CaptureQuickEvent
    data class SetHighlightThreshold(val value: Float) : CaptureQuickEvent
    data class SetHighlightCompression(val value: Float) : CaptureQuickEvent
}
