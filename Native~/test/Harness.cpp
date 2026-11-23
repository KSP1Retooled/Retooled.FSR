// Runs FSR through Fsr.h on a Direct3D 12 device that this program creates, without Unity.
//
//     FsrHarness [frames]
//
// It fills a color texture with a pattern, evaluates FSR at native resolution on it for a number of frames, and reads
// the output back. The exit code is zero when every step succeeded and the output is not blank. The FSR DLLs must be
// next to the executable.

#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "Fsr.h"

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr uint32_t Width = 1280;
    constexpr uint32_t Height = 720;

    // FfxApiCreateContextUpscaleFlags: inverted depth, an infinite far plane, debug checking and non-linear color.
    constexpr uint32_t ContextFlags = (1 << 3) | (1 << 4) | (1 << 7) | (1 << 8);

    // FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB.
    constexpr uint32_t FrameFlags = 1 << 1;

    // FSR's debug checking repeats its warnings every frame, so each message prints once. It always warns that there
    // is no exposure texture, which is right for an image whose values run from zero to one.
    void Log(int32_t type, const char* message)
    {
        static std::set<std::string> seen;
        if (seen.insert(message).second)
            std::printf("[fsr %s] %s\n", type == 0 ? "error" : "warn", message);
    }

    bool Check(const char* step, int32_t result)
    {
        std::printf("%s: %d\n", step, result);
        return result == fsr::Ok;
    }

    bool Check(const char* step, HRESULT result)
    {
        if (FAILED(result))
            std::printf("%s: HRESULT 0x%08lX\n", step, static_cast<unsigned long>(result));
        return SUCCEEDED(result);
    }

    float Halton(uint32_t index, uint32_t radix)
    {
        float result = 0.0f;
        float fraction = 1.0f / static_cast<float>(radix);
        while (index > 0)
        {
            result += static_cast<float>(index % radix) * fraction;
            index /= radix;
            fraction /= static_cast<float>(radix);
        }
        return result;
    }

    // Diagonal stripes, so that the image has edges for FSR to work on.
    std::vector<uint32_t> Pattern()
    {
        std::vector<uint32_t> pixels(Width * Height);
        for (uint32_t y = 0; y < Height; y++)
        {
            for (uint32_t x = 0; x < Width; x++)
                pixels[y * Width + x] = ((x + 2 * y) / 48) % 2 == 0 ? 0xFF2060E0u : 0xFFE0E0E0u;
        }
        return pixels;
    }

    fsr::Frame NewFrame(uint32_t index)
    {
        fsr::Frame frame{};
        frame.renderWidth = Width;
        frame.renderHeight = Height;
        frame.outputWidth = Width;
        frame.outputHeight = Height;
        frame.jitterX = Halton(index % 8 + 1, 2) - 0.5f;
        frame.jitterY = Halton(index % 8 + 1, 3) - 0.5f;
        frame.motionScaleX = 1.0f;
        frame.motionScaleY = 1.0f;
        frame.cameraNear = 0.1f;
        frame.cameraFar = FLT_MAX;
        frame.cameraFovY = 1.0f;
        frame.frameTime = 16.6f;
        frame.flags = FrameFlags;
        frame.reset = index == 0 ? 1 : 0;
        return frame;
    }

    bool CheckOutput(const uint8_t* data, size_t rowPitch)
    {
        uint64_t sum = 0;
        for (uint32_t y = 0; y < Height; y++)
        {
            const uint8_t* row = data + y * rowPitch;
            for (uint32_t x = 0; x < Width; x++)
                sum += row[x * 4] + row[x * 4 + 1] + row[x * 4 + 2];
        }
        double mean = static_cast<double>(sum) / (3.0 * Width * Height);
        std::printf("output mean: %.2f (the pattern's mean is about 171)\n", mean);
        return mean > 160.0 && mean < 180.0;
    }

    struct Queue
    {
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12Fence> fence;
        HANDLE fenceEvent = nullptr;
        uint64_t fenceValue = 0;

        bool Create()
        {
            D3D12_COMMAND_QUEUE_DESC description{};
            description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            return Check("D3D12CreateDevice", D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))
                && Check("queue", device->CreateCommandQueue(&description, IID_PPV_ARGS(&queue)))
                && Check("allocator", device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))
                && Check("list", device->CreateCommandList(
                    0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)))
                && Check("fence", device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        }

        // Runs the recorded commands, waits for the GPU, and opens the list again.
        bool Submit()
        {
            if (!Check("close", list->Close()))
                return false;
            ID3D12CommandList* lists[] = {list.Get()};
            queue->ExecuteCommandLists(1, lists);
            fenceValue++;
            queue->Signal(fence.Get(), fenceValue);
            fence->SetEventOnCompletion(fenceValue, fenceEvent);
            WaitForSingleObject(fenceEvent, INFINITE);
            return Check("device", device->GetDeviceRemovedReason())
                && Check("allocator reset", allocator->Reset())
                && Check("list reset", list->Reset(allocator.Get(), nullptr));
        }
    };

    bool Run(uint32_t frames)
    {
        Queue gpu;
        if (!gpu.Create())
            return false;

        D3D12_HEAP_PROPERTIES defaultHeap{D3D12_HEAP_TYPE_DEFAULT};
        auto create = [&](DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES resourceState,
                          ComPtr<ID3D12Resource>& texture)
        {
            D3D12_RESOURCE_DESC description{};
            description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            description.Width = Width;
            description.Height = Height;
            description.DepthOrArraySize = 1;
            description.MipLevels = 1;
            description.Format = format;
            description.SampleDesc.Count = 1;
            description.Flags = flags;
            return gpu.device->CreateCommittedResource(
                &defaultHeap, D3D12_HEAP_FLAG_NONE, &description, resourceState, nullptr, IID_PPV_ARGS(&texture));
        };

        // Depth and motion stay zero, which is a still image at the far plane.
        const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        const D3D12_RESOURCE_STATES write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> color, depth, motion, output;
        if (!Check("color", create(DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, color))
            || !Check("depth", create(DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, read, depth))
            || !Check("motion", create(DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, read, motion))
            || !Check("output", create(
                DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, write, output)))
            return false;

        // One buffer layout serves the upload of the pattern and, later, the readback of the output.
        D3D12_RESOURCE_DESC colorDescription = color->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 bufferSize = 0;
        gpu.device->GetCopyableFootprints(&colorDescription, 0, 1, 0, &footprint, nullptr, nullptr, &bufferSize);
        D3D12_RESOURCE_DESC bufferDescription{};
        bufferDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDescription.Width = bufferSize;
        bufferDescription.Height = 1;
        bufferDescription.DepthOrArraySize = 1;
        bufferDescription.MipLevels = 1;
        bufferDescription.SampleDesc.Count = 1;
        bufferDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES uploadHeap{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_HEAP_PROPERTIES readbackHeap{D3D12_HEAP_TYPE_READBACK};
        ComPtr<ID3D12Resource> upload, readback;
        if (!Check("upload buffer", gpu.device->CreateCommittedResource(
                &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDescription, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&upload)))
            || !Check("readback buffer", gpu.device->CreateCommittedResource(
                &readbackHeap, D3D12_HEAP_FLAG_NONE, &bufferDescription, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&readback))))
            return false;

        std::vector<uint32_t> pattern = Pattern();
        uint8_t* mapped = nullptr;
        if (!Check("map upload", upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))))
            return false;
        for (uint32_t y = 0; y < Height; y++)
            std::memcpy(mapped + footprint.Offset + y * footprint.Footprint.RowPitch, &pattern[y * Width], Width * 4);
        upload->Unmap(0, nullptr);

        auto transition = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
        {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = resource;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = before;
            barrier.Transition.StateAfter = after;
            gpu.list->ResourceBarrier(1, &barrier);
        };

        D3D12_TEXTURE_COPY_LOCATION texture{};
        texture.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION buffer{};
        buffer.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        buffer.PlacedFootprint = footprint;

        texture.pResource = color.Get();
        buffer.pResource = upload.Get();
        gpu.list->CopyTextureRegion(&texture, 0, 0, 0, &buffer, nullptr);
        transition(color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, read);
        if (!gpu.Submit())
            return false;

        fsr::SetLog(Log);
        fsr::Options options{Width, Height, Width, Height, ContextFlags};
        void* upscaler = nullptr;
        if (!Check("create", fsr::Create(gpu.device.Get(), options, &upscaler)))
            return false;

        fsr::Info info = fsr::Describe(upscaler);
        std::printf("version: %s\n", info.version);
        std::printf("video memory: %.1f MB\n", static_cast<double>(info.videoMemory) / (1024.0 * 1024.0));

        for (uint32_t i = 0; i < frames; i++)
        {
            fsr::Frame frame = NewFrame(i);
            frame.color = color.Get();
            frame.depth = depth.Get();
            frame.motion = motion.Get();
            frame.output = output.Get();
            int32_t evaluated = fsr::Evaluate(upscaler, frame, gpu.list.Get());
            bool submitted = gpu.Submit();
            if (i == 0 || evaluated != fsr::Ok)
                std::printf("frame %u: evaluate %d\n", i, evaluated);
            if (evaluated != fsr::Ok || !submitted)
                return false;
        }

        // The readback also proves that FSR returned the output in the state it was given in.
        transition(output.Get(), write, D3D12_RESOURCE_STATE_COPY_SOURCE);
        texture.pResource = output.Get();
        buffer.pResource = readback.Get();
        gpu.list->CopyTextureRegion(&buffer, 0, 0, 0, &texture, nullptr);
        if (!gpu.Submit() || !Check("map readback", readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped))))
            return false;
        bool written = CheckOutput(mapped + footprint.Offset, footprint.Footprint.RowPitch);
        readback->Unmap(0, nullptr);

        // Every submit waited for the GPU, so nothing still uses the upscaler's textures.
        return Check("destroy", fsr::Destroy(upscaler)) && written;
    }
}

int main(int argc, char** argv)
{
    uint32_t frames = argc > 1 ? static_cast<uint32_t>(std::atoi(argv[1])) : 16;
    bool passed = Run(frames);
    std::printf(passed ? "PASSED\n" : "FAILED\n");
    return passed ? 0 : 1;
}
