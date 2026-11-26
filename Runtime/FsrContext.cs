using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using UnityEngine;
using UnityEngine.Rendering;

namespace Retooled.FSR;

/// <summary>An FSR upscaling context on Unity's graphics device, for one view at one output size.</summary>
/// <remarks>
/// FSR records its passes into the graphics API's own command list, so every call runs as a plugin event on the
/// render thread. Methods queue or record those events and return at once. <see cref="Status"/> shows what the
/// render thread has done so far.
///
/// Direct3D 12 and Direct3D 11 are supported. On Direct3D 11 the plugin runs FSR on a Direct3D 12 device of its own
/// and copies the textures between the two devices. On anything else the context fails and <see cref="Status"/> says
/// why. The FSR API picks the FSR version for the GPU: FSR 4 on recent AMD GPUs and FSR 3.1 on everything else.
/// </remarks>
public sealed class FsrContext : IDisposable
{
    static IntPtr eventFunction;

    readonly CommandBuffer commands = new() { name = "FSR" };

    IntPtr native;

    int frames;

    string[] versions;

    /// <summary>Queues the creation of a context.</summary>
    /// <remarks>Creation loads the FSR DLLs and compiles FSR's shaders on the render thread.</remarks>
    /// <param name="renderSize">The largest size that the inputs of a frame may have.</param>
    /// <param name="outputSize">The size of the output. Equal to the render size for native anti-aliasing.</param>
    public FsrContext(Vector2Int renderSize, Vector2Int outputSize, FsrContextFlags flags)
    {
        var options = new FsrNative.Options
        {
            RenderWidth = (uint)renderSize.x,
            RenderHeight = (uint)renderSize.y,
            OutputWidth = (uint)outputSize.x,
            OutputHeight = (uint)outputSize.y,
            Flags = flags,
        };
        native = FsrNative.RetooledFSR_NewContext(options);
        Issue(FsrNative.Event.Create);
    }

    /// <summary>The graphics API the plugin saw at device creation.</summary>
    public static GraphicsDeviceType Renderer => (GraphicsDeviceType)FsrNative.RetooledFSR_GetRenderer();

    /// <summary>Which of the FSR API's messages go to the Unity log.</summary>
    public static FsrLogLevel LogLevel
    {
        set => FsrNative.RetooledFSR_SetLogLevel(value);
    }

    internal static IntPtr EventFunction
    {
        get
        {
            if (eventFunction == IntPtr.Zero)
                eventFunction = FsrNative.RetooledFSR_GetEventFunction();
            return eventFunction;
        }
    }

    public FsrStatus Status
    {
        get
        {
            ThrowIfDisposed();
            FsrNative.RetooledFSR_GetStatus(native, out FsrNative.Status status);
            return new FsrStatus(status);
        }
    }

    /// <summary>
    /// Every FSR version that the API offers for the GPU, newest first, such as "4.1.2" and "3.1.5". Empty while the
    /// context is pending, and if the FSR DLLs didn't load.
    /// </summary>
    /// <remarks>An AMD GPU without FSR 4 in this list needs a newer graphics driver, or doesn't support FSR 4.</remarks>
    public IReadOnlyList<string> Versions
    {
        get
        {
            ThrowIfDisposed();
            if (versions != null)
                return versions;

            // The render thread sets the versions together with the stage, and never changes them after.
            if (Status.Stage == FsrStage.Pending)
                return Array.Empty<string>();

            var names = new IntPtr[FsrNative.RetooledFSR_GetVersions(native, null, 0)];
            FsrNative.RetooledFSR_GetVersions(native, names, names.Length);
            versions = Array.ConvertAll(names, Marshal.PtrToStringAnsi);
            return versions;
        }
    }

    /// <summary>Records the evaluation of one frame into a command buffer.</summary>
    /// <remarks>
    /// The recorded commands hold this frame's inputs, so record them again every frame and execute them once. On
    /// Direct3D 12, Unity restores its own state afterwards.
    /// </remarks>
    public void Evaluate(CommandBuffer target, in FsrFrame frame)
    {
        ThrowIfDisposed();
        var nativeFrame = new FsrNative.Frame
        {
            Color = frame.Color,
            Depth = frame.Depth,
            Motion = frame.Motion,
            Output = frame.Output,
            RenderWidth = (uint)frame.RenderSize.x,
            RenderHeight = (uint)frame.RenderSize.y,
            OutputWidth = (uint)frame.OutputSize.x,
            OutputHeight = (uint)frame.OutputSize.y,
            JitterX = frame.Jitter.x,
            JitterY = frame.Jitter.y,
            MotionScaleX = frame.MotionScale.x,
            MotionScaleY = frame.MotionScale.y,
            CameraNear = frame.CameraNear,
            CameraFar = frame.CameraFar,
            CameraFovY = frame.CameraFieldOfView,
            FrameTime = frame.FrameTime,
            Sharpness = frame.Sharpness,
            Flags = frame.Flags,
            Reset = frame.Reset ? 1u : 0u,
        };

        // The render thread may still be working on earlier frames, so each frame gets a slot of its own.
        IntPtr slot = FsrNative.RetooledFSR_SetFrame(native, frames++ % FsrNative.FrameSlots, nativeFrame);
        target.IssuePluginEventAndData(EventFunction, (int)FsrNative.Event.Evaluate, slot);
    }

    /// <summary>Queues the destruction of the context.</summary>
    /// <remarks>
    /// Command buffers recorded by <see cref="Evaluate"/> must not run after this. The plugin keeps FSR's textures
    /// until the GPU has finished the frames that used them.
    /// </remarks>
    public void Dispose()
    {
        if (native == IntPtr.Zero)
            return;

        Issue(FsrNative.Event.Destroy);
        native = IntPtr.Zero;
        commands.Release();
        FsrCollector.Start();
    }

    void Issue(FsrNative.Event pluginEvent)
    {
        commands.Clear();
        commands.IssuePluginEventAndData(EventFunction, (int)pluginEvent, native);
        Graphics.ExecuteCommandBuffer(commands);
    }

    void ThrowIfDisposed()
    {
        if (native == IntPtr.Zero)
            throw new ObjectDisposedException(nameof(FsrContext));
    }
}
