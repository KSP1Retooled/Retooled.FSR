#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Unity's Direct3D headers expect the Direct3D and DXGI types to be declared already.
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>

#include "Fsr.h"
#include "IUnityGraphics.h"
#include "IUnityGraphicsD3D11.h"
#include "IUnityGraphicsD3D12.h"
#include "IUnityInterface.h"
#include "IUnityLog.h"
#include "Sidecar.h"

namespace
{
    // Frames that the main thread may prepare before the render thread has evaluated the oldest of them.
    constexpr int FrameSlots = 8;

    enum class Stage : int32_t
    {
        Pending = 0,
        Ready = 1,
        Failed = -1,
    };

    enum Event
    {
        CreateEvent = 0,
        EvaluateEvent = 1,
        DestroyEvent = 2,
        CollectEvent = 3,
    };

    // Shared with C# as FsrLogLevel.
    enum LogLevel : int32_t
    {
        LogOff = 0,
        LogErrors = 1,
        LogWarnings = 2,
    };

    // Shared with C# as FsrNative.Status, field for field.
    struct Status
    {
        Stage stage = Stage::Pending;
        int32_t createResult = fsr::NotRun;
        int32_t evaluateResult = fsr::NotRun;
        uint32_t evaluations = 0;
        fsr::Info info{};
    };

    struct Context;

    // The data of an evaluate event. The main thread fills a slot each frame and the render thread reads it when it
    // reaches that frame's commands.
    struct FrameSlot
    {
        Context* context;
        fsr::Frame frame;
    };

    // The main thread writes the options and frame slots and reads the status. The render thread does the opposite,
    // and it alone uses the upscaler.
    struct Context
    {
        std::mutex mutex;
        Status status;
        fsr::Options options{};
        void* upscaler = nullptr;
        FrameSlot slots[FrameSlots]{};
    };

    // An upscaler whose context was destroyed while the GPU might still have been running its passes.
    struct Retired
    {
        void* upscaler;
        // The value that the device's fence reaches when the GPU has finished the upscaler's last frame: Unity's
        // frame fence on Direct3D 12, or the sidecar's fence on Direct3D 11.
        uint64_t fenceValue;
    };

    IUnityInterfaces* interfaces;
    IUnityGraphics* graphics;
    IUnityLog* unityLog;
    IUnityGraphicsD3D12v8* d3d12;
    IUnityGraphicsD3D11* d3d11;
    UnityGfxRenderer renderer = kUnityGfxRendererNull;
    std::atomic<int32_t> logLevel{LogWarnings};

    // Used on the render thread only.
    std::vector<Retired> retired;

    // On Direct3D 11, FSR runs on a Direct3D 12 device of its own. It is set up with the first context, so that a game
    // that never uses FSR never creates the device. Used on the render thread only.
    std::unique_ptr<fsr::Sidecar> sidecar;
    bool sidecarTried;

    // The FSR API may call this from its own threads. Unity's log accepts messages from any thread.
    void OnLog(int32_t type, const char* message)
    {
        bool error = type == 0;
        if (unityLog == nullptr || logLevel < (error ? LogErrors : LogWarnings))
            return;

        std::string text = std::string("[FSR] ") + message;
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
            text.pop_back();
        unityLog->Log(error ? kUnityLogTypeError : kUnityLogTypeWarning, text.c_str(), "", 0);
    }

    // Destroys the retired upscalers that the GPU has finished with, or all of them.
    void Collect(bool all)
    {
        if (retired.empty())
            return;

        if (all && sidecar != nullptr)
            sidecar->WaitForIdle();
        uint64_t completed = d3d12 != nullptr ? d3d12->GetFrameFence()->GetCompletedValue()
            : sidecar != nullptr ? sidecar->Completed()
            : 0;
        std::erase_if(retired, [&](const Retired& entry)
        {
            if (!all && completed < entry.fenceValue)
                return false;
            fsr::Destroy(entry.upscaler);
            return true;
        });
    }

    void UNITY_INTERFACE_API OnGraphicsDeviceEvent(UnityGfxDeviceEventType type)
    {
        if (type == kUnityGfxDeviceEventInitialize)
        {
            renderer = graphics->GetRenderer();
            d3d12 = nullptr;
            d3d11 = nullptr;
            // Version 8 is the first that lets a plugin ask Unity for a resource state.
            // UNITY_GET_INTERFACE ends in a semicolon, so an else can't follow it.
            if (renderer == kUnityGfxRendererD3D12)
                d3d12 = UNITY_GET_INTERFACE(interfaces, IUnityGraphicsD3D12v8);
            if (renderer == kUnityGfxRendererD3D11)
                d3d11 = UNITY_GET_INTERFACE(interfaces, IUnityGraphicsD3D11);
        }
        else if (type == kUnityGfxDeviceEventShutdown)
        {
            // The upscalers hold textures of the device, so they go before it does. The sidecar's shared textures
            // belong to Unity's Direct3D 11 device too.
            Collect(true);
            sidecar.reset();
            sidecarTried = false;
            renderer = kUnityGfxRendererNull;
            d3d12 = nullptr;
            d3d11 = nullptr;
        }
    }

    // The device that upscalers are created on, or null with the reason in result.
    ID3D12Device* UpscalerDevice(int32_t& result)
    {
        if (d3d12 != nullptr)
            return d3d12->GetDevice();
        if (d3d11 == nullptr)
        {
            result = fsr::ErrorUnsupportedGraphicsApi;
            return nullptr;
        }

        if (!sidecarTried)
        {
            sidecarTried = true;
            sidecar = fsr::Sidecar::Create(d3d11->GetDevice());
            if (sidecar == nullptr)
                fsr::LogError("cannot set up a Direct3D 12 device next to Unity's Direct3D 11 device");
        }
        result = fsr::ErrorSidecarFailed;
        return sidecar != nullptr ? sidecar->Device() : nullptr;
    }

    void Create(Context& context)
    {
        fsr::Options options;
        {
            std::lock_guard lock(context.mutex);
            options = context.options;
        }

        fsr::SetLog(OnLog);
        int32_t result = fsr::NotRun;
        fsr::Info info{};
        if (ID3D12Device* device = UpscalerDevice(result))
        {
            result = fsr::Create(device, options, &context.upscaler);
            if (result == fsr::Ok)
                info = fsr::Describe(context.upscaler);
        }

        std::lock_guard lock(context.mutex);
        context.status.createResult = result;
        context.status.info = info;
        context.status.stage = result == fsr::Ok ? Stage::Ready : Stage::Failed;
    }

    void Evaluate(const FrameSlot& slot)
    {
        Context& context = *slot.context;
        if (context.upscaler == nullptr)
            return;

        fsr::Frame frame;
        {
            std::lock_guard lock(context.mutex);
            frame = slot.frame;
        }

        int32_t result;
        if (sidecar != nullptr)
        {
            result = sidecar->Evaluate(context.upscaler, frame);
        }
        else
        {
            UnityGraphicsD3D12RecordingState recording{};
            if (!d3d12->CommandRecordingState(&recording))
                return;

            // Unity adds the barriers to its command list and tracks the new states. FSR returns the textures in these
            // states.
            const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            d3d12->RequestResourceState(static_cast<ID3D12Resource*>(frame.color), read);
            d3d12->RequestResourceState(static_cast<ID3D12Resource*>(frame.depth), read);
            d3d12->RequestResourceState(static_cast<ID3D12Resource*>(frame.motion), read);
            d3d12->RequestResourceState(
                static_cast<ID3D12Resource*>(frame.output), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            result = fsr::Evaluate(context.upscaler, frame, recording.commandList);
        }

        std::lock_guard lock(context.mutex);
        context.status.evaluateResult = result;
        context.status.evaluations++;
    }

    // The GPU may still be running this upscaler's passes, and FSR releases its textures as soon as the upscaler is
    // destroyed. So the upscaler waits in the retired list until the GPU has finished them: on Direct3D 12, the frame
    // that Unity is recording, and on Direct3D 11, the sidecar's last evaluation.
    //
    // Unity can send this event after the device's shutdown event when it quits. The device is gone by then, so the
    // upscaler is left to the end of the process.
    void Destroy(Context* context)
    {
        if (context->upscaler != nullptr && d3d12 != nullptr)
            retired.push_back({context->upscaler, d3d12->GetNextFrameFenceValue()});
        else if (context->upscaler != nullptr && sidecar != nullptr)
            retired.push_back({context->upscaler, sidecar->Submitted()});
        delete context;
    }

    void UNITY_INTERFACE_API OnRenderEvent(int event, void* data)
    {
        switch (event)
        {
        case CreateEvent:
            Create(*static_cast<Context*>(data));
            break;
        case EvaluateEvent:
            Evaluate(*static_cast<FrameSlot*>(data));
            break;
        case DestroyEvent:
            Destroy(static_cast<Context*>(data));
            break;
        case CollectEvent:
            Collect(false);
            break;
        default:
            break;
        }
    }
}

extern "C"
{
    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces* unityInterfaces)
    {
        interfaces = unityInterfaces;
        graphics = UNITY_GET_INTERFACE(interfaces, IUnityGraphics);
        unityLog = UNITY_GET_INTERFACE(interfaces, IUnityLog);
        graphics->RegisterDeviceEventCallback(OnGraphicsDeviceEvent);

        // A plugin loaded after the device was created misses the initialize event.
        OnGraphicsDeviceEvent(kUnityGfxDeviceEventInitialize);
    }

    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityPluginUnload()
    {
        graphics->UnregisterDeviceEventCallback(OnGraphicsDeviceEvent);
    }

    UNITY_INTERFACE_EXPORT int RetooledFSR_GetRenderer()
    {
        return renderer;
    }

    UNITY_INTERFACE_EXPORT void RetooledFSR_SetLogLevel(int level)
    {
        logLevel = level;
    }

    UNITY_INTERFACE_EXPORT UnityRenderingEventAndData RetooledFSR_GetEventFunction()
    {
        return OnRenderEvent;
    }

    // The context is freed by its destroy event on the render thread.
    UNITY_INTERFACE_EXPORT Context* RetooledFSR_NewContext(const fsr::Options* options)
    {
        auto* context = new Context();
        context->options = *options;
        for (FrameSlot& slot : context->slots)
            slot.context = context;
        return context;
    }

    UNITY_INTERFACE_EXPORT void RetooledFSR_GetStatus(Context* context, Status* status)
    {
        std::lock_guard lock(context->mutex);
        *status = context->status;
    }

    // Stores a frame's inputs in a slot and returns the slot, which is the data of that frame's evaluate event.
    UNITY_INTERFACE_EXPORT FrameSlot* RetooledFSR_SetFrame(Context* context, int index, const fsr::Frame* frame)
    {
        FrameSlot& slot = context->slots[static_cast<uint32_t>(index) % FrameSlots];
        std::lock_guard lock(context->mutex);
        slot.frame = *frame;
        return &slot;
    }
}
