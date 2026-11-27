#pragma once

#include "Fsr.h"
#include "api/include/ffx_api.h"
#include "upscalers/include/ffx_upscale.h"

// What Fsr.cpp and FsrVulkan.cpp share. AMD's dx12 and vk headers declare the same names differently, so each graphics
// API has a source file of its own, and this header includes neither.
namespace fsr
{
    struct Library
    {
        PfnFfxCreateContext CreateContext = nullptr;
        PfnFfxDestroyContext DestroyContext = nullptr;
        PfnFfxQuery Query = nullptr;
        PfnFfxDispatch Dispatch = nullptr;
        bool loaded = false;
    };

    // Loads AMD's loader and upscaler DLLs for an API, "dx12" or "vk", from the folder of this module. The first call
    // decides the API, and later calls return its result. Null if the DLLs are missing.
    const Library* LoadFsrLibrary(const wchar_t* api);

    // Creates an upscaler on the device that the backend description names. The library must be loaded.
    int32_t CreateOn(ffxCreateContextDescHeader& backend, const Options& options, void** upscaler);

    // Records the FSR passes for a frame whose textures are already described to the FSR API.
    int32_t Dispatch(void* upscaler, const Frame& frame, void* commandList, const FfxApiResource& color,
        const FfxApiResource& depth, const FfxApiResource& motion, const FfxApiResource& output);
}
