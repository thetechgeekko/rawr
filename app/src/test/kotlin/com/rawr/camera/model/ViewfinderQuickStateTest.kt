package com.rawr.camera.model

import com.rawr.camera.settings.model.SettingsCatalog
import com.rawr.camera.settings.model.VideoEncoderConfig
import kotlin.test.*

class ViewfinderQuickStateTest {
    private val defaults = SettingsCatalog.initialState().values

    @Test fun railShowsOisOnlyInVideo() {
        assertFalse(defaults.copy(captureModeId = "photo").toViewfinderQuickState().video)
        assertTrue(defaults.copy(captureModeId = "video").toViewfinderQuickState().video)
    }

    @Test fun railFollowsTheOisPreference() {
        val video = defaults.copy(captureModeId = "video")
        assertTrue(video.copy(oisEnabledPreference = true).toViewfinderQuickState().oisEnabled)
        assertFalse(video.copy(oisEnabledPreference = false).toViewfinderQuickState().oisEnabled)
    }

    @Test fun gyroEmbeddingDefaultsOnAndSurvivesSanitizing() {
        assertTrue(VideoEncoderConfig().embedGyro)
        assertFalse(VideoEncoderConfig(embedGyro = false).sanitized().embedGyro)
    }

    @Test fun mcrawRecordingIsOptInAndSurvivesSanitizing() {
        assertFalse(VideoEncoderConfig().recordMcraw)
        assertTrue(VideoEncoderConfig(recordMcraw = true).sanitized().recordMcraw)
    }

    @Test fun mcrawStoresTheFullRawFrameByDefault() {
        assertTrue(VideoEncoderConfig().mcrawFullFrame)
        assertFalse(VideoEncoderConfig(mcrawFullFrame = false).sanitized().mcrawFullFrame)
    }
}
