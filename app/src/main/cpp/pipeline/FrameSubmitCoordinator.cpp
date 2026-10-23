#include "pipeline/FrameSubmitCoordinator.h"

#include <android/log.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <utility>

#include "color/ColorCalibration.h"
#include "develop/render/DenoiseProfile.h"
#include "diagnostics/logging/RuntimeTraceRecorder.h"
#include "diagnostics/probes/RawImportProbeOptions.h"
#include "diagnostics/timing/GpuTimingTracker.h"
#include "imaging/FrameLimits.h"
#include "imaging/FramePairer.h"
#include "imaging/Raw16CpuSnapshot.h"
#include "metadata/MetadataDiagnostics.h"
#include "pipeline/FrameSlotPool.h"
#include "pipeline/RawCpuUploadPool.h"
#include "pipeline/RawDevelopRecorder.h"
#include "presentation/SwapchainRenderer.h"
#include "raw_preview/RawPreview.hpp"
#include "tonemap/TonemapMath.h"
#include "video/VideoSession.h"
#include "video_pipeline/VideoCrop.h"
#include "vulkan/RawAhbImporter.h"
#include "vulkan/Synchronization.h"
#include "vulkan/VulkanContext.h"

namespace rawrcam::pipeline {
namespace {
constexpr const char* kTag = "RawrCamNative";
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, kTag, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, kTag, __VA_ARGS__)

void vkCheck(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        throw std::runtime_error(std::string(what) + " VkResult=" + std::to_string(r));
    }
}

int64_t clockNs(clockid_t clock) {
    timespec now{};
    clock_gettime(clock, &now);
    return int64_t(now.tv_sec) * 1'000'000'000 + now.tv_nsec;
}

// The sensor timestamp on the encoder's clock (CLOCK_MONOTONIC), so the
// encoded PTS records capture time rather than when the frame was queued.
int64_t encoderTimestampNs(const rawrcam::metadata::FrameMetadataSnapshot& metadata, uint64_t sensorNs) {
    const bool boottime = metadata.cameraContext && metadata.cameraContext->sensorTimestampRealtime;
    const int64_t offset = boottime ? clockNs(CLOCK_BOOTTIME) - clockNs(CLOCK_MONOTONIC) : 0;
    return static_cast<int64_t>(sensorNs) - offset;
}
}  // namespace

FrameSubmitCoordinator::FrameSubmitCoordinator(rawrcam::vulkan::VulkanContext& vulkanContext,
                                               pipeline::FrameSlotPool& frameSlots,
                                               rawrcam::diagnostics::GpuTimingTracker& performanceTracker,
                                               RealtimeResources& resources,
                                               rawrcam::presentation::SwapchainRenderer& presentationRenderer,
                                               FrameLifecyclePort& lifecyclePort, FrameCapturePort& capturePort,
                                               FrameDiagnosticsPort& diagnosticsPort, std::mutex& queueSubmitMutex,
                                               std::string filesDir)
    : vulkanContext_(vulkanContext),
      frameSlots_(frameSlots),
      performanceTracker_(performanceTracker),
      resources_(resources),
      swapchainRenderer_(presentationRenderer),
      lifecyclePort_(lifecyclePort),
      capturePort_(capturePort),
      diagnosticsPort_(diagnosticsPort),
      queueSubmitMutex_(queueSubmitMutex),
      filesDir_(std::move(filesDir)),
      framePairer_(std::make_unique<rawrcam::imaging::FramePairer>()) {}

FrameSubmitCoordinator::~FrameSubmitCoordinator() = default;

void FrameSubmitCoordinator::configure(const FrameSubmitConfiguration& config) {
    config_ = config;
    config_.diagnosticMode = diagnosticMode_;
    config_.experimentalZeroCopy = experimentalZeroCopy_;
    configured_ = true;
    cpuUpload_.initialize(vulkanContext_.physicalDevice(), vulkanContext_.device(), vulkanContext_.queueFamily());
    // `adb shell setprop debug.rawr.force_cpu_ingress 1` exercises the CPU
    // upload path on devices whose GPU import works.
    {
        char value[PROP_VALUE_MAX]{};
        const bool forced = __system_property_get("debug.rawr.force_cpu_ingress", value) > 0 && value[0] == '1';
        if (forced) forceCpuIngress_.store(true, std::memory_order_relaxed);
        // Exercises the RAW16 GPU buffer copy on devices whose image import works.
        if (__system_property_get("debug.rawr.raw16_buffer_ingress", value) > 0 && value[0] == '1')
            raw16ImageImportLatched_.store(true, std::memory_order_relaxed);
        raw10ParityEnabled_ = __system_property_get("debug.rawr.raw10_parity", value) > 0 && value[0] == '1';
    }
    cpuUpload_.configure(config_.rawWidth, config_.rawHeight);
    cpuUpload_.configureRemosaic(config_.cfaBlock, config_.cfa, config_.cfaQuality);
    cpuIngressActive_.store(false, std::memory_order_relaxed);
    raw16GpuCopyDeferredLogged_ = false;
    pipelineAuditMetadataCount_ = 0;
    capturePort_.configureMultiframe(config_.rawWidth, config_.rawHeight, config_.cfa);
    pipelineAuditPairCount_ = 0;
    pipelineAuditSubmitCount_ = 0;
    metadataDiagnosticCount_ = 0;
    framePairer_->clear();
}

void FrameSubmitCoordinator::resetConfiguration() noexcept {
    capturePort_.resetMultiframe();
    configured_ = false;
    config_ = {};
    framePairer_->clear();
}

void FrameSubmitCoordinator::shutdownDeviceResources() noexcept {
    resetConfiguration();
    cpuUpload_.destroy();
    capturePort_.shutdownMultiframe();
}

void FrameSubmitCoordinator::clearPairing() noexcept { framePairer_->clear(); }

bool FrameSubmitCoordinator::videoActive() const noexcept {
    return videoOutput_ && videoOutput_->ready() && config_.diagnosticMode == 0u;
}

void FrameSubmitCoordinator::trimPairer() {
    // While a full-size still is in flight, hold its viewfinder copy (and the
    // frames behind it) until the still's late result pairs it.
    const size_t maxImages = capturePort_.fullSizeStillPending() ? kMaxPairerImagesFullSize : kMaxPairerImages;
    const uint64_t dropped = framePairer_->trim(maxImages, kMaxPairerMetadata);
    performanceTracker_.recordDropped(dropped);
    if (videoActive()) videoDropReasons_.pairer += dropped;
    if (dropped > 0 && pairerPressureLogCount_ % 120 == 0) {
        LOGI("PAIRER_PRESSURE dropped=%llu pendingImages=%zu pendingMetadata=%zu", (unsigned long long)dropped,
             framePairer_->imageCount(), framePairer_->metadataCount());
    }
    if (dropped > 0) ++pairerPressureLogCount_;
}

bool FrameSubmitCoordinator::submitMetadata(const rawrcam::metadata::FrameMetadataSnapshot& metadata) {
    if (!configured_ || !metadata.cameraContext ||
        metadata.cameraContext->cameraContextGeneration != config_.generation) {
        return false;
    }
    if (metadata.cameraContext->geometry.rawBufferWidth != config_.rawWidth ||
        metadata.cameraContext->geometry.rawBufferHeight != config_.rawHeight ||
        metadata.cameraContext->rawPreviewCfa != config_.cfa) {
        return false;
    }

    const auto colorState =
        rawrcam::color::deriveFrameColorTransform(metadata, rawrcam::color::parsePreviewColorMode(colorMode_));
    rawrcam::diagnostics::RuntimeTraceRecorder::instance().setCurrentCameraId(
        std::atoi(metadata.cameraContext->cameraId.c_str()));
    rawrcam::diagnostics::RuntimeTraceRecorder::instance().record(
        rawrcam::diagnostics::RuntimeTraceStage::Metadata, metadata.timestampNs, metadata.frameOrdinal, -1,
        metadata.exposureTimeNs, metadata.sensitivity, 0, 0, std::atoi(metadata.cameraContext->cameraId.c_str()));
    framePairer_->putMetadata(metadata.timestampNs, rawrcam::imaging::FrameRenderMetadata{metadata, colorState});

    if (pipelineAuditMetadataCount_ < 6) {
        ++pipelineAuditMetadataCount_;
        lifecyclePort_.postAudit(
            "PIPELINE_METADATA_ACCEPTED generation=" + std::to_string(metadata.cameraContext->cameraContextGeneration) +
            " timestampNs=" + std::to_string(metadata.timestampNs) +
            " raw=" + std::to_string(metadata.cameraContext->geometry.rawBufferWidth) + "x" +
            std::to_string(metadata.cameraContext->geometry.rawBufferHeight) + " whiteSource=" +
            rawrcam::metadata::effectiveWhiteLevelSourceName(metadata.effectiveWhiteLevelSource));
    }
    if (metadataDiagnosticCount_ < 3) {
        lifecyclePort_.postDiagnostic(rawrcam::metadata::describe(metadata));
        lifecyclePort_.postDiagnostic(rawrcam::color::describe(colorState, metadata));
        ++metadataDiagnosticCount_;
    }

    tryPair(metadata.timestampNs);
    trimPairer();
    return true;
}

void FrameSubmitCoordinator::onRawFrame(rawrcam::imaging::RawFrameLease frame) {
    if (!configured_ || frame.get().generation != config_.generation) {
        performanceTracker_.recordDropped();
        if (videoActive()) ++videoDropReasons_.stale;
        return;
    }

    const uint64_t key = frame.get().timestampNs;
    rawrcam::diagnostics::RuntimeTraceRecorder::instance().record(rawrcam::diagnostics::RuntimeTraceStage::RawImage,
                                                                  key);
    framePairer_->putImage(key, std::move(frame));
    tryPair(key);
    trimPairer();
}

void FrameSubmitCoordinator::tryPair(uint64_t timestampNs) {
    rawrcam::imaging::MatchedFrame matched{};
    if (!framePairer_->takeMatched(timestampNs, &matched)) return;
    rawrcam::diagnostics::RuntimeTraceRecorder::instance().record(
        rawrcam::diagnostics::RuntimeTraceStage::Paired, matched.timestampNs, matched.metadata.snapshot.frameOrdinal,
        -1, matched.metadata.snapshot.exposureTimeNs, matched.metadata.snapshot.sensitivity, 0, 0,
        std::atoi(matched.metadata.snapshot.cameraContext->cameraId.c_str()));

    if (pipelineAuditPairCount_ < 6) {
        ++pipelineAuditPairCount_;
        lifecyclePort_.postAudit("PIPELINE_FRAME_PAIR_MATCHED generation=" + std::to_string(config_.generation) +
                                 " timestampNs=" + std::to_string(timestampNs) + " effectiveWhiteLevel=" +
                                 std::to_string(matched.metadata.snapshot.effectiveWhiteLevel) + " whiteSource=" +
                                 rawrcam::metadata::effectiveWhiteLevelSourceName(
                                     matched.metadata.snapshot.effectiveWhiteLevelSource));
    }

    (void)submitMatchedFrame(matched);
}

bool FrameSubmitCoordinator::submitMatchedFrame(rawrcam::imaging::MatchedFrame& matched) {
    auto& image = matched.image;
    auto& meta = matched.metadata;
    const uint64_t timestampNs = matched.timestampNs;

    // Calibration and its transition frames must never reach presentation,
    // scopes, the ZSL ring, or a pending single-frame capture. The camera has
    // already consumed their results for ISO calibration. Keep the previous
    // displayed image; the lease releases the camera buffer/fence unimported.
    if (meta.snapshot.suppressPreview ||
        meta.snapshot.optimizedStillRequestId == metadata::kSensitivityProbeRequestId) {
        return false;
    }

    auto applyCpuSnapshotFence = [&](int gpuAcquireFenceFd, const char* failureTag) -> bool {
        if (gpuAcquireFenceFd == -2) {
            lifecyclePort_.postDiagnostic(std::string(failureTag) + " timestampNs=" + std::to_string(timestampNs));
            image.reset();
            performanceTracker_.recordDropped();
            return false;
        }
        image.resetFence(gpuAcquireFenceFd);
        return true;
    };

    capturePort_.advanceStill();
    lifecyclePort_.finalizeDetachedStill();
    if (diagnosticsPort_.cpuCopyNeedsSample()) {
        const auto outcome = diagnosticsPort_.sampleCpuCopy(image.image(), image.ahb(), image.fence(), timestampNs);
        if (!applyCpuSnapshotFence(outcome.frameRejected ? -2 : outcome.fenceFd,
                                   "CPU_RAW_COPY_PROBE_FRAME_REJECTED reason=cpu_unlock_failed")) {
            return false;
        }
        if (!outcome.copied) {
            lifecyclePort_.postDiagnostic("CPU_RAW_COPY_PROBE_FRAME_NOT_SAMPLED timestampNs=" +
                                          std::to_string(timestampNs));
        }
    }

    const bool fullSizeCopy = capturePort_.isFullSizeStillCopy(meta.snapshot);
    if (fullSizeCopy) capturePort_.observeFullSizeStillCopy(meta.snapshot, meta.colorState);
    const bool accepted = !fullSizeCopy &&
                          submitAhb(timestampNs, image, meta.snapshot.blackLevelPhysicalRggb,
                                    meta.snapshot.effectiveWhiteLevel, meta.colorState.baselineWbRggb,
                                    meta.colorState.cameraToLinearSrgbRowMajor, meta.snapshot, meta.colorState);
    if (capturePort_.isStillCaptureRequested()) {
        const bool claimed =
            capturePort_.deferSubmittedFrame(meta.snapshot, meta.colorState, tonemapParams_,
                                             lifecyclePort_.postGainFor(meta.snapshot), filmSimEnabled_, filmLook_);
        if (claimed && !accepted &&
            capturePort_.beginDeferredSnapshot(image.image(), image.ahb(), timestampNs, image.fence())) {
            // An unavailable/background swapchain must not prevent RAW acquisition.
            // The worker waits on the camera fence, then releases the image it now owns.
            (void)image.takeImage().release();
        }
        lifecyclePort_.finalizeDetachedStill();
    }
    if (pipelineAuditSubmitCount_ < 6) {
        ++pipelineAuditSubmitCount_;
        lifecyclePort_.postAudit(std::string("PIPELINE_GPU_SUBMIT_RESULT generation=") +
                                 std::to_string(config_.generation) + " timestampNs=" + std::to_string(timestampNs) +
                                 " accepted=" + (accepted ? "true" : "false"));
    }

    image.reset();
    return accepted;
}

std::optional<FrameSubmitCoordinator::AcquiredSubmit> FrameSubmitCoordinator::acquireSubmitSlot(
    const SubmitParams& params, pipeline::FrameSlot* slot, uint32_t slotIndex) {
    auto& trace = rawrcam::diagnostics::RuntimeTraceRecorder::instance();
    const auto& metadata = *params.metadata;

    if (!params.ahb || !resources_.importer()) {
        throw std::runtime_error("RAW AHardwareBuffer/importer is null");
    }
    rawrcam::vulkan::ImportedRaw& raw = ingestRaw(params, slotIndex);

    std::optional<uint32_t> videoIndex;
    // Frames captured before the encoder swapchain existed predate the
    // recording; stamping them would put PTS before the file's start.
    const bool preRoll = videoOutput_ && videoOutput_->ready() && videoOutput_->stampsPresentTime() &&
                         encoderTimestampNs(metadata, params.timestampNs) < videoOutput_->startedAtNs();
    if (videoOutput_ && videoOutput_->ready() && config_.diagnosticMode == 0u && !preRoll) {
        if (videoOutput_->extent().width <= config_.rawWidth && videoOutput_->extent().height <= config_.rawHeight) {
            uint32_t index = 0;
            if (videoOutput_->acquire(slotIndex, &index))
                videoIndex = index;
            else {
                ++videoDrops_;
                ++videoDropReasons_.encoderBusy;
            }
        } else {
            ++videoDrops_;
            ++videoDropReasons_.geometry;
        }
    }
    std::optional<uint32_t> swapIndex;
    if (slot->previewSubmitted) {
        const VkResult preview = vkGetFenceStatus(vulkanContext_.device(), slot->previewFence);
        if (preview == VK_SUCCESS)
            slot->previewSubmitted = false;
        else if (preview != VK_NOT_READY)
            vkCheck(preview, "video preview fence status");
    }
    if (!presentationPaused_ && !metadata.hidePreview && swapchainRenderer_.ready() && !slot->previewSubmitted) {
        uint32_t index = 0;
        const VkResult acquire = swapchainRenderer_.acquireNextImage(slot->imageAvailable, &index);
        trace.record(rawrcam::diagnostics::RuntimeTraceStage::SwapAcquire, params.timestampNs, metadata.frameOrdinal,
                     static_cast<int32_t>(slotIndex), metadata.exposureTimeNs, metadata.sensitivity, 0u, acquire);
        if (acquire == VK_SUCCESS || acquire == VK_SUBOPTIMAL_KHR) {
            swapIndex = index;
        } else if (acquire == VK_NOT_READY || acquire == VK_TIMEOUT || acquire == VK_ERROR_OUT_OF_DATE_KHR) {
            if (acquire == VK_ERROR_OUT_OF_DATE_KHR) lifecyclePort_.notifySwapchainOutOfDate();
            if (videoIndex)
                ++previewSkippedDuringVideo_;
            else {
                performanceTracker_.recordDropped();
                return std::nullopt;
            }
        } else {
            vkCheck(acquire, "vkAcquireNextImageKHR");
        }
    } else if (videoIndex) {
        ++previewSkippedDuringVideo_;
    }
    if (!swapIndex && !videoIndex) {
        performanceTracker_.recordDropped();
        return std::nullopt;
    }
    return AcquiredSubmit{slot, slotIndex, swapIndex, videoIndex, &raw};
}

rawrcam::vulkan::ImportedRaw& FrameSubmitCoordinator::ingestRaw(const SubmitParams& params, uint32_t slotIndex) {
    const auto format = rawrcam::imaging::rawPixelFormatOf(params.lease->image());
    const bool raw10 = format == rawrcam::geometry::RawPixelFormat::Raw10;
    // A block-CFA lens skips the zero-copy image import: consumers must read
    // the remosaiced copy, never the camera's block mosaic.
    if (!raw10 && !cpuIngressRequired() && !raw16ImageImportLatched_.load(std::memory_order_relaxed) &&
        config_.cfaBlock <= 1u) {
        try {
            rawrcam::vulkan::ImportedRaw& raw =
                resources_.importer()->import(params.ahb, config_.rawWidth, config_.rawHeight, importedRawUsage(),
                                              diagnostics::rawImportProbeOptions(config_.diagnosticMode));
            if (config_.diagnosticMode == 16u || config_.diagnosticMode == 0u)
                resources_.importer()->ensureBufferImport(params.ahb);
            return raw;
        } catch (const std::exception& e) {
            // Diagnostic modes exist to exercise the import itself; never mask them.
            if (config_.diagnosticMode != 0u) throw;
            // Some drivers reject the R16 image import of camera AHBs but accept
            // the buffer import: try the GPU copy before the CPU upload.
            raw16ImageImportLatched_.store(true, std::memory_order_relaxed);
            const std::string line = std::string("RAW_INGRESS_FALLBACK to=gpu_copy reason=") + e.what();
            LOGE("%s", line.c_str());
            lifecyclePort_.postDiagnostic(line);
        }
    }
    // RAW16 whose image import was rejected: copy the imported camera buffer
    // into the pool's owned R16 image on the GPU.
    if (!raw10 && !cpuIngressRequired() && config_.diagnosticMode == 0u) {
        try {
            const auto& camera =
                resources_.importer()->importRaw16Buffer(params.ahb, config_.rawWidth, config_.rawHeight);
            rawrcam::vulkan::ImportedRaw& raw = cpuUpload_.gpuCopy(slotIndex, camera);
            if (!raw16GpuCopyLogged_) {
                raw16GpuCopyLogged_ = true;
                const std::string line = std::string(raw.remosaicker ? "RAW_INGRESS_GPU_REMOSAIC" : "RAW_INGRESS_GPU_COPY") +
                                         " format=RAW16 rowStrideBytes=" + std::to_string(camera.rowStrideBytes) +
                                         " cfaBlock=" + std::to_string(config_.cfaBlock) +
                                         " quality=" + (config_.cfaQuality == 0 ? "fast" : "standard");
                LOGI("%s", line.c_str());
                lifecyclePort_.postDiagnostic(line);
            }
            return raw;
        } catch (const std::exception& e) {
            // A reader configured before the latch has no GPU buffer usage, so
            // its failure says nothing about the driver: upload this session
            // and retry once the reader is reconfigured for buffer access.
            AHardwareBuffer_Desc desc{};
            if (params.ahb) AHardwareBuffer_describe(params.ahb, &desc);
            const bool bufferReader = (desc.usage & AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER) != 0 ||
                                      cpuOnlyReader_.load(std::memory_order_relaxed);
            if (bufferReader) cpuIngressLatched_.store(true, std::memory_order_relaxed);
            if (bufferReader || !raw16GpuCopyDeferredLogged_) {
                raw16GpuCopyDeferredLogged_ = true;
                const std::string line = std::string("RAW_INGRESS_FALLBACK from=gpu_copy to=cpu_upload latched=") +
                                         (bufferReader ? "true" : "false") + " reason=" + e.what();
                LOGE("%s", line.c_str());
                lifecyclePort_.postDiagnostic(line);
            }
        }
    }
    // RAW10: unpack the imported camera buffer on the GPU into the pool's
    // owned R16 image. Diagnostic modes keep the CPU route they were built on.
    if (raw10 && !cpuIngressRequired() && !raw10GpuUnpackLatched_.load(std::memory_order_relaxed) &&
        config_.diagnosticMode == 0u) {
        try {
            int32_t rowStride = 0;
            if (!params.lease->image() || AImage_getPlaneRowStride(params.lease->image(), 0, &rowStride) != AMEDIA_OK ||
                rowStride <= 0)
                throw std::runtime_error("RAW10 row stride unavailable");
            const auto& camera = resources_.importer()->importRaw10Buffer(
                params.ahb, config_.rawWidth, config_.rawHeight, static_cast<uint32_t>(rowStride));
            if (raw10ParityEnabled_) {
                const std::string report = cpuUpload_.takeRaw10ParityReport(slotIndex);
                if (!report.empty()) {
                    LOGI("%s", report.c_str());
                    lifecyclePort_.postDiagnostic(report);
                }
                if (cpuUpload_.raw10ParityIdle() && (++raw10ParityFrame_ % 30u) == 0u)
                    cpuUpload_.armRaw10Parity(slotIndex, params.lease->image(), params.ahb, params.acquireFenceFd);
            }
            rawrcam::vulkan::ImportedRaw& raw = cpuUpload_.gpuUnpack(slotIndex, camera);
            if (!raw10GpuUnpackLogged_) {
                raw10GpuUnpackLogged_ = true;
                const std::string line =
                    "RAW_INGRESS_GPU_UNPACK format=RAW10 rowStrideBytes=" + std::to_string(rowStride);
                LOGI("%s", line.c_str());
                lifecyclePort_.postDiagnostic(line);
            }
            return raw;
        } catch (const std::exception& e) {
            raw10GpuUnpackLatched_.store(true, std::memory_order_relaxed);
            const std::string line = std::string("RAW_INGRESS_FALLBACK from=gpu_unpack to=cpu_upload reason=") + e.what();
            LOGE("%s", line.c_str());
            lifecyclePort_.postDiagnostic(line);
        }
    }
    cpuIngressActive_.store(true, std::memory_order_relaxed);
    if (!cpuIngressLogged_) {
        cpuIngressLogged_ = true;
        const std::string line = std::string("RAW_INGRESS_CPU_UPLOAD active=true format=") +
                                 (raw10 ? "RAW10" : "RAW16") +
                                 " forced=" + (forceCpuIngress_.load(std::memory_order_relaxed) ? "true" : "false");
        LOGI("%s", line.c_str());
        lifecyclePort_.postDiagnostic(line);
    }
    return cpuUpload_.upload(slotIndex, params.lease->image(), params.ahb, params.acquireFenceFd);
}

void FrameSubmitCoordinator::submitSplitVideoAndMonitor(const SubmitParams& params, const AcquiredSubmit& acquired,
                                                        bool& submitted) {
    auto* slot = acquired.slot;
    const uint32_t slotIndex = acquired.slotIndex;
    const auto& metadata = *params.metadata;
    const bool directBuffer = acquired.raw->importBufferAvailable;
    const bool bridgeCopy = !directBuffer && slot->rawCopy.image != VK_NULL_HANDLE;
    // A GPU-unpacked frame reads the camera buffer in a compute dispatch
    // ahead of any bridge copy, so the camera fence must gate compute too.
    const VkPipelineStageFlags rawWaitStage =
        (bridgeCopy ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT) |
        (acquired.raw->gpuUnpack ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT : 0u);
    vkCheck(vkResetFences(vulkanContext_.device(), 1, &slot->fence), "video reset fence");
    vkCheck(vkResetCommandBuffer(slot->videoCommand, 0), "video reset command");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkCheck(vkBeginCommandBuffer(slot->videoCommand, &begin), "video begin command");
    performanceTracker_.beginFrame(slot->videoCommand, slotIndex, false);

    if (bridgeCopy) {
        acquireRawInputImage(slot->videoCommand, *acquired.raw, vulkanContext_.queueFamily(),
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        rawrcam::vulkan::recordImageCopy(slot->videoCommand, acquired.raw->image, slot->rawCopy.image, config_.rawWidth,
                                         config_.rawHeight);
        rawrcam::vulkan::transferWriteToComputeRead(slot->videoCommand, slot->rawCopy.image);
        releaseRawInputImage(slot->videoCommand, *acquired.raw, vulkanContext_.queueFamily(),
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    } else if (!directBuffer) {
        if (!acquired.raw->view) throw std::runtime_error("video RAW image view is unavailable");
        acquireRawInputImage(slot->videoCommand, *acquired.raw, vulkanContext_.queueFamily(),
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    performanceTracker_.markRawInputReady(slot->videoCommand, slotIndex);

    rawrcam::video::VideoDemosaic::Frame frame{};
    frame.rawView = bridgeCopy ? slot->rawCopy.view : acquired.raw->view;
    frame.rawBuffer = directBuffer ? acquired.raw->importBuffer : VK_NULL_HANDLE;
    frame.rawStridePixels = directBuffer ? acquired.raw->importBufferStridePixels : 0u;
    frame.black = params.black;
    frame.wb = params.wb;
    frame.white = params.white;
    frame.cfa = config_.cfa;
    frame.cameraToLinearSrgb = params.sensorToSrgb;
    if (config_.lensShadingCorrectionEnabled && !metadata.lensShadingMap.empty()) {
        frame.lensShading = metadata.lensShadingMap.data();
        frame.lensShadingCount = metadata.lensShadingMap.size();
        frame.lensShadingWidth = metadata.lensShadingMapWidth;
        frame.lensShadingHeight = metadata.lensShadingMapHeight;
    }
    frame.noiseProfileValid = rawrcam::develop::rendered::resolveDenoiseNoise(metadata, frame.noiseA, frame.noiseB);
    auto tone = tonemapParams_;
    tone.aePostGain = previewPostGain(metadata);
    lifecyclePort_.applyAutoKey(tone, metadata);
    const auto cameraToAp1 = RawDevelopRecorder::composeCameraToAp1(params.sensorToSrgb);

    // The first submission waits for the camera buffer and the encoder image
    // (its layout transition is recorded at the start of the frame).
    std::array<VkSemaphore, 2> waits{};
    std::array<VkPipelineStageFlags, 2> stages{};
    uint32_t waitCount = 0;
    if (params.acquireFenceFd >= 0) {
        const int fd = dup(params.acquireFenceFd);
        if (fd < 0) throw std::runtime_error("dup video camera fence failed");
        VkImportSemaphoreFdInfoKHR import{};
        import.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
        import.semaphore = slot->cameraAcquire;
        import.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
        import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        import.fd = fd;
        const VkResult result = vulkanContext_.importSemaphoreFd()(vulkanContext_.device(), &import);
        if (result != VK_SUCCESS) {
            close(fd);
            vkCheck(result, "video import camera fence");
        }
        waits[waitCount] = slot->cameraAcquire;
        stages[waitCount++] = rawWaitStage;
    }
    waits[waitCount] = videoOutput_->available(slotIndex);
    stages[waitCount++] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

    // Each frame goes to the queue in three parts (demosaic, post, tonemap).
    VkCommandBuffer current = slot->videoCommand;
    uint32_t partsSubmitted = 0;
    const auto submitPart = [&](VkCommandBuffer command, const VkSemaphore* signal, VkFence fence) {
        VkSubmitInfo part{};
        part.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        if (partsSubmitted == 0) {
            part.waitSemaphoreCount = waitCount;
            part.pWaitSemaphores = waits.data();
            part.pWaitDstStageMask = stages.data();
        }
        part.commandBufferCount = command ? 1u : 0u;
        part.pCommandBuffers = &command;
        part.signalSemaphoreCount = signal ? 1u : 0u;
        part.pSignalSemaphores = signal;
        std::lock_guard<std::mutex> queueLock(queueSubmitMutex_);
        vkCheck(vkQueueSubmit(vulkanContext_.queue(), 1, &part, fence), "video queue submit");
        ++partsSubmitted;
    };
    const auto splitSubmit = [&](VkCommandBuffer command) -> VkCommandBuffer {
        vkCheck(vkEndCommandBuffer(command), "video end partial command");
        submitPart(command, nullptr, VK_NULL_HANDLE);
        current = slot->videoStageCommands.at(partsSubmitted - 1);
        vkCheck(vkResetCommandBuffer(current, 0), "video reset partial command");
        vkCheck(vkBeginCommandBuffer(current, &begin), "video begin partial command");
        return current;
    };
    try {
        performanceTracker_.markVideoProcessBegin(current, slotIndex, true);
        videoOutput_->setLensDistortion(metadata.cameraContext ? metadata.cameraContext->profileDistortion
                                                               : std::nullopt);
        videoOutput_->setLensMirror(metadata.cameraContext ? metadata.cameraContext->profileMirror : 0u);
        videoOutput_->setDcpProfile(metadata.cameraContext ? metadata.cameraContext->dcpProfile : nullptr,
                                    params.colorState->calibrationWeight1);
        videoOutput_->record(current, slotIndex, *acquired.videoIndex, config_.rawWidth, config_.rawHeight, frame,
                             cameraToAp1, tone, performanceTracker_, acquired.swapIndex.has_value(), splitSubmit,
                             [&](VkCommandBuffer cmd, VkImageView view, uint32_t width, uint32_t height) {
                                 if (!diagnosticsPort_.sceneKeyMeteringEnabled()) return;
                                 // Spot and focus are on the full frame; the video image is its record crop.
                                 const auto crop = rawrcam::video::videoSourceRect(config_.rawWidth, config_.rawHeight,
                                                                                   width, height);
                                 const auto toCrop = [&](const std::array<float, 4>& source) {
                                     std::array<float, 4> out{};
                                     if (!(source[2] > source[0]) || crop.width <= 0 || crop.height <= 0) return out;
                                     for (int i = 0; i < 4; ++i) {
                                         const bool x = (i % 2) == 0;
                                         const float raw = source[i] * float(x ? config_.rawWidth : config_.rawHeight);
                                         const float origin = float(x ? crop.x : crop.y);
                                         const float extent = float(x ? crop.width : crop.height);
                                         out[i] = std::clamp((raw - origin) / extent, 0.0f, 1.0f);
                                     }
                                     if (!(out[2] > out[0] && out[3] > out[1])) out = {};
                                     return out;
                                 };
                                 resources_.recorder()->recordSceneExposure(
                                     cmd, slotIndex, view, width, height, params.sensorToSrgb, params.wb,
                                     toCrop(diagnosticsPort_.sceneMeteringSpot()),
                                     diagnosticsPort_.sceneMeteringWeighting(),
                                     toCrop(diagnosticsPort_.sceneMeteringFocus()));
                             });
        if (!directBuffer && !bridgeCopy) {
            releaseRawInputImage(current, *acquired.raw, vulkanContext_.queueFamily(),
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        }
        performanceTracker_.markVideoProcessDone(current, slotIndex);
        performanceTracker_.markRawDone(current, slotIndex);
        performanceTracker_.markTonemapInputReady(current, slotIndex);
        performanceTracker_.markTonemapDone(current, slotIndex);
        performanceTracker_.markTonemapRepeatInputReady(current, slotIndex);
        performanceTracker_.markTonemapRepeatDone(current, slotIndex);
        performanceTracker_.markScopesDone(current, slotIndex);
        performanceTracker_.markOverlayDone(current, slotIndex);
        performanceTracker_.markPresentationDone(current, slotIndex);
        performanceTracker_.markFrameDone(current, slotIndex);
        vkCheck(vkEndCommandBuffer(current), "video end command");
        const VkSemaphore rendered = videoOutput_->rendered(slotIndex);
        submitPart(current, &rendered, slot->fence);
    } catch (...) {
        // Earlier parts are already queued: retire the slot fence behind them
        // so the slot is not reused while they run.
        // The camera image stays leased to the slot until then.
        if (partsSubmitted > 0) {
            submitPart(VK_NULL_HANDLE, nullptr, slot->fence);
            submitted = true;
            slot->submitted = true;
            slot->imageLease = params.lease->takeImage();
            slot->timestampNs = params.timestampNs;
        }
        throw;
    }
    submitted = true;
    slot->submitted = true;
    slot->imageLease = params.lease->takeImage();
    slot->timestampNs = params.timestampNs;
    slot->metadataSnapshot = metadata;
    slot->colorStateSnapshot = *params.colorState;
    slot->videoRecorded = true;
    performanceTracker_.recordSubmitted();

    VkResult videoPresent;
    {
        std::lock_guard<std::mutex> queueLock(queueSubmitMutex_);
        videoPresent = videoOutput_->present(vulkanContext_.queue(), slotIndex, *acquired.videoIndex,
                                             static_cast<uint64_t>(encoderTimestampNs(metadata, params.timestampNs)));
    }
    if (videoPresent == VK_SUCCESS || videoPresent == VK_SUBOPTIMAL_KHR)
        ++videoSubmitted_;
    else {
        ++videoDrops_;
        ++videoDropReasons_.presentFail;
        lifecyclePort_.postDiagnostic("VIDEO_PRESENT_FAIL result=" + std::to_string(videoPresent));
    }

    if (!acquired.swapIndex) return;
    try {
        vkCheck(vkResetCommandBuffer(slot->command, 0), "video monitor reset command");
        vkCheck(vkBeginCommandBuffer(slot->command, &begin), "video monitor begin command");
        videoOutput_->beginMonitorTiming(slot->command, slotIndex);
        const auto extent = videoOutput_->extent();
        resources_.recorder()->recordVideoScopes(slot->command, slotIndex, videoOutput_->monitorView(slotIndex),
                                                 videoOutput_->monitorImage(slotIndex), extent.width, extent.height,
                                                 config_.sensorOrientationDegrees, scopeDeviceRotationDegrees_);
        swapchainRenderer_.record(slot->command, slotIndex, *acquired.swapIndex, extent.width, extent.height,
                                  config_.sensorOrientationDegrees, displayRotationDegrees_, 0u, false, 0u, true);
        resources_.recorder()->restoreVideoScopes(slot->command, slotIndex);
        videoOutput_->endMonitorTiming(slot->command, slotIndex);
        vkCheck(vkEndCommandBuffer(slot->command), "video monitor end command");
        vkCheck(vkResetFences(vulkanContext_.device(), 1, &slot->previewFence), "video monitor reset fence");
        const VkPipelineStageFlags previewStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo preview{};
        preview.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        preview.waitSemaphoreCount = 1;
        preview.pWaitSemaphores = &slot->imageAvailable;
        preview.pWaitDstStageMask = &previewStage;
        preview.commandBufferCount = 1;
        preview.pCommandBuffers = &slot->command;
        preview.signalSemaphoreCount = 1;
        preview.pSignalSemaphores = &slot->renderFinished;
        {
            std::lock_guard<std::mutex> queueLock(queueSubmitMutex_);
            vkCheck(vkQueueSubmit(vulkanContext_.queue(), 1, &preview, slot->previewFence),
                    "video monitor queue submit");
        }
        slot->previewSubmitted = true;
        VkResult present;
        {
            std::lock_guard<std::mutex> queueLock(queueSubmitMutex_);
            present = swapchainRenderer_.present(vulkanContext_.queue(), slot->renderFinished, *acquired.swapIndex);
        }
        if (present == VK_ERROR_OUT_OF_DATE_KHR) {
            ++previewSkippedDuringVideo_;
            lifecyclePort_.notifySwapchainOutOfDate();
        } else if (present != VK_SUCCESS && present != VK_SUBOPTIMAL_KHR) {
            vkCheck(present, "video monitor present");
        }
    } catch (const std::exception& error) {
        ++previewSkippedDuringVideo_;
        lifecyclePort_.postDiagnostic(std::string("VIDEO_MONITOR_FAIL ") + error.what());
        (void)vulkanContext_.waitIdle();
        try {
            lifecyclePort_.notifySwapchainOutOfDate();
            frameSlots_.recreatePreviewSyncObjects(slotIndex);
        } catch (const std::exception& recovery) {
            lifecyclePort_.postDiagnostic(std::string("VIDEO_MONITOR_RECOVERY_FAIL ") + recovery.what());
        }
    }
}

FrameSubmitCoordinator::RecordedSubmit FrameSubmitCoordinator::recordSubmitCommands(const SubmitParams& params,
                                                                                    const AcquiredSubmit& acquired) {
    auto& trace = rawrcam::diagnostics::RuntimeTraceRecorder::instance();
    const auto& metadata = *params.metadata;
    const auto& colorState = *params.colorState;
    auto* slot = acquired.slot;
    const uint32_t slotIndex = acquired.slotIndex;

    vkCheck(vkResetFences(vulkanContext_.device(), 1, &slot->fence), "vkResetFences");
    vkCheck(vkResetCommandBuffer(slot->command, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkCheck(vkBeginCommandBuffer(slot->command, &bi), "vkBeginCommandBuffer");
    if (!resources_.recorder()) {
        throw std::runtime_error("RawDevelopRecorder is not configured");
    }

    auto frameTonemapParams = tonemapParams_;
    frameTonemapParams.aePostGain = previewPostGain(metadata);
    lifecyclePort_.applyAutoKey(frameTonemapParams, metadata);
    trace.record(rawrcam::diagnostics::RuntimeTraceStage::PostGain, params.timestampNs, metadata.frameOrdinal,
                 static_cast<int32_t>(slotIndex), metadata.exposureTimeNs, metadata.sensitivity, 0,
                 static_cast<int64_t>(std::llround(frameTonemapParams.aePostGain * 1000000.0f)));
    constexpr bool renderedExposureFeedbackEnabled = false;
    trace.record(rawrcam::diagnostics::RuntimeTraceStage::RecordBegin, params.timestampNs, metadata.frameOrdinal,
                 static_cast<int32_t>(slotIndex), metadata.exposureTimeNs, metadata.sensitivity);
    rawrcam::pipeline::RawDevelopRecordInput recordInput{*slot,
                                                         slotIndex,
                                                         acquired.swapIndex.value_or(0u),
                                                         *acquired.raw,
                                                         config_.generation,
                                                         params.timestampNs,
                                                         config_.rawWidth,
                                                         config_.rawHeight,
                                                         config_.previewWidth,
                                                         config_.previewHeight,
                                                         config_.cfa,
                                                         config_.diagnosticMode,
                                                         config_.experimentalZeroCopy,
                                                         config_.lensShadingCorrectionEnabled,
                                                         highlightReconstructionEnabled_,
                                                         vulkanContext_.queueFamily(),
                                                         config_.sensorOrientationDegrees,
                                                         displayRotationDegrees_,
                                                         scopeDeviceRotationDegrees_,
                                                         params.black,
                                                         params.white,
                                                         params.wb,
                                                         params.sensorToSrgb,
                                                         metadata,
                                                         frameTonemapParams,
                                                         renderedExposureFeedbackEnabled,
                                                         false,
                                                         false,
                                                         {},
                                                         2u,
                                                         true,
                                                         {}};
    recordInput.multiframeEnabled = capturePort_.multiframeEnabled();
    recordInput.distortionCorrectionEnabled = distortionCorrectionEnabled_.load(std::memory_order_relaxed);
    recordInput.videoCropOutWidth = videoCropOutWidth_.load(std::memory_order_relaxed);
    recordInput.videoCropOutHeight = videoCropOutHeight_.load(std::memory_order_relaxed);
    recordInput.filmEnabled = filmSimEnabled_;
    recordInput.filmLook = filmLook_;
    recordInput.viewfinderDivisor = viewfinderDivisor_;
    recordInput.presentationEnabled = acquired.swapIndex.has_value();
    recordInput.highlightMethod = highlightMethod_;
    recordInput.highlightThreshold = highlightThreshold_;
    recordInput.highlightCompression = highlightCompression_;
    recordInput.sceneExposureEnabled = diagnosticsPort_.sceneKeyMeteringEnabled();
    if (recordInput.sceneExposureEnabled) {
        recordInput.sceneSpot = diagnosticsPort_.sceneMeteringSpot();
        recordInput.sceneWeighting = diagnosticsPort_.sceneMeteringWeighting();
        recordInput.sceneFocus = diagnosticsPort_.sceneMeteringFocus();
    }
    if (acquired.videoIndex && videoOutput_) {
        const auto cameraToAp1 = RawDevelopRecorder::composeCameraToAp1(params.sensorToSrgb);
        rawrcam::video::VideoDemosaic::Frame videoFrame{};
        const bool directBuffer = acquired.raw->importBufferAvailable;
        videoFrame.rawView = (directBuffer || config_.experimentalZeroCopy) ? acquired.raw->view : slot->rawCopy.view;
        if (directBuffer) {
            videoFrame.rawBuffer = acquired.raw->importBuffer;
            videoFrame.rawStridePixels = acquired.raw->importBufferStridePixels;
        }
        videoFrame.black = params.black;
        videoFrame.wb = params.wb;
        videoFrame.white = params.white;
        videoFrame.cfa = config_.cfa;
        videoFrame.cameraToLinearSrgb = params.sensorToSrgb;
        if (config_.lensShadingCorrectionEnabled && !metadata.lensShadingMap.empty()) {
            videoFrame.lensShading = metadata.lensShadingMap.data();
            videoFrame.lensShadingCount = metadata.lensShadingMap.size();
            videoFrame.lensShadingWidth = metadata.lensShadingMapWidth;
            videoFrame.lensShadingHeight = metadata.lensShadingMapHeight;
        }
        videoFrame.noiseProfileValid =
            rawrcam::develop::rendered::resolveDenoiseNoise(metadata, videoFrame.noiseA, videoFrame.noiseB);
        recordInput.recordPrimaryVideo = [&, videoFrame, cameraToAp1, frameTonemapParams] {
            videoOutput_->setLensDistortion(metadata.cameraContext ? metadata.cameraContext->profileDistortion
                                                                   : std::nullopt);
            videoOutput_->setLensMirror(metadata.cameraContext ? metadata.cameraContext->profileMirror : 0u);
            videoOutput_->setDcpProfile(metadata.cameraContext ? metadata.cameraContext->dcpProfile : nullptr,
                                        params.colorState->calibrationWeight1);
            videoOutput_->record(slot->command, slotIndex, *acquired.videoIndex, config_.rawWidth, config_.rawHeight,
                                 videoFrame, cameraToAp1, frameTonemapParams, performanceTracker_);
        };
    }
    const auto recordResult = resources_.recorder()->record(recordInput, pipelineAuditSubmitCount_);
    if (capturePort_.multiframeEnabled()) {
        if (!slot->rawCopy.image && multiframeStarveLoggedGeneration_ != config_.generation) {
            multiframeStarveLoggedGeneration_ = config_.generation;
            lifecyclePort_.postDiagnostic(
                "MULTIFRAME_RING_STARVED reason=bridge_image_released "
                "remedy=reconfigure_session_with_multiframe_enabled");
        }
        capturePort_.recordMultiframeFrame(slot->command, slot->rawCopy.image, params.timestampNs, metadata,
                                           colorState);
    }
    trace.record(rawrcam::diagnostics::RuntimeTraceStage::RecordEnd, params.timestampNs, metadata.frameOrdinal,
                 static_cast<int32_t>(slotIndex), metadata.exposureTimeNs, metadata.sensitivity);
    vkCheck(vkEndCommandBuffer(slot->command), "vkEndCommandBuffer");
    // The GPU RAW10 unpack (compute) or RAW16 copy (transfer) reads the camera
    // buffer before the recorder's own first RAW access.
    return RecordedSubmit{recordResult.rawWaitStage |
                          (acquired.raw->gpuUnpack ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT
                                                   : 0u)};
}

void FrameSubmitCoordinator::submitAndPresent(const SubmitParams& params, const AcquiredSubmit& acquired,
                                              const RecordedSubmit& recorded, bool& submitted) {
    auto& trace = rawrcam::diagnostics::RuntimeTraceRecorder::instance();
    const auto& metadata = *params.metadata;
    auto* slot = acquired.slot;
    const uint32_t slotIndex = acquired.slotIndex;

    std::array<VkSemaphore, 3> waits{};
    std::array<VkPipelineStageFlags, 3> stages{};
    uint32_t waitCount = 0;
    if (acquired.swapIndex) {
        waits[waitCount] = slot->imageAvailable;
        stages[waitCount++] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    }
    if (params.acquireFenceFd >= 0) {
        int fd = dup(params.acquireFenceFd);
        if (fd < 0) throw std::runtime_error("dup acquire fence failed");
        VkImportSemaphoreFdInfoKHR ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
        ii.semaphore = slot->cameraAcquire;
        ii.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
        ii.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        ii.fd = fd;
        const VkResult ir = vulkanContext_.importSemaphoreFd()(vulkanContext_.device(), &ii);
        if (ir != VK_SUCCESS) {
            close(fd);
            vkCheck(ir, "vkImportSemaphoreFdKHR");
        }
        waits[waitCount] = slot->cameraAcquire;
        stages[waitCount++] = recorded.rawWaitStage;
    }
    if (acquired.videoIndex && videoOutput_) {
        waits[waitCount] = videoOutput_->available(slotIndex);
        stages[waitCount++] = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    }

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = waitCount;
    si.pWaitSemaphores = waits.data();
    si.pWaitDstStageMask = stages.data();
    si.commandBufferCount = 1;
    si.pCommandBuffers = &slot->command;
    std::array<VkSemaphore, 2> signals{};
    uint32_t signalCount = 0;
    if (acquired.swapIndex) signals[signalCount++] = slot->renderFinished;
    if (acquired.videoIndex && videoOutput_) signals[signalCount++] = videoOutput_->rendered(slotIndex);
    si.signalSemaphoreCount = signalCount;
    si.pSignalSemaphores = signals.data();
    trace.record(rawrcam::diagnostics::RuntimeTraceStage::QueueSubmitBegin, params.timestampNs, metadata.frameOrdinal,
                 static_cast<int32_t>(slotIndex), metadata.exposureTimeNs, metadata.sensitivity);
    trace.record(rawrcam::diagnostics::RuntimeTraceStage::QueueMutexWait, params.timestampNs, metadata.frameOrdinal,
                 static_cast<int32_t>(slotIndex), metadata.exposureTimeNs, metadata.sensitivity);
    {
        std::lock_guard<std::mutex> queueLock(queueSubmitMutex_);
        trace.record(rawrcam::diagnostics::RuntimeTraceStage::QueueMutexAcquired, params.timestampNs,
                     metadata.frameOrdinal, static_cast<int32_t>(slotIndex), metadata.exposureTimeNs,
                     metadata.sensitivity);
        vkCheck(vkQueueSubmit(vulkanContext_.queue(), 1, &si, slot->fence), "vkQueueSubmit");
    }
    trace.record(rawrcam::diagnostics::RuntimeTraceStage::QueueMutexReleased, params.timestampNs, metadata.frameOrdinal,
                 static_cast<int32_t>(slotIndex), metadata.exposureTimeNs, metadata.sensitivity);
    trace.record(rawrcam::diagnostics::RuntimeTraceStage::QueueSubmitEnd, params.timestampNs, metadata.frameOrdinal,
                 static_cast<int32_t>(slotIndex), metadata.exposureTimeNs, metadata.sensitivity);
    submitted = true;
    // The GPU now reads the camera buffer: the slot owns it before anything
    // else can throw, so an accepted frame is always retired by its fence.
    slot->submitted = true;
    slot->imageLease = params.lease->takeImage();
    slot->timestampNs = params.timestampNs;
    capturePort_.commitMultiframeFrame(params.timestampNs);

    slot->metadataSnapshot = metadata;
    slot->colorStateSnapshot = *params.colorState;
    performanceTracker_.recordSubmitted();

    if (acquired.videoIndex && videoOutput_) {
        VkResult videoPresent = VK_SUCCESS;
        {
            std::lock_guard<std::mutex> queueLock(queueSubmitMutex_);
            videoPresent =
                videoOutput_->present(vulkanContext_.queue(), slotIndex, *acquired.videoIndex,
                                      static_cast<uint64_t>(encoderTimestampNs(metadata, params.timestampNs)));
        }
        if (videoPresent == VK_SUCCESS || videoPresent == VK_SUBOPTIMAL_KHR)
            ++videoSubmitted_;
        else {
            ++videoDrops_;
            ++videoDropReasons_.presentFail;
            lifecyclePort_.postDiagnostic("VIDEO_PRESENT_FAIL result=" + std::to_string(videoPresent));
        }
    }
    if (acquired.swapIndex) {
        trace.record(rawrcam::diagnostics::RuntimeTraceStage::PresentBegin, params.timestampNs, metadata.frameOrdinal,
                     static_cast<int32_t>(slotIndex), metadata.exposureTimeNs, metadata.sensitivity);
        VkResult presentResult;
        {
            std::lock_guard<std::mutex> queueLock(queueSubmitMutex_);
            presentResult =
                swapchainRenderer_.present(vulkanContext_.queue(), slot->renderFinished, *acquired.swapIndex);
        }
        trace.record(rawrcam::diagnostics::RuntimeTraceStage::PresentEnd, params.timestampNs, metadata.frameOrdinal,
                     static_cast<int32_t>(slotIndex), metadata.exposureTimeNs, metadata.sensitivity, 0u, presentResult);
        if ((presentResult == VK_SUCCESS || presentResult == VK_SUBOPTIMAL_KHR) &&
            !metadata.hidePreview && !metadata.optimizedStillRequestId.value_or(0) && metadata.cameraContext) {
            swapchainRenderer_.noteLivePresentation(metadata.cameraContext->cameraContextGeneration, params.timestampNs);
        }
        if (presentResult == VK_ERROR_OUT_OF_DATE_KHR) {
            lifecyclePort_.notifySwapchainOutOfDate();
            if (acquired.videoIndex) ++previewSkippedDuringVideo_;
        } else if (presentResult != VK_SUCCESS && presentResult != VK_SUBOPTIMAL_KHR) {
            vkCheck(presentResult, "vkQueuePresentKHR");
        }
    }
}

void FrameSubmitCoordinator::recoverFailedSubmit(uint32_t slotIndex, bool slotSelected, bool queueSubmitted,
                                                 bool swapchainImageAcquired, const char* error) {
    if (slotSelected && !queueSubmitted) {
        diagnosticsPort_.discardScopesSlot(slotIndex);
    }
    // Repeated identical failures are rate-limited: vendor builds mute an app's
    // logcat after heavy logging, which would hide the first (useful) lines.
    // The key drops the per-buffer facts suffix (" | ...") so rotating AHBs count as one failure.
    std::string failureKey(error);
    if (const auto facts = failureKey.find(" | "); facts != std::string::npos) failureKey.resize(facts);
    if (lastSubmitFailure_ == failureKey) {
        ++repeatedSubmitFailures_;
    } else {
        lastSubmitFailure_ = std::move(failureKey);
        repeatedSubmitFailures_ = 0;
    }
    if (repeatedSubmitFailures_ < 3 || repeatedSubmitFailures_ % 300 == 0)
        LOGE("submit failed (repeat %llu): %s", static_cast<unsigned long long>(repeatedSubmitFailures_), error);
    if (videoActive()) lastVideoSubmitFailure_ = error;
    lifecyclePort_.postDiagnostic(std::string("SUBMIT_FAILURE ") + error);
    if (queueSubmitted) return;

    // A successful acquire signals imageAvailable and transfers one
    // swapchain image to the application. If command recording/import fails
    // before submit, neither can be reused as though acquire never happened.
    // Recreate the swapchain to release the acquired image, then replace the
    // slot's binary synchronization objects so the signaled semaphore and
    // reset-but-unsignaled fence cannot leak into a later frame.
    if (swapchainImageAcquired) {
        try {
            lifecyclePort_.notifySwapchainOutOfDate();
            frameSlots_.recreateSyncObjects(slotIndex);
            lifecyclePort_.postDiagnostic("SWAPCHAIN_PRE_SUBMIT_RECOVERY_PASS slot=" + std::to_string(slotIndex));
        } catch (const std::exception& recoveryError) {
            lifecyclePort_.postDiagnostic(std::string("SWAPCHAIN_PRE_SUBMIT_RECOVERY_FAIL slot=") +
                                          std::to_string(slotIndex) + " error=" + recoveryError.what());
        }
    }
    if (videoOutput_ && videoOutput_->ready()) {
        // A video acquire can have signaled a binary semaphore even when
        // recording fails before queue submission. Retire that swapchain.
        videoOutput_->stop();
        ++videoDropReasons_.submitFail;
        lifecyclePort_.postDiagnostic("VIDEO_STOPPED_AFTER_PRE_SUBMIT_FAILURE");
    }

    performanceTracker_.recordDropped();
}

float FrameSubmitCoordinator::previewPostGain(const rawrcam::metadata::FrameMetadataSnapshot& metadata) {
    const float gain = lifecyclePort_.postGainFor(metadata);
    const double exposure = double(metadata.exposureTimeNs) * double(metadata.sensitivity);
    if (!(exposure > 0.0)) return gain;
    if (!metadata.optimizedStillRequestId || *metadata.optimizedStillRequestId == 0) {
        lastRepeatingExposure_ = exposure;
        lastRepeatingGain_ = gain;
        return gain;
    }
    if (lastRepeatingExposure_ <= 0.0) return gain;
    // Interior-ISO calibration probes and HDR+ long frames can also be
    // brighter than preview. Match the repeating frames' rendered brightness
    // (their gain includes any HDR+ headroom lift) without changing the
    // repeating seed. Same ceiling the tonemap engine enforces.
    return std::min(tonemap::kMaxAePostGain, lastRepeatingGain_ * float(lastRepeatingExposure_ / exposure));
}

bool FrameSubmitCoordinator::submitAhb(uint64_t timestampNs, rawrcam::imaging::RawFrameLease& lease,
                                       const std::array<float, 4>& black, float white,
                                       const std::array<float, 4>& wb, const std::array<float, 9>& sensorToSrgb,
                                       const rawrcam::metadata::FrameMetadataSnapshot& metadata,
                                       const rawrcam::color::FrameColorTransform& colorState) {
    AImage* const imageLease = lease.image();
    AHardwareBuffer* const ahb = lease.ahb();
    const int acquireFenceFd = lease.fence();
    const bool videoReady = videoOutput_ && videoOutput_->ready() && config_.diagnosticMode == 0u;
    if (!configured_ || ((!swapchainRenderer_.ready() || presentationPaused_) && !videoReady)) return false;

    // The CPU read below waits on the camera fence and touches the whole RAW
    // buffer; during recording that stall lands in the encoder's frame cadence.
    if (!videoReady && imageLease && metadata.cameraContext &&
        (lastRawContentStatsNs_ == 0 || timestampNs < lastRawContentStatsNs_ ||
         timestampNs - lastRawContentStatsNs_ >= 1'000'000'000ull)) {
        lastRawContentStatsNs_ = timestampNs;
        rawrcam::imaging::RawContentStatsInput statsInput;
        statsInput.cfa = static_cast<int>(metadata.cameraContext->rawPreviewCfa);
        statsInput.blackLevel = *std::min_element(black.begin(), black.end());
        statsInput.whiteLevel = white;
        statsInput.quadStep = 16;
        const std::string stats = "PREVIEW " +
                                  rawrcam::imaging::sampleRawContentStats(imageLease, ahb, acquireFenceFd, statsInput) +
                                  " frame=" + std::to_string(metadata.frameOrdinal) +
                                  " exposureTimeNs=" + std::to_string(metadata.exposureTimeNs) +
                                  " sensitivity=" + std::to_string(metadata.sensitivity);
        LOGI("%s", stats.c_str());
        lifecyclePort_.postDiagnostic(stats);
    }

    auto& trace = rawrcam::diagnostics::RuntimeTraceRecorder::instance();
    trace.record(rawrcam::diagnostics::RuntimeTraceStage::SubmitBegin, timestampNs, metadata.frameOrdinal, -1,
                 metadata.exposureTimeNs, metadata.sensitivity);
    const SubmitParams params{timestampNs, ahb, acquireFenceFd, &lease,    black,
                              white,       wb,  sensorToSrgb,   &metadata, &colorState};
    uint32_t slotIndex = 0;
    bool slotSelected = false;
    bool swapchainImageAcquired = false;
    bool queueSubmitted = false;
    try {
        releaseCompletedSlots(false);
        auto* slot = frameSlots_.findAvailable(&slotIndex);
        if (!slot) {
            trace.record(rawrcam::diagnostics::RuntimeTraceStage::SlotUnavailable, timestampNs, metadata.frameOrdinal);
            performanceTracker_.recordDropped();
            if (videoReady) {
                ++videoDrops_;
                ++videoDropReasons_.noSlot;
            }
            return false;
        }
        slotSelected = true;
        auto acquired = acquireSubmitSlot(params, slot, slotIndex);
        if (!acquired) return false;
        swapchainImageAcquired = acquired->swapIndex.has_value();
        if (acquired->videoIndex && (acquired->raw->importBufferAvailable || slot->rawCopy.image != VK_NULL_HANDLE ||
                                     acquired->raw->view != VK_NULL_HANDLE)) {
            submitSplitVideoAndMonitor(params, *acquired, queueSubmitted);
            return true;
        }
        const auto recorded = recordSubmitCommands(params, *acquired);
        submitAndPresent(params, *acquired, recorded, queueSubmitted);
        return true;
    } catch (const std::exception& e) {
        if (!queueSubmitted) capturePort_.discardMultiframeFrame(timestampNs);
        recoverFailedSubmit(slotIndex, slotSelected, queueSubmitted, swapchainImageAcquired, e.what());
        return queueSubmitted;
    }
}

bool FrameSubmitCoordinator::pollSlotFence(pipeline::FrameSlot& slot, bool force) {
    if (!slot.submitted) return false;
    const VkResult result = force ? vkWaitForFences(vulkanContext_.device(), 1, &slot.fence, VK_TRUE, UINT64_MAX)
                                  : vkGetFenceStatus(vulkanContext_.device(), slot.fence);
    if (result == VK_SUCCESS) {
        return true;
    }
    if (result != VK_NOT_READY) vkCheck(result, "slot fence status");
    return false;
}

void FrameSubmitCoordinator::reportRenderedFeedback(uint32_t slotIndex, const pipeline::FrameSlot& slot) {
    auto& trace = rawrcam::diagnostics::RuntimeTraceRecorder::instance();
    const auto* traceMeta = slot.metadataSnapshot ? &*slot.metadataSnapshot : nullptr;
    const auto rendered = diagnosticsPort_.consumeExposureFeedback(slotIndex);
    if (!rendered) return;

    trace.record(rawrcam::diagnostics::RuntimeTraceStage::RenderP50, slot.timestampNs,
                 traceMeta ? traceMeta->frameOrdinal : 0, static_cast<int32_t>(slotIndex), 0, 0, 0,
                 static_cast<int64_t>(std::llround(rendered->lumaP50 * 1000000.0f)));
    trace.record(rawrcam::diagnostics::RuntimeTraceStage::RenderP95, slot.timestampNs,
                 traceMeta ? traceMeta->frameOrdinal : 0, static_cast<int32_t>(slotIndex), 0, 0, 0,
                 static_cast<int64_t>(std::llround(rendered->lumaP95 * 1000000.0f)));
    trace.record(rawrcam::diagnostics::RuntimeTraceStage::RenderP99, slot.timestampNs,
                 traceMeta ? traceMeta->frameOrdinal : 0, static_cast<int32_t>(slotIndex), 0, 0, 0,
                 static_cast<int64_t>(std::llround(rendered->lumaP99 * 1000000.0f)));
    trace.record(rawrcam::diagnostics::RuntimeTraceStage::RenderBright90, slot.timestampNs,
                 traceMeta ? traceMeta->frameOrdinal : 0, static_cast<int32_t>(slotIndex), 0, 0, 0,
                 static_cast<int64_t>(std::llround(rendered->brightFraction90 * 1000000.0f)));
    trace.record(rawrcam::diagnostics::RuntimeTraceStage::RenderBright96, slot.timestampNs,
                 traceMeta ? traceMeta->frameOrdinal : 0, static_cast<int32_t>(slotIndex), 0, 0, 0,
                 static_cast<int64_t>(std::llround(rendered->brightFraction96 * 1000000.0f)));
    trace.record(rawrcam::diagnostics::RuntimeTraceStage::RenderClip, slot.timestampNs,
                 traceMeta ? traceMeta->frameOrdinal : 0, static_cast<int32_t>(slotIndex), 0, 0, 0,
                 static_cast<int64_t>(std::llround(rendered->anyChannelClippedFraction * 1000000.0f)));
}

void FrameSubmitCoordinator::reportSceneKey(uint32_t slotIndex, const pipeline::FrameSlot& slot) {
    if (!slot.metadataSnapshot) return;
    const auto& metadata = *slot.metadataSnapshot;
    const auto key = diagnosticsPort_.updateSceneKey(slotIndex, metadata, slot.timestampNs, videoActive());
    if (!key) return;
    auto& trace = rawrcam::diagnostics::RuntimeTraceRecorder::instance();
    trace.record(rawrcam::diagnostics::RuntimeTraceStage::SceneKeyEv, slot.timestampNs, metadata.frameOrdinal,
                 static_cast<int32_t>(slotIndex), metadata.exposureTimeNs, metadata.sensitivity, 0,
                 static_cast<int64_t>(std::llround(key->keyEv * 1000000.0f)));
    trace.record(rawrcam::diagnostics::RuntimeTraceStage::AutoKeyEv, slot.timestampNs, metadata.frameOrdinal,
                 static_cast<int32_t>(slotIndex), metadata.exposureTimeNs, metadata.sensitivity, 0,
                 static_cast<int64_t>(std::llround(key->autoKeyEv * 1000000.0f)));
}

void FrameSubmitCoordinator::reportAuditFrame(const pipeline::FrameSlot& slot) {
    if (!slot.metadataSnapshot || !slot.colorStateSnapshot) {
        return;
    }
    diagnosticsPort_.recordAuditFrame(*slot.metadataSnapshot, *slot.colorStateSnapshot);
}

void FrameSubmitCoordinator::retireSlot(uint32_t slotIndex, pipeline::FrameSlot& slot) {
    auto& trace = rawrcam::diagnostics::RuntimeTraceRecorder::instance();
    const auto frameOrdinal = slot.metadataSnapshot ? slot.metadataSnapshot->frameOrdinal : 0;
    const auto exposureTimeNs = slot.metadataSnapshot ? slot.metadataSnapshot->exposureTimeNs : 0;
    const auto sensitivity = slot.metadataSnapshot ? slot.metadataSnapshot->sensitivity : 0;
    trace.record(rawrcam::diagnostics::RuntimeTraceStage::FenceComplete, slot.timestampNs, frameOrdinal,
                 static_cast<int32_t>(slotIndex), exposureTimeNs, sensitivity);
    performanceTracker_.consumeCompleted(slotIndex, resources_.importer() ? resources_.importer()->size() : 0,
                                         resources_.preview() ? resources_.preview()->cachedDescriptorSetCount() : 0,
                                         diagnosticsPort_.overlayNeedsRawState());
    diagnosticsPort_.integrityComplete(slotIndex);
    capturePort_.retireMultiframeFrame(slot.timestampNs);
    trace.record(rawrcam::diagnostics::RuntimeTraceStage::ScopesBegin, slot.timestampNs, frameOrdinal,
                 static_cast<int32_t>(slotIndex));
    diagnosticsPort_.retireScopesSlot(slotIndex);
    trace.record(rawrcam::diagnostics::RuntimeTraceStage::ScopesEnd, slot.timestampNs, frameOrdinal,
                 static_cast<int32_t>(slotIndex));
    reportRenderedFeedback(slotIndex, slot);
    reportSceneKey(slotIndex, slot);
    reportAuditFrame(slot);

    if (slot.imageLease) {
        AHardwareBuffer* completedAhb = nullptr;
        if (AImage_getHardwareBuffer(slot.imageLease.image(), &completedAhb) == AMEDIA_OK && completedAhb != nullptr &&
            capturePort_.beginDeferredSnapshot(slot.imageLease.image(), completedAhb, slot.timestampNs)) {
            (void)slot.imageLease.release();  // The capture worker now owns the image.
        }
        // A frame recorded to video also goes to a RAW video recording, if one runs;
        // it takes the image from the lease when it accepts the frame.
        if (slot.imageLease && slot.videoRecorded && slot.metadataSnapshot)
            (void)capturePort_.offerRawVideoFrame(slot.imageLease, *slot.metadataSnapshot, config_.rawWidth,
                                                  config_.rawHeight);
        trace.record(rawrcam::diagnostics::RuntimeTraceStage::ImageRelease, slot.timestampNs, frameOrdinal,
                     static_cast<int32_t>(slotIndex));
        slot.imageLease.reset();
    }
    slot.metadataSnapshot.reset();
    slot.colorStateSnapshot.reset();
    slot.submitted = false;
    slot.videoRecorded = false;
    trace.record(rawrcam::diagnostics::RuntimeTraceStage::FrameRetired, slot.timestampNs, frameOrdinal,
                 static_cast<int32_t>(slotIndex));
}

void FrameSubmitCoordinator::releaseCompletedSlots(bool force) {
    for (uint32_t i = 0; i < rawrcam::imaging::kRealtimeFramesInFlight; ++i) {
        auto& slot = frameSlots_[i];
        if (pollSlotFence(slot, force)) {
            retireSlot(i, slot);
        }
    }
}

VkImageUsageFlags FrameSubmitCoordinator::importedRawUsage() const noexcept {
    if (config_.diagnosticMode == 8u) return VK_IMAGE_USAGE_SAMPLED_BIT;
    if (config_.diagnosticMode == 0u) {
        if (!config_.experimentalZeroCopy) return VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        // The multiframe ring is fed from the owned bridge copy via
        // vkCmdCopyImage, which requires TRANSFER_SRC on the import. Preview
        // itself keeps reading the import buffer (zero-copy). Request the
        // union regardless of the multiframe toggle: reader usage is fixed
        // at configure time and the toggle must not force a restart.
        return VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    if (config_.diagnosticMode == 9u) return VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (config_.diagnosticMode == 11u || config_.diagnosticMode == 12u)
        return VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (config_.diagnosticMode == 13u || config_.diagnosticMode == 14u)
        return VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (config_.diagnosticMode == 16u) return VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (config_.diagnosticMode == 15u)
        return VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    return VK_IMAGE_USAGE_STORAGE_BIT;
}

}  // namespace rawrcam::pipeline
