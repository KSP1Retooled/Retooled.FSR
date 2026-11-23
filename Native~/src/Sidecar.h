#pragma once

#include <cstdint>
#include <memory>

#include "Fsr.h"

struct ID3D11Device;
struct ID3D12Device;

namespace fsr
{
    // Runs FSR on a Direct3D 12 device next to a Direct3D 11 device, since the FSR API has no Direct3D 11 backend.
    //
    // Each evaluation copies the Direct3D 11 inputs into textures that both devices share and signals a shared fence.
    // The Direct3D 12 queue waits for that signal, runs FSR and signals again. The Direct3D 11 context waits for the
    // second signal on the GPU, then copies the result back. The CPU doesn't wait for either device.
    //
    // Every method must be called from the thread that uses the Direct3D 11 device's immediate context.
    class Sidecar
    {
    public:
        // Creates the Direct3D 12 device on the Direct3D 11 device's adapter. Returns null if Direct3D 12 is missing,
        // or if the Direct3D 11 device has no shared fences, which need Direct3D 11.4.
        static std::unique_ptr<Sidecar> Create(ID3D11Device* device);

        virtual ~Sidecar() = default;

        // The device to create upscalers on.
        virtual ID3D12Device* Device() const = 0;

        // Evaluates a frame whose textures are ID3D11Texture2D pointers, on an upscaler created on Device(). The output
        // needs no unordered access, since FSR writes a shared texture that is then copied into it.
        virtual int32_t Evaluate(void* upscaler, const Frame& frame) = 0;

        // The fence value that marks the end of the last evaluation submitted so far.
        virtual uint64_t Submitted() const = 0;

        // The fence value that the Direct3D 12 queue has reached.
        virtual uint64_t Completed() const = 0;

        // Waits on the CPU until the Direct3D 12 queue has finished everything submitted to it.
        virtual void WaitForIdle() = 0;
    };
}
