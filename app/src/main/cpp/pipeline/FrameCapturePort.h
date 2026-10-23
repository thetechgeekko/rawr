#pragma once

#include <android/hardware_buffer.h>
#include <media/NdkImage.h>
#include <tonemap/TonemapEngine.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <optional>
#include <string>

#include "color/FrameColorTransform.h"
#include "imaging/RawFrameLease.h"
#include "metadata/FrameMetadataSnapshot.h"

namespace spektrafilm_native {
struct FilmLook;
}

namespace rawrcam::pipeline {

class FrameCapturePort {
   public:
    virtual ~FrameCapturePort() = default;
    virtual bool detachedCaptureActive() const noexcept = 0;
    virtual void resetStillProcessing() noexcept = 0;
    virtual void configureStillSnapshot(uint32_t width, uint32_t height, uint64_t generation) = 0;
    virtual void configureMultiframe(uint32_t width, uint32_t height, uint32_t cfa) = 0;
    virtual void resetMultiframe() noexcept = 0;
    virtual void shutdownMultiframe() noexcept = 0;
    virtual bool multiframeEnabled() const noexcept = 0;
    virtual void recordMultiframeFrame(VkCommandBuffer command, VkImage rawCopy, uint64_t timestampNs,
                                       const metadata::FrameMetadataSnapshot& metadata,
                                       const color::FrameColorTransform& color) = 0;
    virtual void commitMultiframeFrame(uint64_t timestampNs) noexcept = 0;
    virtual void discardMultiframeFrame(uint64_t timestampNs) noexcept = 0;
    virtual void retireMultiframeFrame(uint64_t timestampNs) noexcept = 0;
    virtual void advanceStill() = 0;
    virtual bool isStillCaptureRequested() = 0;
    // The viewfinder-stream copy of a full-size still (capture-stream lens):
    // claimed by the photo but never shown (the HAL fills it with a crop).
    virtual bool isFullSizeStillCopy(const rawrcam::metadata::FrameMetadataSnapshot&) { return false; }
    // Its result, with the viewfinder-stream frame's colour state (bursts merge from it).
    virtual void observeFullSizeStillCopy(const rawrcam::metadata::FrameMetadataSnapshot&,
                                          const rawrcam::color::FrameColorTransform&) {}
    // A full-size still is in flight: its copy's result comes only with the
    // full-size frame (~4 s at 200 MP), so the pairer must hold the copy longer.
    virtual bool fullSizeStillPending() { return false; }
    virtual bool deferSubmittedFrame(const rawrcam::metadata::FrameMetadataSnapshot& metadata,
                                     const rawrcam::color::FrameColorTransform& colorState,
                                     const tonemap::TonemapParams& tonemapParams, float aePostGain, bool filmEnabled,
                                     const spektrafilm_native::FilmLook& filmLook) = 0;
    virtual bool beginDeferredSnapshot(AImage* imageLease, AHardwareBuffer* ahb, std::uint64_t timestampNs,
                                       int acquireFenceFd = -1) = 0;
    // A retired frame that was recorded to video, offered to a RAW video recording
    // (MCRAW). Takes the image (moving it out of lease) and returns true when used.
    virtual bool offerRawVideoFrame(imaging::RawFrameLease& /*lease*/, const metadata::FrameMetadataSnapshot& /*metadata*/,
                                    uint32_t /*rawWidth*/, uint32_t /*rawHeight*/) {
        return false;
    }
};
}  // namespace rawrcam::pipeline
