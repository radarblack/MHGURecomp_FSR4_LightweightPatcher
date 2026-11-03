/*
 * Copyright (c) 2026 Q2RTX FSR Vulkan contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef FFX_VK_PORTABLE_H
#define FFX_VK_PORTABLE_H

#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#if defined(__cplusplus)
extern "C" {
#endif

#define FFX_VK_PORTABLE_ABI_VERSION 2u

typedef enum FfxVkPortableResult {
    FFX_VK_PORTABLE_OK = 0,
    FFX_VK_PORTABLE_ERROR_INVALID_POINTER = -1,
    FFX_VK_PORTABLE_ERROR_INVALID_STRUCT_SIZE = -2,
    FFX_VK_PORTABLE_ERROR_VULKAN = -3,
    FFX_VK_PORTABLE_ERROR_INVALID_ARGUMENT = -4,
    FFX_VK_PORTABLE_ERROR_OUT_OF_MEMORY = -5,
    FFX_VK_PORTABLE_ERROR_UNSUPPORTED = -6,
    FFX_VK_PORTABLE_ERROR_BACKEND = -7
} FfxVkPortableResult;

typedef struct FfxVkPortableUpscaleContext FfxVkPortableUpscaleContext;

typedef enum FfxVkPortableResourceState {
    FFX_VK_PORTABLE_RESOURCE_STATE_UNDEFINED = 0,
    FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ,
    FFX_VK_PORTABLE_RESOURCE_STATE_COMPUTE_READ,
    FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS,
    FFX_VK_PORTABLE_RESOURCE_STATE_TRANSFER_SOURCE,
    FFX_VK_PORTABLE_RESOURCE_STATE_TRANSFER_DESTINATION,
    FFX_VK_PORTABLE_RESOURCE_STATE_COLOR_ATTACHMENT,
    FFX_VK_PORTABLE_RESOURCE_STATE_DEPTH_ATTACHMENT,
    FFX_VK_PORTABLE_RESOURCE_STATE_PRESENT
} FfxVkPortableResourceState;

typedef enum FfxVkPortableContextFlagBits {
    FFX_VK_PORTABLE_CONTEXT_HDR_COLOR_INPUT = 1u << 0,
    FFX_VK_PORTABLE_CONTEXT_DISPLAY_RESOLUTION_MOTION_VECTORS = 1u << 1,
    FFX_VK_PORTABLE_CONTEXT_JITTERED_MOTION_VECTORS = 1u << 2,
    FFX_VK_PORTABLE_CONTEXT_DEPTH_INVERTED = 1u << 3,
    FFX_VK_PORTABLE_CONTEXT_DEPTH_INFINITE = 1u << 4,
    FFX_VK_PORTABLE_CONTEXT_AUTO_EXPOSURE = 1u << 5,
    FFX_VK_PORTABLE_CONTEXT_DYNAMIC_RESOLUTION = 1u << 6,
    FFX_VK_PORTABLE_CONTEXT_DEBUG_CHECKING = 1u << 7
} FfxVkPortableContextFlagBits;

typedef struct FfxVkPortableExtent2D {
    uint32_t width;
    uint32_t height;
} FfxVkPortableExtent2D;

typedef struct FfxVkPortableFloat2 {
    float x;
    float y;
} FfxVkPortableFloat2;

typedef struct FfxVkPortableImage {
    uint32_t structSize;
    VkImage image;
    VkFormat format;
    FfxVkPortableExtent2D extent;
    uint32_t mipCount;
    uint32_t arrayLayers;
    VkImageUsageFlags usage;
    VkImageAspectFlags aspect;
    FfxVkPortableResourceState state;
} FfxVkPortableImage;

typedef struct FfxVkPortableDeviceInfo {
    uint32_t structSize;
    VkInstance instance;
    VkPhysicalDevice physicalDevice;
    VkDevice device;
    PFN_vkGetDeviceProcAddr getDeviceProcAddr;
    const VkAllocationCallbacks* allocationCallbacks;
    VkQueue queue;
    uint32_t queueFamilyIndex;
    VkBool32 deviceCoherentMemoryEnabled;
    VkBool32 shaderFloat16Enabled;
    VkBool32 subgroupSizeControlEnabled;
    VkBool32 computeFullSubgroupsEnabled;
    VkBool32 synchronization2Enabled;
    VkBool32 bufferMarkerEnabled;
    VkBool32 debugUtilsEnabled;
    VkBool32 shaderStorageBufferArrayNonUniformIndexingEnabled;
    VkBool32 accelerationStructureEnabled;
    VkBool32 shaderStorageImageWriteWithoutFormatEnabled;
} FfxVkPortableDeviceInfo;

typedef struct FfxVkPortableUpscaleCreateInfo {
    uint32_t structSize;
    uint32_t flags;
    FfxVkPortableExtent2D maxRenderSize;
    FfxVkPortableExtent2D maxOutputSize;
} FfxVkPortableUpscaleCreateInfo;

typedef struct FfxVkPortableUpscaleDispatchInfo {
    uint32_t structSize;
    VkCommandBuffer commandBuffer;
    FfxVkPortableImage color;
    FfxVkPortableImage depth;
    FfxVkPortableImage motionVectors;
    FfxVkPortableImage exposure;
    FfxVkPortableImage reactiveMask;
    FfxVkPortableImage transparencyAndCompositionMask;
    FfxVkPortableImage output;
    FfxVkPortableFloat2 jitterOffset;
    FfxVkPortableFloat2 motionVectorScale;
    FfxVkPortableExtent2D renderSize;
    FfxVkPortableExtent2D outputSize;
    float frameTimeMilliseconds;
    float preExposure;
    float cameraNear;
    float cameraFar;
    float cameraVerticalFovRadians;
    float viewSpaceToMeters;
    float sharpness;
    VkBool32 enableSharpening;
    VkBool32 reset;
    uint64_t frameId;
} FfxVkPortableUpscaleDispatchInfo;

#if defined(__cplusplus)
}
#endif

#endif
