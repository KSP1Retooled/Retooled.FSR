#pragma once

#include <cstdint>
#include <string>
#include <vector>

// FSR upscaling through AMD's FSR API on Direct3D 12 or Vulkan, without anything from Unity. Plugin.cpp calls this from
// Unity's render thread, and the test harness calls it on a device that it creates itself.
//
// Every function must be called from the thread that records the device's command lists. A process uses one of the
// two APIs: the first upscaler created decides which of AMD's DLLs are loaded.
namespace fsr
{
    // Results are FfxApiReturnCodes values, plus the codes below for steps that never reached the FSR API.
    constexpr int32_t Ok = 0;
    constexpr int32_t NotRun = INT32_MAX;
    constexpr int32_t ErrorLibraryMissing = 1000;
    constexpr int32_t ErrorUnsupportedGraphicsApi = 1001;
    // The Direct3D 12 device that runs FSR next to a Direct3D 11 device could not be set up. See Sidecar.h.
    constexpr int32_t ErrorSidecarFailed = 1002;
    // Unity could not hand over one of a frame's textures on Vulkan.
    constexpr int32_t ErrorTextureAccess = 1003;

    // The type is an FfxApiMsgType: 0 for an error, 1 for a warning.
    using LogFunction = void(int32_t type, const char* message);

    // Shared with C# as FsrNative.Options, field for field.
    struct Options
    {
        // The largest size that the inputs of a frame may have.
        uint32_t renderWidth;
        uint32_t renderHeight;
        uint32_t outputWidth;
        uint32_t outputHeight;
        // FfxApiCreateContextUpscaleFlags.
        uint32_t flags;
    };

    // Shared with C# as FsrNative.Frame, field for field.
    //
    // Textures are ID3D12Resource pointers. When the frame is evaluated, the inputs must be in the state
    // D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE and the output in D3D12_RESOURCE_STATE_UNORDERED_ACCESS. FSR
    // returns them in the same states. On Vulkan the textures are passed as VulkanImages and these four are unused.
    struct Frame
    {
        void* color;
        void* depth;
        void* motion;
        void* output;
        uint32_t renderWidth;
        uint32_t renderHeight;
        uint32_t outputWidth;
        uint32_t outputHeight;
        // The distance that the projection jitter moved the image, in render pixels. Positive values move it towards
        // later columns and later rows of the texture.
        float jitterX;
        float jitterY;
        // Multiplying a motion vector by this scale gives the offset from a pixel to its previous position, as a
        // fraction of the image, with the origin at the top left.
        float motionScaleX;
        float motionScaleY;
        // The camera's planes in view space units and its vertical field of view in radians. A camera with an
        // infinite far plane passes FLT_MAX as the far plane.
        float cameraNear;
        float cameraFar;
        float cameraFovY;
        // The time since the previous frame, in milliseconds.
        float frameTime;
        // From zero to one. Zero skips FSR's sharpening pass.
        float sharpness;
        // FfxApiDispatchFsrUpscaleFlags.
        uint32_t flags;
        // Nonzero if the previous frame has no connection to this one, so that FSR drops its history.
        uint32_t reset;
    };

    // Shared with C# as part of FsrNative.Status.
    struct Info
    {
        // The video memory that the upscaler uses, in bytes, or zero if it is unknown.
        uint64_t videoMemory;
        // The FSR version that the API chose for the device, such as "4.1.2". A trailing " *" marks a version that the
        // graphics driver provided in place of the one in the upscaler DLL.
        char version[32];
    };

    // The Vulkan device that upscalers are created on: a VkDevice, its VkPhysicalDevice and the
    // PFN_vkGetDeviceProcAddr that the device's functions are looked up with.
    struct VulkanDevice
    {
        void* device;
        void* physicalDevice;
        void* getDeviceProcAddr;
    };

    // One of a frame's textures on Vulkan: a VkImage with the VkFormat, VkImageUsageFlags and size it was created
    // with. When the frame is evaluated, the inputs must be in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL and the output
    // in VK_IMAGE_LAYOUT_GENERAL. FSR returns them in the same layouts.
    struct VulkanImage
    {
        void* image;
        uint32_t format;
        uint32_t usage;
        uint32_t width;
        uint32_t height;
        uint32_t mipLevels;
        uint32_t layers;
    };

    // Messages from the FSR API go to this function, which may be called from any thread. Null drops them.
    void SetLog(LogFunction* log);

    // Sends an error of this module's own to the log function.
    void LogError(const char* message);

    // Creates an upscaler on an ID3D12Device. The first call loads amd_fidelityfx_loader_dx12.dll from the folder of
    // this module. Creation compiles FSR's shaders, so it can take a moment.
    int32_t Create(void* device, const Options& options, void** upscaler);

    // The same on a Vulkan device, with amd_fidelityfx_loader_vk.dll.
    int32_t CreateVulkan(const VulkanDevice& device, const Options& options, void** upscaler);

    Info Describe(void* upscaler);

    // Every FSR version that the API offers for an ID3D12Device, in the API's order, or none if the API didn't load.
    // Call it after Create, which loads the API, whether or not Create succeeded.
    std::vector<std::string> ListVersions(void* device);

    std::vector<std::string> ListVersionsVulkan(const VulkanDevice& device);

    // Records the FSR passes into an ID3D12GraphicsCommandList. FSR binds its own descriptor heaps, root signature and
    // pipeline, and the caller restores the command list's state afterwards.
    int32_t Evaluate(void* upscaler, const Frame& frame, void* commandList);

    // Records the FSR passes into a VkCommandBuffer that is outside a render pass. FSR binds its own pipelines and
    // descriptor sets, and the caller restores the command buffer's state afterwards.
    int32_t EvaluateVulkan(void* upscaler, const Frame& frame, const VulkanImage& color, const VulkanImage& depth,
        const VulkanImage& motion, const VulkanImage& output, void* commandBuffer);

    // Releases the upscaler and its textures at once. The GPU must have finished every command list that Evaluate
    // recorded into.
    int32_t Destroy(void* upscaler);
}
