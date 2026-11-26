#include "Fsr.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>
#include <d3d12.h>

#include "api/include/dx12/ffx_api_dx12.h"
#include "upscalers/include/ffx_upscale.h"

namespace
{
    struct Library
    {
        PfnFfxCreateContext CreateContext = nullptr;
        PfnFfxDestroyContext DestroyContext = nullptr;
        PfnFfxQuery Query = nullptr;
        PfnFfxDispatch Dispatch = nullptr;
        bool loaded = false;
    };

    Library library;
    std::once_flag loadOnce;
    std::atomic<fsr::LogFunction*> logFunction{nullptr};

    std::wstring ModuleDirectory()
    {
        HMODULE self = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ModuleDirectory),
            &self);

        std::wstring path(MAX_PATH, L'\0');
        while (true)
        {
            DWORD length = GetModuleFileNameW(self, path.data(), static_cast<DWORD>(path.size()));
            if (length < path.size())
            {
                path.resize(length);
                break;
            }
            path.resize(path.size() * 2);
        }
        return path.substr(0, path.find_last_of(L"\\/") + 1);
    }

    template <typename T>
    bool Resolve(HMODULE module, const char* name, T& function)
    {
        function = reinterpret_cast<T>(GetProcAddress(module, name));
        return function != nullptr;
    }

    // The loader DLL uses an upscaler DLL that is already loaded, then looks next to the application's executable, and
    // only then in its own folder. Unity ships an older upscaler DLL next to its executables, so the one in the folder
    // of this module is loaded first. The loader then finds it by name and the two versions match.
    void Load()
    {
        std::wstring directory = ModuleDirectory();
        std::wstring upscaler = directory + L"amd_fidelityfx_upscaler_dx12.dll";
        if (LoadLibraryExW(upscaler.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) == nullptr)
            return;

        std::wstring path = directory + L"amd_fidelityfx_loader_dx12.dll";
        HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (module == nullptr)
            return;

        library.loaded = Resolve(module, "ffxCreateContext", library.CreateContext)
            && Resolve(module, "ffxDestroyContext", library.DestroyContext)
            && Resolve(module, "ffxQuery", library.Query)
            && Resolve(module, "ffxDispatch", library.Dispatch);
    }

    void OnMessage(uint32_t type, const wchar_t* message)
    {
        fsr::LogFunction* log = logFunction;
        if (log == nullptr)
            return;

        int length = WideCharToMultiByte(CP_UTF8, 0, message, -1, nullptr, 0, nullptr, nullptr);
        std::string text(static_cast<size_t>(length), '\0');
        WideCharToMultiByte(CP_UTF8, 0, message, -1, text.data(), length, nullptr, nullptr);
        log(static_cast<int32_t>(type), text.c_str());
    }

    FfxApiResource ToResource(void* texture, uint32_t resourceState)
    {
        return ffxApiGetResourceDX12(static_cast<ID3D12Resource*>(texture), resourceState);
    }
}

namespace fsr
{
    void SetLog(LogFunction* log)
    {
        logFunction = log;
    }

    void LogError(const char* message)
    {
        fsr::LogFunction* log = logFunction;
        if (log != nullptr)
            log(0, message);
    }

    int32_t Create(void* device, const Options& options, void** upscaler)
    {
        *upscaler = nullptr;
        std::call_once(loadOnce, Load);
        if (!library.loaded)
            return ErrorLibraryMissing;

        ffxCreateBackendDX12Desc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = static_cast<ID3D12Device*>(device);

        // Tells the upscaler DLL, or a newer one in the graphics driver, which version of the API these structures
        // come from.
        ffxCreateContextDescUpscaleVersion version{};
        version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
        version.header.pNext = &backend.header;
        version.version = FFX_UPSCALER_VERSION;

        ffxCreateContextDescUpscale create{};
        create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        create.header.pNext = &version.header;
        create.flags = options.flags;
        create.maxRenderSize = {options.renderWidth, options.renderHeight};
        create.maxUpscaleSize = {options.outputWidth, options.outputHeight};
        create.fpMessage = OnMessage;

        ffxContext context = nullptr;
        ffxReturnCode_t result = library.CreateContext(&context, &create.header, nullptr);
        if (result == FFX_API_RETURN_OK)
            *upscaler = context;
        return static_cast<int32_t>(result);
    }

    Info Describe(void* upscaler)
    {
        Info info{};
        ffxContext context = upscaler;

        ffxQueryGetProviderVersion version{};
        version.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        bool found = library.Query(&context, &version.header) == FFX_API_RETURN_OK && version.versionName != nullptr;
        if (found)
            strncpy_s(info.version, version.versionName, _TRUNCATE);

        FfxApiEffectMemoryUsage usage{};
        ffxQueryDescUpscaleGetGPUMemoryUsage memory{};
        memory.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE;
        memory.gpuMemoryUsageUpscaler = &usage;
        if (library.Query(&context, &memory.header) == FFX_API_RETURN_OK)
            info.videoMemory = usage.totalUsageInBytes;
        return info;
    }

    std::vector<std::string> ListVersions(void* device)
    {
        if (!library.loaded)
            return {};

        ffxQueryDescGetVersions query{};
        query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        query.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        query.device = device;
        uint64_t count = 0;
        query.outputCount = &count;
        if (library.Query(nullptr, &query.header) != FFX_API_RETURN_OK || count == 0)
            return {};

        std::vector<const char*> names(count, nullptr);
        query.versionNames = names.data();
        if (library.Query(nullptr, &query.header) != FFX_API_RETURN_OK)
            return {};

        std::vector<std::string> versions;
        for (const char* name : names)
        {
            if (name != nullptr)
                versions.emplace_back(name);
        }

        return versions;
    }

    int32_t Evaluate(void* upscaler, const Frame& frame, void* commandList)
    {
        ffxDispatchDescUpscale dispatch{};
        dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        dispatch.commandList = commandList;
        dispatch.color = ToResource(frame.color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        dispatch.depth = ToResource(frame.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        dispatch.motionVectors = ToResource(frame.motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        dispatch.output = ToResource(frame.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch.jitterOffset = {frame.jitterX, frame.jitterY};
        // FSR wants the offset to the previous position in render pixels.
        dispatch.motionVectorScale = {
            frame.motionScaleX * static_cast<float>(frame.renderWidth),
            frame.motionScaleY * static_cast<float>(frame.renderHeight),
        };
        dispatch.renderSize = {frame.renderWidth, frame.renderHeight};
        dispatch.upscaleSize = {frame.outputWidth, frame.outputHeight};
        dispatch.enableSharpening = frame.sharpness > 0.0f;
        dispatch.sharpness = frame.sharpness;
        dispatch.frameTimeDelta = frame.frameTime;
        dispatch.preExposure = 1.0f;
        dispatch.reset = frame.reset != 0;
        dispatch.cameraNear = frame.cameraNear;
        dispatch.cameraFar = frame.cameraFar;
        dispatch.cameraFovAngleVertical = frame.cameraFovY;
        dispatch.viewSpaceToMetersFactor = 1.0f;
        dispatch.flags = frame.flags;

        ffxContext context = upscaler;
        return static_cast<int32_t>(library.Dispatch(&context, &dispatch.header));
    }

    int32_t Destroy(void* upscaler)
    {
        ffxContext context = upscaler;
        return static_cast<int32_t>(library.DestroyContext(&context, nullptr));
    }
}
