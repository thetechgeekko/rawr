#pragma once
#include "color/AutoKeyExposure.h"
#include "color/ExposureNormalizer.h"
#include "color/HdrHeadroomMeter.h"
#include "color/RawrAutoExposure.h"

#include <mutex>
#include <optional>
#include "image_scopes/types.h"
#include "pipeline/FrameCapturePort.h"
#include "pipeline/FrameDiagnosticsPort.h"
#include "pipeline/FrameLifecyclePort.h"
namespace rawrcam::session {
class SessionEngine;
class SessionFrameCallbacks final : public pipeline::FrameLifecyclePort,
                                    public pipeline::FrameCapturePort,
                                    public pipeline::FrameDiagnosticsPort {
   public:
    explicit SessionFrameCallbacks(SessionEngine* engine);
    void postDiagnostic(const std::string& line) override;
    void postAudit(const std::string& line) override;
    void notifySwapchainOutOfDate() override;
    void finalizeDetachedStill() override;
    float postGainFor(const rawrcam::metadata::FrameMetadataSnapshot& metadata) override;
    // HDR+ Bracketed: the lift (EV) the viewfinder showed this frame with.
    std::optional<float> normalizedLiftEv(std::uint64_t timestampNs) const;
    void applyAutoKey(tonemap::TonemapParams& tone, const rawrcam::metadata::FrameMetadataSnapshot& metadata) override;
    void advanceStill() override;
    bool isStillCaptureRequested() override;
    bool isFullSizeStillCopy(const rawrcam::metadata::FrameMetadataSnapshot& metadata) override;
    void observeFullSizeStillCopy(const rawrcam::metadata::FrameMetadataSnapshot& metadata,
                                  const rawrcam::color::FrameColorTransform& color) override;
    bool fullSizeStillPending() override;
    bool deferSubmittedFrame(const rawrcam::metadata::FrameMetadataSnapshot& metadata,
                             const rawrcam::color::FrameColorTransform& colorState,
                             const tonemap::TonemapParams& tonemapParams, float aePostGain, bool filmEnabled,
                             const spektrafilm_native::FilmLook& filmLook) override;
    bool beginDeferredSnapshot(AImage* imageLease, AHardwareBuffer* ahb, std::uint64_t timestampNs,
                               int acquireFenceFd = -1) override;
    bool offerRawVideoFrame(imaging::RawFrameLease& lease, const metadata::FrameMetadataSnapshot& metadata,
                            uint32_t rawWidth, uint32_t rawHeight) override;
    void discardScopesSlot(std::uint32_t slotIndex) override;
    void retireScopesSlot(std::uint32_t slotIndex) override;
    std::optional<RenderedFeedback> consumeExposureFeedback(std::uint32_t slotIndex) override;
    bool sceneKeyMeteringEnabled() override;
    std::array<float, 4> sceneMeteringSpot() override;
    uint32_t sceneMeteringWeighting() override;
    std::array<float, 4> sceneMeteringFocus() override;
    std::optional<SceneKeyFeedback> updateSceneKey(std::uint32_t slotIndex,
                                                   const rawrcam::metadata::FrameMetadataSnapshot& metadata,
                                                   std::uint64_t timestampNs, bool recording) override;
    bool overlayNeedsRawState() override;
    void recordAuditFrame(const rawrcam::metadata::FrameMetadataSnapshot& metadata,
                          const rawrcam::color::FrameColorTransform& colorState) override;
    void integrityComplete(std::uint32_t slotIndex) override;
    bool cpuCopyNeedsSample() override;
    CpuCopyOutcome sampleCpuCopy(AImage* image, AHardwareBuffer* ahb, int acquireFenceFd,
                                 std::uint64_t timestampNs) override;

    bool detachedCaptureActive() const noexcept override;
    void resetStillProcessing() noexcept override;
    void configureStillSnapshot(uint32_t width, uint32_t height, uint64_t generation) override;
    void configureMultiframe(uint32_t width, uint32_t height, uint32_t cfa) override;
    void resetMultiframe() noexcept override;
    void shutdownMultiframe() noexcept override;
    bool multiframeEnabled() const noexcept override;
    void recordMultiframeFrame(VkCommandBuffer command, VkImage rawCopy, uint64_t timestampNs,
                               const metadata::FrameMetadataSnapshot& metadata,
                               const color::FrameColorTransform& color) override;
    void commitMultiframeFrame(uint64_t timestampNs) noexcept override;
    void discardMultiframeFrame(uint64_t timestampNs) noexcept override;
    void retireMultiframeFrame(uint64_t timestampNs) noexcept override;

   private:
    SessionEngine* engine_;
    // Frame-thread only (slot retire).
    rawrcam::color::AutoKeySmoother autoKeySmoother_;
    rawrcam::color::AutoKeySmoother keySmoother_, highlightSmoother_;
    rawrcam::color::HdrHeadroomMeter headroomMeter_;
    // Frame lane and still admission both ask for lifts.
    mutable std::mutex normalizerMutex_;
    rawrcam::color::ExposureNormalizer normalizer_;
    // rawr auto exposure: metered at slot retire, read by postGainFor.
    mutable std::mutex rawrAeMutex_;
    rawrcam::color::rawr_ae::Controller rawrAe_;
    std::uint64_t rawrAeCameraGeneration_ = 0;
    bool rawrAeEngaged() const;
    void updateRawrAe(const image_scopes::SceneExposureStatsData* stats, float clipLuma,
                      const rawrcam::metadata::FrameMetadataSnapshot& metadata, std::uint64_t timestampNs);
    void updateHdrHeadroom(const image_scopes::SceneExposureStatsData* stats, float clipLuma,
                           const rawrcam::metadata::FrameMetadataSnapshot& metadata, std::uint64_t timestampNs);
};

}  // namespace rawrcam::session
