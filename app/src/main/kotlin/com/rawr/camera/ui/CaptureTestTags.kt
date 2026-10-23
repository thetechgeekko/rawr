package com.rawr.camera.ui

/**
 * Stable automation identifiers for the Capture screen.
 *
 * Human-readable labels rotate with orientation (`PEAK` vs `PEAKING`) and embed live
 * values, so UI tests must match on these tags instead of text or contentDescription.
 * Tags are constant across orientation and state. Adding a tag never changes visuals.
 */
object CaptureTestTags {
    const val VIEWFINDER = "capture_viewfinder"
    const val SHUTTER = "capture_shutter"
    const val VIDEO_RECORD = "capture_video_record"
    const val VIDEO_RECORD_TIMER = "capture_video_record_timer"
    const val VIDEO_AUDIO_METER = "capture_video_audio_meter"
    const val VIDEO_RECORD_DROPS = "capture_video_record_drops"
    const val VIDEO_FPS = "capture_video_fps"
    const val VIDEO_RESOLUTION = "capture_video_resolution"
    const val VIDEO_TIMING_MONITOR = "capture_video_timing_monitor"
    const val THUMBNAIL = "capture_thumbnail"
    const val THUMBNAIL_PROCESSING = "capture_thumbnail_processing"
    const val THUMBNAIL_SUCCESS = "capture_thumbnail_success"
    const val TOPBAR_JPEG = "capture_topbar_jpeg"
    const val TOPBAR_VIDEO_RESOLUTION = "capture_topbar_video_resolution"
    const val TOPBAR_VIDEO_LOG = "topbar_video_log"
    const val TOPBAR_VIDEO_FPS = "capture_topbar_video_fps"
    const val MODE_SELECTOR = "capture_mode_selector"
    const val TOPBAR_WHITE_BALANCE = "capture_topbar_white_balance"
    const val TOPBAR_SELF_TIMER = "capture_topbar_self_timer"
    const val SELF_TIMER_COUNTDOWN = "capture_self_timer_countdown"
    const val HDR_BRACKETED_NOTICE = "hdr_bracketed_notice"
    const val BRACKET_HOLD_NOTICE = "bracket_hold_notice"
    const val TOPBAR_MONITOR = "capture_topbar_monitor"
    const val TOPBAR_SETTINGS = "capture_topbar_settings"

    const val EXPOSURE_SCRUB_SHUTTER = "capture_exposure_scrub_shutter"
    const val EXPOSURE_SCRUB_ISO = "capture_exposure_scrub_iso"
    const val EXPOSURE_SCRUB_EV = "capture_exposure_scrub_ev"
    const val FOCUS_MODE_BAR = "capture_focus_mode_bar"
    const val EXPOSURE_MODE_BAR = "capture_exposure_mode_bar"
    const val FOCUS_SCRUB = "capture_focus_scrub"
    const val AF_TARGET = "capture_af_target"
    const val FACE_BOXES = "capture_face_boxes"
    const val SPOT_AE_TARGET = "capture_spot_ae_target"
    const val WB_PANEL = "capture_wb_panel"
    const val MONITOR_PANEL = "capture_monitor_panel"
    const val EXPOSURE_MONITOR = "capture_exposure_monitor"
    const val COMPACT_PARAM_ROW = "capture_compact_param_row"
    const val COMPACT_WB = "capture_compact_wb"
    const val COMPACT_SS = "capture_compact_ss"
    const val COMPACT_ISO = "capture_compact_iso"
    const val COMPACT_EV = "capture_compact_ev"
    const val COMPACT_FOCUS = "capture_compact_focus"
    const val COMPACT_MULTIFRAME = "capture_compact_multiframe"
    const val COMPACT_FILMSIM = "capture_compact_filmsim"
    const val FILM_STRIP = "capture_film_strip"
    const val FILM_STRIP_PROFILE = "capture_film_strip_profile"
    const val FILM_STRIP_PARAMS = "capture_film_strip_params"
    const val FILM_STRIP_BACK = "capture_film_strip_back"
    const val FILM_STRIP_SECTION_ROW = "capture_film_strip_sections"

    const val RAIL_AE = "capture_rail_ae"
    const val RAIL_OIS = "capture_rail_ois"
    const val RAIL_METERING = "capture_rail_metering"
    const val RAIL_HIGHLIGHT = "capture_rail_highlight"

    const val RAIL_HIGHLIGHT_MODE = "capture_rail_highlight_mode"
    const val RAIL_HIGHLIGHT_THRESHOLD = "capture_rail_highlight_threshold"
    const val RAIL_HIGHLIGHT_COMPRESSION = "capture_rail_highlight_compression"

    const val TONEMAP_STRIP = "capture_tonemap_strip"
    const val TONEMAP_STRIP_PROFILE = "capture_tonemap_strip_profile"
    const val TONEMAP_STRIP_PARAMS = "capture_tonemap_strip_params"
    const val TONEMAP_STRIP_BACK = "capture_tonemap_strip_back"

    fun tonemapParam(key: String): String = "capture_tonemap_param_$key"

    fun filmPreset(id: String): String = "capture_film_preset_$id"

    fun filmParam(key: String): String = "capture_film_param_$key"

    fun filmDiscreteOption(key: String, index: Int): String = "capture_film_option_${key}_$index"

    fun filmSection(key: String): String = "capture_film_section_$key"

    fun filmSubsectionRow(sectionKey: String): String = "capture_film_subsections_$sectionKey"

    fun filmSubsection(sectionKey: String, subKey: String): String = "capture_film_subsection_${sectionKey}_$subKey"

    fun lens(lensId: String): String = "capture_lens_$lensId"

    fun captureMode(mode: String): String = "capture_mode_$mode"

    fun exposureMode(mode: String): String = "capture_exposure_mode_$mode"

    fun focusMode(mode: String): String = "capture_focus_mode_$mode"

    fun faceBox(index: Int): String = "capture_face_box_$index"

    fun whiteBalance(mode: String): String = "capture_wb_$mode"

    fun monitorArm(id: String): String = "capture_monitor_arm_$id"

    fun scope(type: String): String = "capture_scope_$type"

    fun scopeCard(type: String): String = "capture_scope_card_$type"

    fun renderProfile(id: String): String = "capture_render_profile_$id"

    fun toneScrub(label: String): String = "capture_tone_scrub_$label"
}
