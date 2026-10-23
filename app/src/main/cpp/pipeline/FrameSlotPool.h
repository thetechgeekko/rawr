#pragma once
#include <media/NdkImage.h>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <optional>

#include "color/FrameColorTransform.h"
#include "develop/render/DcpRenderStage.h"
#include "imaging/FrameLimits.h"
#include "imaging/RawFrameLease.h"
#include "metadata/FrameMetadataSnapshot.h"
#include "vulkan/CommandResources.h"
#include "vulkan/ImageResources.h"

namespace rawrcam::pipeline {

struct FrameSlot {
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    std::unique_ptr<develop::rendered::DcpRenderStage> dcpStage;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkCommandBuffer videoCommand = VK_NULL_HANDLE;
    // Recording splits each frame into three submissions (demosaic, post,
    // tonemap); these hold the second and third parts.
    std::array<VkCommandBuffer, 2> videoStageCommands{};
    VkFence fence = VK_NULL_HANDLE;
    VkFence previewFence = VK_NULL_HANDLE;
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkSemaphore renderFinished = VK_NULL_HANDLE;
    VkSemaphore cameraAcquire = VK_NULL_HANDLE;
    rawrcam::vulkan::OwnedImage rawCopy;
    rawrcam::vulkan::OwnedImage linear;
    rawrcam::vulkan::OwnedImage tonemapped;
    // Film-simulation quarter-res pair (W/4 x H/4). Allocated alongside the
    // rest; the film stage is the only consumer.
    rawrcam::vulkan::OwnedImage lookScaledLinear;
    rawrcam::vulkan::OwnedImage lookScaledOut;
    // Camera image held until the slot fence retires; the fence is not kept.
    rawrcam::imaging::RawFrameLease imageLease{rawrcam::imaging::AcquiredRawFrame{}};
    uint64_t timestampNs = 0;
    std::optional<rawrcam::metadata::FrameMetadataSnapshot> metadataSnapshot;
    std::optional<rawrcam::color::FrameColorTransform> colorStateSnapshot;
    bool submitted = false;
    bool previewSubmitted = false;
    // The frame went to the video encoder; on retirement it is offered as a RAW video frame.
    bool videoRecorded = false;
};

class FrameSlotPool {
   public:
    void initialize(VkPhysicalDevice physical, VkDevice device, uint32_t queueFamily);
    // When retainBridgeCopy is false the owned R16 bridge image is left
    // unallocated (callers read the import buffer/image directly instead).
    // All bridge uses must then be skipped or guarded by the caller; see
    // RawDevelopRecorder.
    void createImages(uint32_t rawWidth, uint32_t rawHeight, uint32_t previewWidth, uint32_t previewHeight,
                      bool retainBridgeCopy = true);
    void transitionImagesToGeneral(VkQueue queue);
    // Allocates the bridge image for any slot that lacks one (multiframe
    // enabled after configure) and moves only those to GENERAL. Caller must
    // have the queue idle and hold the submit lock. Returns true if any were
    // allocated.
    bool ensureBridgeImages(uint32_t rawWidth, uint32_t rawHeight, VkQueue queue);
    void destroyImages();
    void destroy();

    FrameSlot* findAvailable(uint32_t* slotIndex);
    // Exceptional pre-submit recovery only. Replaces binary semaphores and the
    // fence atomically after an acquired swapchain image is abandoned.
    void recreateSyncObjects(uint32_t slotIndex);
    // Used when video already submitted but optional preview failed before
    // consuming its acquired swapchain semaphore. Caller first waits idle.
    void recreatePreviewSyncObjects(uint32_t slotIndex);
    FrameSlot& operator[](uint32_t index) { return slots_[index]; }
    const FrameSlot& operator[](uint32_t index) const { return slots_[index]; }
    auto begin() { return slots_.begin(); }
    auto end() { return slots_.end(); }
    auto begin() const { return slots_.begin(); }
    auto end() const { return slots_.end(); }
    VkCommandPool commandPool() const noexcept { return commands_.pool(); }

   private:
    rawrcam::vulkan::OwnedImage createBridgeImage(uint32_t rawWidth, uint32_t rawHeight) const;
    void transitionToGeneral(VkQueue queue, const VkImage* images, size_t imageCount);

    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    vulkan::CommandResources commands_;
    std::array<FrameSlot, rawrcam::imaging::kRealtimeFramesInFlight> slots_{};
};

}  // namespace rawrcam::pipeline
