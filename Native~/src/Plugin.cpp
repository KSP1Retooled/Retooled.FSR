#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Unity's D3D12 header expects the Direct3D and DXGI types to be declared already.
#include <windows.h>
#include <d3d12.h>
#include <dxgi.h>

#include "Fsr.h"
#include "IUnityGraphics.h"
#include "IUnityGraphicsD3D12.h"
#include "IUnityInterface.h"
#include "IUnityLog.h"

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
        // The value that Unity's frame fence reaches when the GPU has finished the frame of the destroy event.
        uint64_t fenceValue;
    };

    IUnityInterfaces* interfaces;
    IUnityGraphics* graphics;
    IUnityLog* unityLog;
    IUnityGraphicsD3D12v8* d3d12;
    UnityGfxRenderer renderer = kUnityGfxRendererNull;
    std::atomic<int32_t> logLevel{LogWarnings};

    // Used on the render thread only.
    std::vector<Retired> retired;

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

        uint64_t completed = d3d12->GetFrameFence()->GetCompletedValue();
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
            // Version 8 is the first that lets a plugin ask Unity for a resource state.
            if (renderer == kUnityGfxRendererD3D12)
                d3d12 = UNITY_GET_INTERFACE(interfaces, IUnityGraphicsD3D12v8);
        }
        else if (type == kUnityGfxDeviceEventShutdown)
        {
            // The upscalers hold textures of the device, so they go before it does.
            if (d3d12 != nullptr)
                Collect(true);
            renderer = kUnityGfxRendererNull;
            d3d12 = nullptr;
        }
    }

    void Create(Context& context)
    {
        fsr::Options options;
        {
            std::lock_guard lock(context.mutex);
            options = context.options;
        }

        int32_t result = fsr::ErrorUnsupportedGraphicsApi;
        fsr::Info info{};
        if (d3d12 != nullptr)
        {
            fsr::SetLog(OnLog);
            result = fsr::Create(d3d12->GetDevice(), options, &context.upscaler);
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

        UnityGraphicsD3D12RecordingState recording{};
        if (!d3d12->CommandRecordingState(&recording))
            return;

        // Unity adds the barriers to its command list and tracks the new states. FSR returns the textures in these
        // states.
        const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        d3d12->RequestResourceState(static_cast<ID3D12Resource*>(frame.color), read);
        d3d12->RequestResourceState(static_cast<ID3D12Resource*>(frame.depth), read);
        d3d12->RequestResourceState(static_cast<ID3D12Resource*>(frame.motion), read);
        d3d12->RequestResourceState(static_cast<ID3D12Resource*>(frame.output), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        int32_t result = fsr::Evaluate(context.upscaler, frame, recording.commandList);

        std::lock_guard lock(context.mutex);
        context.status.evaluateResult = result;
        context.status.evaluations++;
    }

    // The command list that Unity is recording may hold this upscaler's passes, and FSR releases its textures as soon
    // as the upscaler is destroyed. So the upscaler waits in the retired list until the GPU has finished this frame.
    //
    // Unity can send this event after the device's shutdown event when it quits. The device is gone by then, so the
    // upscaler is left to the end of the process.
    void Destroy(Context* context)
    {
        if (context->upscaler != nullptr && d3d12 != nullptr)
            retired.push_back({context->upscaler, d3d12->GetNextFrameFenceValue()});
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
            if (d3d12 != nullptr)
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
