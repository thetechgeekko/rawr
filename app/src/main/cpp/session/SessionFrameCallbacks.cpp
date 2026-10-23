// Adapts narrow frame ports to session-owned domain collaborators.

#include <sys/system_properties.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "capture/multiframe/BracketExposure.h"
#include "color/AePostGainCurve.h"
#include "camera/NativeCameraController.h"
#include "color/AutoKeyExposure.h"
#include "image_scopes/types.h"
#include "session/SessionEngine.h"
#include "tonemap/TonemapMath.h"
#include "video/McrawRecorder.h"

namespace rawrcam::session {

SessionFrameCallbacks::SessionFrameCallbacks(SessionEngine* engine) : engine_(engine) {}

void SessionFrameCallbacks::postDiagnostic(const std::string& line) { engine_->appendDiagnostic(line); }
void SessionFrameCallbacks::postAudit(const std::string& line) { engine_->recordPipelineAuditLine(line); }
void SessionFrameCallbacks::notifySwapchainOutOfDate() {
    {
        engine_->recreateSwapchainAfterOutOfDate();
    }
}
void SessionFrameCallbacks::finalizeDetachedStill() {
    {
        engine_->finalizeDeferredSurfaceDetach();
    }
}

namespace {
// Realized exposure (shutter ns x request-coordinate ISO) of a delivered frame.
// AE OFF frames carry the exact request; Camera2 AE frames convert the
// reported ISO through the learned coordinate ratio.
struct RawrFrameExposure {
    double shutterNs = 0.0, iso = 0.0;
    [[nodiscard]] double logExposure() const { return std::log2(shutterNs * iso); }
};
std::optional<RawrFrameExposure> rawrFrameExposure(const rawrcam::metadata::FrameMetadataSnapshot& m,
                                                   double reportedPerRequest) {
    constexpr int32_t kAeModeOff = 0;
    RawrFrameExposure e;
    e.shutterNs = double(m.exposureTimeNs > 0 ? m.exposureTimeNs : m.requestedExposureTimeNs.value_or(0));
    if (m.aeMode == kAeModeOff && m.requestedSensitivity && *m.requestedSensitivity > 0) {
        e.iso = *m.requestedSensitivity;
        // The HAL may deliver less than asked (past the sensor mode's real
        // range); its report, in the request coordinate, says so.
        if (m.sensitivity > 0 && reportedPerRequest > 0.0)
            e.iso = std::min(e.iso, double(m.sensitivity) / reportedPerRequest * 1.02);
    } else if (m.sensitivity > 0)
        e.iso = double(m.sensitivity) / (reportedPerRequest > 0.0 ? reportedPerRequest : 1.0);
    if (!(e.shutterNs > 0.0) || !(e.iso > 0.0)) return std::nullopt;
    return e;
}
}  // namespace

// Share of the Matrix key the focus area takes, in EV. Tuning:
// `adb shell setprop debug.rawr.ae_focus_share 0..100` (percent; read once
// per process).
static float rawrAeFocusShare() {
    static const float share = [] {
        char value[PROP_VALUE_MAX]{};
        if (__system_property_get("debug.rawr.ae_focus_share", value) > 0)
            return std::clamp(float(std::atoi(value)), 0.0f, 100.0f) / 100.0f;
        return 0.4f;
    }();
    return share;
}

bool SessionFrameCallbacks::rawrAeEngaged() const {
    const auto* controller = engine_->cameraControls_.controller();
    return controller && controller->rawrAeContext().engaged;
}

void SessionFrameCallbacks::updateRawrAe(const image_scopes::SceneExposureStatsData* stats, float clipLuma,
                                         const rawrcam::metadata::FrameMetadataSnapshot& metadata,
                                         std::uint64_t timestampNs) {
    namespace ae = rawrcam::color::rawr_ae;
    auto* controller = engine_->cameraControls_.controller();
    if (!controller) return;
    const auto context = controller->rawrAeContext();
    std::lock_guard<std::mutex> lock(rawrAeMutex_);
    // A different camera (lens switch, session rebuild) starts over from its first frame.
    const std::uint64_t generation = metadata.cameraContext ? metadata.cameraContext->cameraContextGeneration : 0;
    if (!context.engaged || generation != rawrAeCameraGeneration_) {
        if (rawrAe_.primed()) rawrAe_.reset();
        rawrAeCameraGeneration_ = generation;
        if (!context.engaged) return;
    }
    // One-shot bracket/probe exposures say nothing about the repeating scene.
    if (!stats || !(clipLuma > 0.0f) || (metadata.optimizedStillRequestId && *metadata.optimizedStillRequestId != 0))
        return;
    const auto exposure = rawrFrameExposure(metadata, context.reportedPerRequest);
    if (!exposure) return;
    ae::Settings settings;
    settings.enabled = true;
    settings.mode = ae::Mode(std::clamp(context.mode, 0, 2));
    settings.night = ae::nightLookFromId(context.config.nightLook);
    settings.balanceEv = context.config.balanceEv;
    settings.lockedShutterNs = context.lockedShutterNs;
    settings.lockedIso = context.lockedIso;
    settings.flickerPeriodNs = context.flickerPeriodNs;
    // S: the user's shutter, up to what the sensor can do (not the handheld cap).
    const int64_t shutterMax =
        settings.mode == ae::Mode::ShutterPriority ? std::max(context.shutterMaxNs, context.sensorShutterMaxNs)
                                                   : context.shutterMaxNs;
    settings.limits = ae::Limits{context.isoMin,       context.isoMax, context.isoAnalogMax,
                                 context.shutterMinNs, shutterMax,     std::min(context.shutterStartNs, shutterMax),
                                 context.config.maxDigitalGainEv};
    ae::FrameObservation f{stats->weightedBins, std::log2(clipLuma), exposure->logExposure(), exposure->shutterNs,
                           exposure->iso, metadata.aperture.value_or(0.0f), timestampNs};
    static_assert(image_scopes::kSceneChannelClip == ae::kChannelClip);
    f.channelBins = stats->channelBins;
    // A spot's bins cover the tapped area only; the night look judges the frame.
    if (context.spotActive) f.frameLogMeanEv = image_scopes::sceneFrameLogMeanEv(*stats);
    // Matrix leans its key toward the focus area (empty bins when there is none).
    if (!context.spotActive && context.config.metering == 0) {
        f.focusBins = stats->focusBins;
        f.focusShare = rawrAeFocusShare();
    }
    // A tapped spot meters with the default highlight handling.
    const auto meterPolicy = context.spotActive ? ae::MeterPolicy{} : ae::meterPolicyFor(context.config.metering);
    if (const auto step = rawrAe_.update(f, settings, context.userEv, meterPolicy, ae::filterPolicyForSpeed(context.video ? 0 : context.config.speed)))
        controller->setRawrAeRequest(step->request.shutterNs, step->request.iso);
}

float SessionFrameCallbacks::postGainFor(const rawrcam::metadata::FrameMetadataSnapshot& metadata) {
    // rawr AE: the frame is shown at the smoothed target, whatever exposure
    // the sensor actually delivered (latency, integer ISO, flicker steps,
    // digital gain past the analog maximum). Camera2's post-RAW boost is not
    // used: AE OFF frames carry none, and the target already includes it.
    // A tagged one-shot still at the repeating exposure gets the same gain
    // (photo = viewfinder, digital gain included); bracket frames keep their
    // own exposure for the merge.
    if (rawrAeEngaged()) {
        const auto* controller = engine_->cameraControls_.controller();
        const auto exposure = rawrFrameExposure(metadata, controller->rawrAeContext().reportedPerRequest);
        const bool tagged = metadata.optimizedStillRequestId && *metadata.optimizedStillRequestId != 0;
        std::lock_guard<std::mutex> lock(rawrAeMutex_);
        const auto& last = rawrAe_.last();
        const bool atRepeating = !tagged || (exposure && last &&
                                             std::abs(exposure->logExposure() - std::log2(double(last->request.shutterNs) *
                                                                                         double(last->request.iso))) < 0.15);
        if (exposure && atRepeating) {
            if (const auto residual = rawrAe_.residualEvFor(metadata.timestampNs, exposure->logExposure()))
                return std::clamp(std::exp2(*residual), 1.0f / 16.0f, tonemap::kMaxAePostGain);
        }
    }
    // Camera2 boost (100 == 1x) shaped by the user cap + HighlightProtection
    // knee width. Both live on SessionEngine so preview/stills share one
    // choke point.
    // HDR+ Bracketed headroom is lifted back here as a linear gain, so the
    // viewfinder, AutoKey and frozen still gains all see the scene at Camera2
    // AE brightness. Highlights above display white are the render's: AgX,
    // NTRL (DaVinci Intermediate), film and log LUT inputs roll them off;
    // only direct sRGB/Rec.709 clip, as for any capture.
    const float gain = rawrcam::color::effectiveAePostGain(metadata.postRawSensitivityBoost, engine_->maxAePostGain(),
                                                           engine_->postGainKneeWidthEv());
    // M with a shutter past the viewfinder cap: the viewfinder runs the cap at
    // raised ISO; where ISO ran out, show the rest as gain (capture = preview).
    if (const auto* controller = engine_->cameraControls_.controller();
        controller && metadata.aeMode == 0 && !(metadata.optimizedStillRequestId && *metadata.optimizedStillRequestId != 0)) {
        const auto context = controller->rawrAeContext();
        const auto exposure = rawrFrameExposure(metadata, context.reportedPerRequest);
        if (context.manualActive && exposure && context.manualExposureNs > 0 && context.manualIso > 0) {
            const double deficit = double(context.manualExposureNs) * double(context.manualIso) /
                                   (exposure->shutterNs * exposure->iso);
            if (deficit > 1.01) return std::clamp(gain * float(deficit), 1.0f / 16.0f, tonemap::kMaxAePostGain);
        }
    }
    // HDR+ Bracketed with Camera2 AE: lift by reference / actual exposure so
    // compensation steps and AE re-convergence never show (ExposureNormalizer).
    constexpr int32_t kAeModeOff = 0, kAeStateConverged = 2, kAeStateLocked = 3, kAeStateFlashRequired = 4;
    const bool tagged = metadata.optimizedStillRequestId && *metadata.optimizedStillRequestId != 0;
    if (engine_->hdrHeadroomMaxEv() < 0.0f && metadata.aeMode != kAeModeOff && !tagged) {
        const double exposure = rawrcam::capture::multiframe::linearExposure(metadata);
        if (exposure > 0.0) {
            const bool settled = metadata.aeState == kAeStateConverged || metadata.aeState == kAeStateLocked ||
                                 metadata.aeState == kAeStateFlashRequired;
            float lift;
            {
                std::lock_guard<std::mutex> lock(normalizerMutex_);
                lift = normalizer_.update(metadata.timestampNs, exposure, engine_->reportedHeadroomEvFor(metadata),
                                          settled);
            }
            return std::clamp(gain * std::exp2(lift), 1.0f / 16.0f, tonemap::kMaxAePostGain);
        }
    } else if (!tagged) {
        std::lock_guard<std::mutex> lock(normalizerMutex_);
        if (normalizer_.primed()) normalizer_.reset();
    }
    const float headroomEv = engine_->reportedHeadroomEvFor(metadata);
    return headroomEv < 0.0f ? std::min(tonemap::kMaxAePostGain, gain * std::exp2(-headroomEv)) : gain;
}

std::optional<float> SessionFrameCallbacks::normalizedLiftEv(std::uint64_t timestampNs) const {
    std::lock_guard<std::mutex> lock(normalizerMutex_);
    return normalizer_.liftFor(timestampNs);
}

void SessionFrameCallbacks::applyAutoKey(tonemap::TonemapParams& tone,
                                         const rawrcam::metadata::FrameMetadataSnapshot& metadata) {
    // The noise limit assumes the next photo's merge, so the viewfinder shows
    // the lift the photo will get.
    const auto noise = SessionEngine::liftNoiseFor(metadata, tone.aePostGain * std::exp2(tone.exposureEV),
                                                   engine_->expectedMergeFrames());
    engine_->noteLiftNoise(noise);
    engine_->applyAutoKey(tone, noise);
}

std::array<float, 4> SessionFrameCallbacks::sceneMeteringSpot() {
    const auto* controller = engine_->cameraControls_.controller();
    if (!controller) return {};
    const auto context = controller->rawrAeContext();
    if (!context.engaged || !context.spotActive) return {};
    // The Camera2 spot region's footprint: 1/8 of each dimension.
    constexpr float kHalf = 1.0f / 16.0f;
    const float cx = std::clamp(context.spotX, kHalf, 1.0f - kHalf);
    const float cy = std::clamp(context.spotY, kHalf, 1.0f - kHalf);
    return {cx - kHalf, cy - kHalf, cx + kHalf, cy + kHalf};
}

uint32_t SessionFrameCallbacks::sceneMeteringWeighting() {
    const auto* controller = engine_->cameraControls_.controller();
    if (!controller) return 0;
    const auto context = controller->rawrAeContext();
    if (!context.engaged) return 0;
    // Highlight-weighted keeps matrix weighting; it changes the policy only.
    const int m = context.config.metering;
    return m == 1 || m == 2 ? uint32_t(m) : 0u;
}

std::array<float, 4> SessionFrameCallbacks::sceneMeteringFocus() {
    const auto* controller = engine_->cameraControls_.controller();
    if (!controller) return {};
    const auto context = controller->rawrAeContext();
    // Matrix only: a spot meters the tapped area itself, the averages and
    // highlight-weighted keep their whole-frame meaning.
    if (!context.engaged || context.spotActive || context.config.metering != 0 || !context.focusArea) return {};
    return *context.focusArea;
}

bool SessionFrameCallbacks::sceneKeyMeteringEnabled() {
    return engine_->hdrHeadroomMaxEv() < 0.0f || rawrAeEngaged() || (engine_->autoKeyMode() != rawrcam::color::AutoKeyMode::Off &&
                                                   !engine_->sceneReferredRender_.load(std::memory_order_relaxed));
}

void SessionFrameCallbacks::updateHdrHeadroom(const image_scopes::SceneExposureStatsData* stats, float clipLuma,
                                              const rawrcam::metadata::FrameMetadataSnapshot& metadata,
                                              std::uint64_t timestampNs) {
    auto* controller = engine_->cameraControls_.controller();
    const float floorEv = engine_->hdrHeadroomMaxEv();
    if (!controller) return;
    if (!(floorEv < 0.0f)) {
        headroomMeter_.reset();
        return;
    }
    // Manual exposure keeps the user's shutter/ISO; one-shot frames and AE
    // transitions say nothing about the repeating exposure.
    constexpr int32_t kAeModeOff = 0, kAeStateSearching = 1;
    if (!stats || !(clipLuma > 0.0f) || metadata.aeMode == kAeModeOff || metadata.aeState == kAeStateSearching ||
        !metadata.aeCompensationSteps || (metadata.optimizedStillRequestId && *metadata.optimizedStillRequestId != 0))
        return;
    // Low light: once Camera2 AE is adding gain, every EV of headroom is an
    // EV of shadow noise; lights at night are not worth it. Judge the light
    // by the exposure Camera2 AE would use without headroom (this frame's
    // shutter is already shortened by it), and ease the floor from the full
    // underexposure up to kLowLightFloorEv over two stops, so a scene near
    // the threshold does not flip between limits.
    constexpr double kLowLightBegin = 20e6 * 1600.0;  // 1/50 s at ISO 1600, in ns x ISO
    constexpr float kLowLightFloorEv = -1.5f;
    const float appliedEv = engine_->hdrHeadroomEvFor(metadata);
    float meterFloorEv = floorEv;
    if (metadata.exposureTimeNs > 0 && metadata.sensitivity > 0 && floorEv < kLowLightFloorEv) {
        const double normal = double(metadata.exposureTimeNs) * double(metadata.sensitivity) *
                              std::exp2(-double(appliedEv));
        const float t = std::clamp(float(std::log2(normal / kLowLightBegin)) * 0.5f, 0.0f, 1.0f);
        meterFloorEv = floorEv + (kLowLightFloorEv - floorEv) * t;
    }
    const auto target =
        headroomMeter_.update(stats->weightedBins, stats->totalWeight, std::log2(clipLuma), appliedEv,
                              meterFloorEv, timestampNs);
    if (target) controller->setHdrHeadroomEv(*target);
}

std::optional<rawrcam::pipeline::FrameDiagnosticsPort::SceneKeyFeedback> SessionFrameCallbacks::updateSceneKey(
    std::uint32_t slotIndex, const rawrcam::metadata::FrameMetadataSnapshot& metadata, std::uint64_t timestampNs,
    bool recording) {
    static_assert(image_scopes::kSceneExposureBins == rawrcam::color::kSceneKeyBins);
    static_assert(image_scopes::kSceneExposureMinEv == rawrcam::color::kSceneKeyMinEv);
    static_assert(image_scopes::kSceneExposureMaxEv == rawrcam::color::kSceneKeyMaxEv);
    const auto stats = engine_->monitoringCoordinator_.scopes().consumeSceneExposure(slotIndex);
    updateRawrAe(stats ? &*stats : nullptr, engine_->monitoringCoordinator_.scopes().sceneClipLuma(slotIndex), metadata,
                 timestampNs);
    updateHdrHeadroom(stats ? &*stats : nullptr, engine_->monitoringCoordinator_.scopes().sceneClipLuma(slotIndex),
                      metadata, timestampNs);
    const auto mode = engine_->autoKeyMode();
    if (mode == rawrcam::color::AutoKeyMode::Off || !sceneKeyMeteringEnabled()) {
        autoKeySmoother_.reset();
        keySmoother_.reset();
        highlightSmoother_.reset();
        std::lock_guard<std::mutex> lock(engine_->autoKeyMutex_);
        engine_->autoKeyCurve_ = {};
        return std::nullopt;
    }
    // One-shot bracket/probe exposures must not move the repeating-scene curve.
    if (!stats || (metadata.optimizedStillRequestId && *metadata.optimizedStillRequestId != 0)) return std::nullopt;
    const auto keyEv = rawrcam::color::sceneKeyEv(stats->weightedBins);
    if (!keyEv) return std::nullopt;
    // Includes the HDR+ headroom lift, like the render gain it predicts.
    const float halGain = postGainFor(metadata);
    const auto highEv = rawrcam::color::scenePercentileEv(stats->weightedBins, 0.95f);
    if (!highEv) return std::nullopt;
    auto curve = rawrcam::color::autoKeyCurve(*keyEv, *highEv, halGain, rawrcam::color::autoKeyPolicy(mode));
    // rawr AE placed a dim scene deliberately low (night look): AutoKey's
    // lift gives that offset back first, so it never lifts the night away.
    {
        std::lock_guard<std::mutex> lock(rawrAeMutex_);
        if (rawrAe_.primed() && rawrAe_.last())
            curve.liftEv = std::max(0.0f, curve.liftEv + rawrAe_.last()->metered.nightOffsetEv);
    }
    curve.liftEv = autoKeySmoother_.update(curve.liftEv, timestampNs, recording);
    curve.keyEv = keySmoother_.update(curve.keyEv, timestampNs, recording);
    curve.highlightEv = highlightSmoother_.update(curve.highlightEv, timestampNs, recording);
    curve.liftEv = std::min(curve.liftEv, tonemap::adaptiveToneMaxLift(curve));
    {
        std::lock_guard<std::mutex> lock(engine_->autoKeyMutex_);
        engine_->autoKeyCurve_ = curve;
    }
    return SceneKeyFeedback{*keyEv, curve.liftEv};
}

void SessionFrameCallbacks::advanceStill() { engine_->capture_.single().advance(); }
bool SessionFrameCallbacks::isStillCaptureRequested() { return engine_->capture_.single().captureRequested(); }
bool SessionFrameCallbacks::isFullSizeStillCopy(const rawrcam::metadata::FrameMetadataSnapshot& metadata) {
    return engine_->isFullSizeStillCopy(metadata);
}
void SessionFrameCallbacks::observeFullSizeStillCopy(const rawrcam::metadata::FrameMetadataSnapshot& metadata,
                                                     const rawrcam::color::FrameColorTransform& color) {
    engine_->observeFullSizeBurstFrame(metadata, color);
}
bool SessionFrameCallbacks::fullSizeStillPending() { return engine_->fullSizeStillPending(); }
bool SessionFrameCallbacks::deferSubmittedFrame(const rawrcam::metadata::FrameMetadataSnapshot& metadata,
                                                const rawrcam::color::FrameColorTransform& colorState,
                                                const tonemap::TonemapParams& tonemapParams, float aePostGain,
                                                bool filmEnabled, const spektrafilm_native::FilmLook& filmLook) {
    // The processing worker may still hold frames that arrived before the
    // shutter was accepted. Do not let queue delay turn one into the photo.
    if (!engine_->capture_.acceptsStillTimestamp(metadata.timestampNs)) return false;
    auto tone = tonemapParams;
    engine_->applyAutoKey(tone, SessionEngine::liftNoiseFor(metadata, aePostGain * std::exp2(tone.exposureEV), 1u));
    // A full-size still's binned copy: the photo is the full-size frame.
    if (auto fullSize = engine_->claimFullSizeStill(metadata)) {
        auto full = metadata;
        full.cameraContext = std::move(fullSize);
        return engine_->capture_.single().deferSubmittedFrame(full, colorState, tone, aePostGain, filmEnabled,
                                                              filmLook);
    }
    if (engine_->fullSizeStillsOnly()) return false;
    return engine_->capture_.single().deferSubmittedFrame(metadata, colorState, tone, aePostGain, filmEnabled,
                                                          filmLook);
}
bool SessionFrameCallbacks::beginDeferredSnapshot(AImage* imageLease, AHardwareBuffer* ahb, std::uint64_t timestampNs,
                                                  int acquireFenceFd) {
    if (engine_->fullSizeStillClaimed(timestampNs)) return engine_->snapshotFullSizeStill(imageLease, timestampNs);
    return engine_->capture_.single().beginDeferredSnapshot(imageLease, ahb, timestampNs, acquireFenceFd);
}

bool SessionFrameCallbacks::offerRawVideoFrame(imaging::RawFrameLease& lease,
                                               const metadata::FrameMetadataSnapshot& metadata, uint32_t rawWidth,
                                               uint32_t rawHeight) {
    const auto recorder = video::McrawRecorder::active();
    return recorder && recorder->offerFrame(lease, metadata, rawWidth, rawHeight);
}

void SessionFrameCallbacks::discardScopesSlot(std::uint32_t slotIndex) {
    engine_->monitoringCoordinator_.scopes().discardFrameSlot(slotIndex);
}
void SessionFrameCallbacks::retireScopesSlot(std::uint32_t slotIndex) {
    engine_->monitoringCoordinator_.scopes().retireFrameSlot(slotIndex);
}
std::optional<rawrcam::pipeline::FrameDiagnosticsPort::RenderedFeedback> SessionFrameCallbacks::consumeExposureFeedback(
    std::uint32_t slotIndex) {
    const auto feedback = engine_->monitoringCoordinator_.scopes().consumeExposureFeedback(slotIndex);
    if (!feedback) return std::nullopt;
    RenderedFeedback out;
    out.lumaP50 = feedback->lumaP50;
    out.lumaP95 = feedback->lumaP95;
    out.lumaP99 = feedback->lumaP99;
    out.brightFraction90 = feedback->brightFraction90;
    out.brightFraction96 = feedback->brightFraction96;
    out.anyChannelClippedFraction = feedback->anyChannelClippedFraction;
    return out;
}
bool SessionFrameCallbacks::overlayNeedsRawState() { return engine_->monitoringCoordinator_.overlay().needsRawState(); }
void SessionFrameCallbacks::recordAuditFrame(const rawrcam::metadata::FrameMetadataSnapshot& metadata,
                                             const rawrcam::color::FrameColorTransform& colorState) {
    if (engine_->frameAuditWriter_) {
        engine_->frameAuditWriter_->recordCompletedFrame(metadata, colorState);
    }
}
void SessionFrameCallbacks::integrityComplete(std::uint32_t slotIndex) {
    engine_->rawIntegrityProbe_.complete(slotIndex);
}

bool SessionFrameCallbacks::cpuCopyNeedsSample() { return engine_->rawCpuCopyProbe_.needsSample(); }
rawrcam::pipeline::FrameDiagnosticsPort::CpuCopyOutcome SessionFrameCallbacks::sampleCpuCopy(
    AImage* image, AHardwareBuffer* ahb, int acquireFenceFd, std::uint64_t timestampNs) {
    int fenceFd = acquireFenceFd;
    const bool copied = engine_->rawCpuCopyProbe_.sample(image, ahb, acquireFenceFd, timestampNs, &fenceFd);
    return CpuCopyOutcome{fenceFd == -2, copied, fenceFd};
}

void SessionFrameCallbacks::configureMultiframe(uint32_t width, uint32_t height, uint32_t cfa) {
    engine_->capture_.multiframe().configure(width, height, cfa);
}
void SessionFrameCallbacks::resetMultiframe() noexcept { engine_->capture_.multiframe().reset(); }
void SessionFrameCallbacks::shutdownMultiframe() noexcept { engine_->capture_.multiframe().shutdown(); }
bool SessionFrameCallbacks::multiframeEnabled() const noexcept {
    return engine_->capture_.multiframe().experimentalMultiframeEnabled();
}
void SessionFrameCallbacks::recordMultiframeFrame(VkCommandBuffer command, VkImage rawCopy, uint64_t timestampNs,
                                                  const metadata::FrameMetadataSnapshot& metadata,
                                                  const color::FrameColorTransform& color) {
    engine_->capture_.multiframe().recordFrame(command, rawCopy, timestampNs, metadata, color);
}
void SessionFrameCallbacks::retireMultiframeFrame(uint64_t timestampNs) noexcept {
    engine_->capture_.multiframe().retireFrame(timestampNs);
}

bool SessionFrameCallbacks::detachedCaptureActive() const noexcept { return engine_->hqStillDetachedWorkActive(); }
void SessionFrameCallbacks::resetStillProcessing() noexcept { engine_->capture_.single().resetHqProcessing(); }
void SessionFrameCallbacks::configureStillSnapshot(uint32_t width, uint32_t height, uint64_t generation) {
    engine_->capture_.single().configureRawSnapshot(width, height, generation);
}
void SessionFrameCallbacks::commitMultiframeFrame(uint64_t timestampNs) noexcept {
    engine_->capture_.multiframe().commitFrame(timestampNs);
}
void SessionFrameCallbacks::discardMultiframeFrame(uint64_t timestampNs) noexcept {
    engine_->capture_.multiframe().discardFrame(timestampNs);
}
}  // namespace rawrcam::session
