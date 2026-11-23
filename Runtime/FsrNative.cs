using System;
using System.Runtime.InteropServices;

namespace Retooled.FSR;

/// <summary>Declarations for RetooledFSR.dll, the native plugin in <c>Native~</c>.</summary>
internal static class FsrNative
{
    const string Plugin = "RetooledFSR";

    /// <summary>The number of frame slots in a native context, <c>FrameSlots</c> in Plugin.cpp.</summary>
    internal const int FrameSlots = 8;

    /// <summary>Render-thread events, handled by <c>OnRenderEvent</c> in Plugin.cpp.</summary>
    internal enum Event
    {
        Create = 0,
        Evaluate = 1,
        Destroy = 2,
        Collect = 3,
    }

    /// <summary>Fsr.h's <c>Options</c>, field for field.</summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct Options
    {
        public uint RenderWidth;
        public uint RenderHeight;
        public uint OutputWidth;
        public uint OutputHeight;
        public FsrContextFlags Flags;
    }

    /// <summary>Fsr.h's <c>Frame</c>, field for field.</summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct Frame
    {
        public IntPtr Color;
        public IntPtr Depth;
        public IntPtr Motion;
        public IntPtr Output;
        public uint RenderWidth;
        public uint RenderHeight;
        public uint OutputWidth;
        public uint OutputHeight;
        public float JitterX;
        public float JitterY;
        public float MotionScaleX;
        public float MotionScaleY;
        public float CameraNear;
        public float CameraFar;
        public float CameraFovY;
        public float FrameTime;
        public float Sharpness;
        public FsrFrameFlags Flags;
        public uint Reset;
    }

    /// <summary>Plugin.cpp's <c>Status</c>, field for field, with Fsr.h's <c>Info</c> at its end.</summary>
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    internal struct Status
    {
        public FsrStage Stage;
        public FsrResult CreateResult;
        public FsrResult EvaluateResult;
        public uint Evaluations;
        public ulong VideoMemory;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string Version;
    }

    [DllImport(Plugin)]
    internal static extern int RetooledFSR_GetRenderer();

    [DllImport(Plugin)]
    internal static extern void RetooledFSR_SetLogLevel(FsrLogLevel level);

    [DllImport(Plugin)]
    internal static extern IntPtr RetooledFSR_GetEventFunction();

    [DllImport(Plugin)]
    internal static extern IntPtr RetooledFSR_NewContext(in Options options);

    [DllImport(Plugin)]
    internal static extern void RetooledFSR_GetStatus(IntPtr context, out Status status);

    /// <summary>Returns the slot that holds the frame, which is the data of that frame's evaluate event.</summary>
    [DllImport(Plugin)]
    internal static extern IntPtr RetooledFSR_SetFrame(IntPtr context, int index, in Frame frame);
}
