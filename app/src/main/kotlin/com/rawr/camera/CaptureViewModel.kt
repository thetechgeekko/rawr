package com.rawr.camera

import android.app.Application
import android.net.Uri
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.rawr.camera.architecture.CaptureQuickEvent
import com.rawr.camera.architecture.CaptureScreenController
import com.rawr.camera.architecture.SyncToneControls
import com.rawr.camera.integration.NativeCaptureScreenController
import com.rawr.camera.integration.RawPreviewCoordinator
import com.rawr.camera.integration.StillCaptureCoordinator
import com.rawr.camera.model.FilmSimQuickState
import com.rawr.camera.model.HighlightQuickMode
import com.rawr.camera.model.RenderProfileQuickState
import com.rawr.camera.model.ToneParameter
import com.rawr.camera.model.ViewfinderQuickState
import com.rawr.camera.model.toFilmSimQuickState
import com.rawr.camera.model.toRenderProfileQuickState
import com.rawr.camera.model.toViewfinderQuickState
import com.rawr.camera.model.withToneParameter
import com.rawr.camera.settings.architecture.SetFilmSimDiscreteValue
import com.rawr.camera.settings.architecture.SetFilmSimFlag
import com.rawr.camera.settings.architecture.SetFilmSimNumericValue
import com.rawr.camera.settings.architecture.capturePreferences
import com.rawr.camera.settings.architecture.editActiveProfileTone
import com.rawr.camera.settings.architecture.withAgxDefault
import com.rawr.camera.settings.architecture.withAgxValue
import com.rawr.camera.settings.architecture.withCapturePreferenceChanges
import com.rawr.camera.settings.architecture.withFilmDiscrete
import com.rawr.camera.settings.architecture.withFilmFlag
import com.rawr.camera.settings.architecture.withFilmNumeric
import com.rawr.camera.settings.architecture.withFilmPreset
import com.rawr.camera.settings.model.FilmSimSpecs
import com.rawr.camera.settings.model.SettingsCatalog
import com.rawr.camera.settings.model.withRenderProfile
import com.rawr.camera.settings.model.SettingsValues
import com.rawr.camera.settings.model.options
import com.rawr.camera.settings.model.persistedId
import com.rawr.camera.settings.model.storedValue
import com.rawr.camera.settings.preferences.SettingsPreferencesStore
import com.rawr.camera.storage.GalleryMediaStore
import com.rawr.camera.video.VideoResolutionMode
import com.rawr.camera.video.toVideoImageSettings
import java.util.concurrent.atomic.AtomicLong
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * Application lifetime owner.
 *
 * Compose state is production-backed for camera/lens/exposure/focus controls.
 * Processing controls not yet integrated remain application/UI concerns.
 */
class CaptureViewModel(application: Application) : AndroidViewModel(application) {
    val previewCoordinator = RawPreviewCoordinator(application)
    private val settingsDefaults = SettingsCatalog.initialState().values
    private val settingsEditor = SettingsPreferencesStore.editor(application, settingsDefaults)

    private val latestSettings: SettingsValues get() = settingsEditor.state.value
    fun videoImageSettings(): com.rawr.camera.video.VideoImageSettings = latestSettings.toVideoImageSettings()
    fun videoEncoderConfig(): com.rawr.camera.settings.model.VideoEncoderConfig = latestSettings.videoEncoder.sanitized()
    /** Microphone setup for the video-mode meter; follows Settings edits. */
    val videoAudioInput: StateFlow<com.rawr.camera.video.VideoAudioInput> = settingsEditor.state
        .map { com.rawr.camera.video.VideoAudioInput.of(it.videoEncoder.sanitized()) }
        .distinctUntilChanged()
        .stateIn(viewModelScope, kotlinx.coroutines.flow.SharingStarted.Eagerly,
            com.rawr.camera.video.VideoAudioInput.of(latestSettings.videoEncoder.sanitized()))
    /** Startup diagnostics use the same optimistic owner, including when preferences are still loading. */
    fun applyStartupVideoOverrides(resolution: VideoResolutionMode?, fps: Int?) {
        if (resolution == null && fps == null) return
        val next = settingsEditor.update { current -> current.copy(
            videoResolutionId = resolution?.persistedId() ?: current.videoResolutionId,
            videoFps = fps?.takeIf { it == 24 || it == 30 } ?: current.videoFps
        ) }
        controller.dispatch(next.capturePreferences())
        settingsSync.updatePreviewCrop(controller.state.value.captureMode, controller.state.value.videoResolution)
    }

    private val mutableRenderProfiles = MutableStateFlow(settingsDefaults.toRenderProfileQuickState())
    val renderProfiles: StateFlow<RenderProfileQuickState> = mutableRenderProfiles.asStateFlow()
    private val mutableFilmSimQuick = MutableStateFlow(settingsDefaults.toFilmSimQuickState())
    val filmSimQuick: StateFlow<FilmSimQuickState> = mutableFilmSimQuick.asStateFlow()
    val viewfinderQuick: StateFlow<ViewfinderQuickState> = settingsEditor.state
        .map { it.toViewfinderQuickState() }
        .distinctUntilChanged()
        .stateIn(viewModelScope, kotlinx.coroutines.flow.SharingStarted.Eagerly,
            latestSettings.toViewfinderQuickState())
    private val galleryMediaStore = GalleryMediaStore(application)
    private val mutableLatestGalleryImage = MutableStateFlow<Uri?>(null)
    val latestGalleryImage: StateFlow<Uri?> = mutableLatestGalleryImage.asStateFlow()
    private val galleryRefreshGeneration = AtomicLong(0L)
    private val stillCaptureCoordinator =
        StillCaptureCoordinator(
            application = application,
            preview = previewCoordinator,
            settings = { latestSettings },
            onJpegPublished = ::onJpegPublished,
            onDngPublished = ::refreshLatestGalleryImage
        )
    val controller: CaptureScreenController =
        NativeCaptureScreenController(
            previewCoordinator,
            onCaptureQueued = stillCaptureCoordinator::enqueueCapture,
            requestStillCapture = stillCaptureCoordinator::capture,
            pollStillCompletion = stillCaptureCoordinator::pollCompletion,
            onToneScrubbed = { parameter, value -> persistQuickTone(parameter, value) },
            onRenderExposureScrubbed = ::persistRenderExposure,
            onAgxControl = ::persistAgxControl,
            videoLocked = { videoSelectionLocked() },
            onRawCpuIngress = {
                android.os.Handler(android.os.Looper.getMainLooper()).post {
                    android.widget.Toast.makeText(
                        application,
                        "Camera frames use the CPU copy path on this device (slower, more battery)",
                        android.widget.Toast.LENGTH_LONG
                    ).show()
                }
            },
            onLensSelected = { lensId -> settingsEditor.update { it.copy(lastLensId = lensId) } },
            onCapturePreferencesChanged = { before, after ->
                settingsEditor.update { it.withCapturePreferenceChanges(before, after) }
                settingsSync.updatePreviewCrop(after.captureMode, after.videoResolution)
            }
        )

    private val settingsSync by lazy {
        com.rawr.camera.integration.CaptureSettingsSync(
            viewModelScope, settingsEditor.values, { latestSettings }, previewCoordinator, controller
        )
    }

    var recordingOptions = com.rawr.camera.video.RecordingOptions()
    private val recordingBackend = com.rawr.camera.video.CaptureRecordingBackend(
        application, controller, previewCoordinator, { latestSettings }
    )
    val recording = com.rawr.camera.video.RecordingCoordinator(
        controller = controller,
        startRecorder = { recordingBackend.start(recordingOptions) },
        clockNs = android.os.SystemClock::elapsedRealtimeNanos,
        onSaved = { recorder ->
            if (recordingOptions.inspectOutput) {
                recordingOptions = recordingOptions.copy(inspectOutput = false)
                recordingBackend.inspectSaved(recorder)
            }
        }
    )
    private var captureVisible = false
    private var previewStopJob: kotlinx.coroutines.Job? = null

    fun startPreview() {
        captureVisible = true
        viewModelScope.launch {
            previewStopJob?.join()
            if (captureVisible) previewCoordinator.start()
        }
    }

    fun stopPreviewAfterRecording() {
        captureVisible = false
        val stopping = recording.stop()
        previewStopJob = viewModelScope.launch {
            stopping.join()
            if (!captureVisible) previewCoordinator.stop()
        }
    }

    init {
        refreshLatestGalleryImage()
        viewModelScope.launch { recordingBackend.recoverInterrupted() }
        viewModelScope.launch {
            settingsEditor.values.map { it.capturePreferences() }.distinctUntilChanged().collect { preferences ->
                controller.dispatch(preferences)
                settingsSync.updatePreviewCrop(controller.state.value.captureMode, controller.state.value.videoResolution)
            }
        }
        viewModelScope.launch {
            settingsEditor.values.map { it.toRenderProfileQuickState() }.distinctUntilChanged().collect {
                mutableRenderProfiles.value = it
            }
        }
        viewModelScope.launch {
            settingsEditor.values.distinctUntilChanged { before, after ->
                before.selectedFilmPresetId == after.selectedFilmPresetId &&
                    before.filmSimLook == after.filmSimLook && before.filmPresets == after.filmPresets
            }.collect { mutableFilmSimQuick.value = it.toFilmSimQuickState() }
        }
        settingsSync.start()
    }

    fun refreshLatestGalleryImage() {
        val generation = galleryRefreshGeneration.incrementAndGet()
        viewModelScope.launch {
            val latest = withContext(Dispatchers.IO) { galleryMediaStore.findLatestRawrImage() }
            if (galleryRefreshGeneration.get() == generation) mutableLatestGalleryImage.value = latest
        }
    }

    private fun onJpegPublished(uri: Uri) {
        galleryMediaStore.rememberPublishedJpeg(uri)
        galleryRefreshGeneration.incrementAndGet()
        mutableLatestGalleryImage.value = uri
    }

    private fun videoSelectionLocked(): Boolean = recording.state.value.let { it.busy || it.recording }

    fun selectRenderProfileFromCapture(selection: com.rawr.camera.model.RenderProfileSelection) {
        if (latestSettings.isVideo && videoSelectionLocked()) return
        val next = settingsEditor.update { it.withRenderProfile(selection) }
        mutableRenderProfiles.value = next.toRenderProfileQuickState()
        previewCoordinator.setColorRenderProfile(next.effectiveRenderProfileId(), next.regularLutProfileId())
        controller.dispatch(SyncToneControls.fromImageTone(next.activeImageTone()))
    }

    fun onFilmEvent(event: com.rawr.camera.architecture.CaptureFilmEvent) {
        val next = settingsEditor.update { current ->
            when (event) {
                is com.rawr.camera.architecture.CaptureFilmEvent.SelectPreset -> current.withFilmPreset(event.id)
                is com.rawr.camera.architecture.CaptureFilmEvent.ScrubNumeric -> current.withFilmNumeric(
                    SetFilmSimNumericValue(event.parameter, event.value), quantize = true, enableGrain = true
                )
                is com.rawr.camera.architecture.CaptureFilmEvent.ResetNumeric -> current.withFilmNumeric(
                    SetFilmSimNumericValue(event.parameter, FilmSimSpecs.forParameter(event.parameter).defaultValue)
                )
                is com.rawr.camera.architecture.CaptureFilmEvent.ScrubDiscrete -> {
                    if (event.index !in event.field.options().indices) current else current.withFilmDiscrete(
                        SetFilmSimDiscreteValue(event.field, event.field.storedValue(event.index))
                    )
                }
                is com.rawr.camera.architecture.CaptureFilmEvent.SetFlag -> current.withFilmFlag(
                    SetFilmSimFlag(event.flag, event.enabled)
                )
            }
        }
        mutableFilmSimQuick.value = next.toFilmSimQuickState()
        previewCoordinator.setFilmSimLook(next.filmSimLook.toFloatArray(), next.filmSimLook.toIntArray())
    }

    /** Rail edits go through settings; CaptureSettingsSync projects them onto the preview and stills. */
    fun onQuickEvent(event: CaptureQuickEvent) {
        if (latestSettings.isVideo && videoSelectionLocked()) return
        settingsEditor.update { c ->
            val video = c.isVideo
            when (event) {
                CaptureQuickEvent.ToggleRawrAe -> c.copy(rawrAe = c.rawrAe.copy(enabled = !c.rawrAe.enabled))
                CaptureQuickEvent.ToggleOis -> c.copy(oisEnabledPreference = !c.oisEnabledPreference)
                is CaptureQuickEvent.SetMetering -> c.copy(rawrAe = c.rawrAe.copy(metering = event.metering))
                is CaptureQuickEvent.SetHighlightMode -> {
                    val enabled = event.mode != HighlightQuickMode.Off
                    val method = when (event.mode) {
                        HighlightQuickMode.Off -> if (video) c.videoHighlightMethod else c.photoHighlightMethod
                        HighlightQuickMode.Propagation -> 0
                        HighlightQuickMode.Opposed -> 1
                    }
                    if (video) c.copy(videoHighlightEnabled = enabled, videoHighlightMethod = method)
                    else c.copy(photoHighlightEnabled = enabled, photoHighlightMethod = method)
                }
                is CaptureQuickEvent.SetHighlightThreshold -> event.value.coerceIn(0.5f, 2f).let {
                    if (video) c.copy(videoHighlightThreshold = it) else c.copy(photoHighlightThreshold = it)
                }
                is CaptureQuickEvent.SetHighlightCompression -> event.value.coerceIn(0f, 300f).let {
                    if (video) c.copy(videoHighlightCompression = it) else c.copy(photoHighlightCompression = it)
                }
            }
        }
    }

    private fun persistQuickTone(parameter: ToneParameter, value: Int) {
        settingsEditor.update { current ->
            current.editActiveProfileTone { it.withToneParameter(parameter, value.coerceIn(-100, 100).toFloat()) }
        }
    }

    private fun persistAgxControl(parameter: com.rawr.camera.settings.model.AgxParameter, value: Float?) {
        settingsEditor.update { current ->
            if (value == null) current.withAgxDefault(parameter) else current.withAgxValue(parameter, value)
        }
    }

    private fun persistRenderExposure(tenths: Int) {
        val ev = com.rawr.camera.model.TonemapControlContract.exposureEvFromTenths(tenths)
        settingsEditor.update { current -> current.editActiveProfileTone { it.copy(renderExposure = ev) } }
    }

    /** Diagnostic camera-ID override; native routing remains authoritative. */

    fun selectCameraIdFromIntent(cameraId: String) {
        previewCoordinator.setPreferredCameraId(cameraId)
    }

    override fun onCleared() {
        recording.closeWhenIdle {
            stillCaptureCoordinator.closeWhenIdle {
                controller.close()
                previewCoordinator.close()
            }
        }
    }
}
