#include <vulkan/vulkan.h>

#include "FsrLibrary.h"
#include "api/include/vk/ffx_api_vk.h"

namespace
{
    FfxApiResource ToResource(const fsr::VulkanImage& image, uint32_t resourceState)
    {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = static_cast<VkFormat>(image.format);
        info.extent = {image.width, image.height, 1};
        info.mipLevels = image.mipLevels;
        info.arrayLayers = image.layers;
        info.usage = image.usage;
        auto handle = static_cast<VkImage>(image.image);
        return ffxApiGetResourceVK(handle, ffxApiGetImageResourceDescriptionVK(handle, info, 0), resourceState);
    }

    VkDeviceContext ToDeviceContext(const fsr::VulkanDevice& device)
    {
        VkDeviceContext context{};
        context.vkDevice = static_cast<VkDevice>(device.device);
        context.vkPhysicalDevice = static_cast<VkPhysicalDevice>(device.physicalDevice);
        context.vkDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(device.getDeviceProcAddr);
        return context;
    }
}

namespace fsr
{
    int32_t CreateVulkan(const VulkanDevice& device, const Options& options, void** upscaler)
    {
        *upscaler = nullptr;
        if (LoadFsrLibrary(L"vk") == nullptr)
            return ErrorLibraryMissing;

        ffxCreateBackendVKDesc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        backend.deviceContext = ToDeviceContext(device);
        return CreateOn(backend.header, options, upscaler);
    }

    // The version query takes the device as the backend does, which on Vulkan is a pointer to a VkDeviceContext.
    std::vector<std::string> ListVersionsVulkan(const VulkanDevice& device)
    {
        VkDeviceContext context = ToDeviceContext(device);
        return ListVersions(&context);
    }

    int32_t EvaluateVulkan(void* upscaler, const Frame& frame, const VulkanImage& color, const VulkanImage& depth,
        const VulkanImage& motion, const VulkanImage& output, void* commandBuffer)
    {
        return Dispatch(upscaler, frame, commandBuffer,
            ToResource(color, FFX_API_RESOURCE_STATE_COMPUTE_READ),
            ToResource(depth, FFX_API_RESOURCE_STATE_COMPUTE_READ),
            ToResource(motion, FFX_API_RESOURCE_STATE_COMPUTE_READ),
            ToResource(output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS));
    }
}
