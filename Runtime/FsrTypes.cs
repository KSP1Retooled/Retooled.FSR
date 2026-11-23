using System;
using UnityEngine;

namespace Retooled.FSR;

/// <summary>The FSR API's <c>FfxApiReturnCodes</c>, plus results for steps that never reached the API.</summary>
public enum FsrResult
{
    Ok = 0,
    Error = 1,
    ErrorUnknownDescType = 2,

    /// <summary>Direct3D or FSR itself returned an error.</summary>
    ErrorRuntimeError = 3,
    NoProvider = 4,
    ErrorMemory = 5,
    ErrorParameter = 6,
    ProviderNoSupportNewDescType = 7,

    /// <summary>amd_fidelityfx_loader_dx12.dll is not in the plugin's folder, or cannot be loaded.</summary>
    ErrorLibraryMissing = 1000,

    /// <summary>The graphics API is not Direct3D 12, or Unity lacks the plugin interface for it.</summary>
    ErrorUnsupportedGraphicsApi = 1001,

    /// <summary>The step has not happened yet.</summary>
    NotRun = int.MaxValue,
}

/// <summary>The FSR API's <c>FfxApiCreateContextUpscaleFlags</c>, which describe the inputs of a context.</summary>
[Flags]
public enum FsrContextFlags : uint
{
    None = 0,

    /// <summary>The color texture holds values that may exceed one.</summary>
    HighDynamicRange = 1 << 0,

    /// <summary>The motion vectors are at the output size, not the render size.</summary>
    DisplayResolutionMotionVectors = 1 << 1,

    /// <summary>The motion vectors include the projection jitter.</summary>
    MotionVectorsJitterCancellation = 1 << 2,

    /// <summary>Nearer surfaces have larger depth values.</summary>
    DepthInverted = 1 << 3,

    /// <summary>The depth comes from a projection with an infinitely far plane.</summary>
    DepthInfinite = 1 << 4,

    /// <summary>FSR works out the exposure from the image.</summary>
    AutoExposure = 1 << 5,

    /// <summary>The render size changes from frame to frame.</summary>
    DynamicResolution = 1 << 6,

    /// <summary>FSR checks the inputs of every frame and reports problems as warnings. For development only.</summary>
    DebugChecking = 1 << 7,

    /// <summary>The color texture holds perceptually encoded values, such as sRGB, in place of linear ones.</summary>
    NonLinearColorSpace = 1 << 8,

    /// <summary>Allows <see cref="FsrFrameFlags.DrawDebugView"/>, which can take more video memory.</summary>
    DebugVisualization = 1 << 9,
}

/// <summary>The FSR API's <c>FfxApiDispatchFsrUpscaleFlags</c>.</summary>
[Flags]
public enum FsrFrameFlags : uint
{
    None = 0,

    /// <summary>FSR draws its motion vectors and other internal images over the output.</summary>
    DrawDebugView = 1 << 0,

    /// <summary>The color texture holds sRGB-encoded values.</summary>
    NonLinearColorSrgb = 1 << 1,

    /// <summary>The color texture holds PQ-encoded values.</summary>
    NonLinearColorPq = 1 << 2,
}

/// <summary>Which of the FSR API's messages go to the Unity log.</summary>
public enum FsrLogLevel
{
    Off = 0,
    Errors = 1,
    Warnings = 2,
}

/// <summary>How far a context has come on the render thread.</summary>
public enum FsrStage
{
    Pending = 0,
    Ready = 1,
    Failed = -1,
}

/// <summary>The inputs of one evaluated frame.</summary>
/// <remarks>
/// Textures are native pointers, from <see cref="Texture.GetNativeTexturePtr"/>. The output texture needs
/// <see cref="RenderTexture.enableRandomWrite"/>.
/// </remarks>
public struct FsrFrame
{
    /// <summary>The jittered image, at the render size.</summary>
    public IntPtr Color;

    /// <summary>A one-channel float texture, at the render size.</summary>
    public IntPtr Depth;

    /// <summary>Two-channel float motion vectors, at the render size.</summary>
    public IntPtr Motion;

    public IntPtr Output;

    public Vector2Int RenderSize;

    public Vector2Int OutputSize;

    /// <summary>The distance that the projection jitter moved the image, in render pixels.</summary>
    /// <remarks>
    /// Positive values move the image towards later columns and later rows of the texture. Each component is between
    /// -0.5 and 0.5, and the two must never both be zero.
    /// </remarks>
    public Vector2 Jitter;

    /// <summary>
    /// Multiplying a motion vector by this scale gives the offset from a pixel to its previous position, as a fraction
    /// of the image, with the origin at the top left.
    /// </summary>
    public Vector2 MotionScale;

    /// <summary>The distance to the camera's near plane, in view space units.</summary>
    public float CameraNear;

    /// <summary>The distance to the camera's far plane. <see cref="float.MaxValue"/> for an infinitely far plane.</summary>
    public float CameraFar;

    /// <summary>The camera's vertical field of view, in radians.</summary>
    public float CameraFieldOfView;

    /// <summary>The time since the previous frame, in milliseconds.</summary>
    public float FrameTime;

    /// <summary>From zero to one. Zero skips FSR's sharpening pass.</summary>
    public float Sharpness;

    public FsrFrameFlags Flags;

    /// <summary>The previous frame has no connection to this one, so FSR drops its history.</summary>
    public bool Reset;
}

/// <summary>A snapshot of a context's state, as last written by the render thread.</summary>
public readonly struct FsrStatus
{
    public readonly FsrStage Stage;

    public readonly FsrResult CreateResult;

    /// <summary>The result of the last evaluated frame.</summary>
    public readonly FsrResult EvaluateResult;

    /// <summary>The number of frames evaluated so far.</summary>
    public readonly int Evaluations;

    /// <summary>The video memory that FSR uses for this context, in bytes.</summary>
    public readonly long VideoMemory;

    /// <summary>The FSR version that the API chose for the GPU, such as "4.1.2". Empty until the context is ready.</summary>
    /// <remarks>
    /// FSR 4 needs a recent AMD GPU, and other GPUs get FSR 3.1. A trailing " *" marks a version that the graphics
    /// driver provided in place of the one in the package.
    /// </remarks>
    public readonly string Version;

    internal FsrStatus(in FsrNative.Status status)
    {
        Stage = status.Stage;
        CreateResult = status.CreateResult;
        EvaluateResult = status.EvaluateResult;
        Evaluations = (int)status.Evaluations;
        VideoMemory = (long)status.VideoMemory;
        Version = status.Version ?? "";
    }

    public override string ToString() =>
        $"stage {Stage}, create {CreateResult}, evaluate {EvaluateResult}, {Evaluations} evaluations, "
        + $"FSR {Version}, {VideoMemory / (1024.0 * 1024.0):F1} MB";
}
