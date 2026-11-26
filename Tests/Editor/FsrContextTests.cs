using System.Collections;
using System.Runtime.InteropServices;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.TestTools;

namespace Retooled.FSR.Tests;

public class FsrContextTests
{
    const float TimeoutSeconds = 60f;

    // The inputs that the tests pass: sRGB-encoded color, and depth that is larger where nearer with no far plane.
    const FsrContextFlags Flags =
        FsrContextFlags.DepthInverted | FsrContextFlags.DepthInfinite | FsrContextFlags.NonLinearColorSpace;

    static readonly Vector2Int Size = new(640, 360);

    static bool ExpectFsr =>
        SystemInfo.graphicsDeviceType is GraphicsDeviceType.Direct3D12 or GraphicsDeviceType.Direct3D11;

    [Test]
    public void StructsMatchTheNativeLayout()
    {
        Assert.That(Marshal.SizeOf<FsrNative.Options>(), Is.EqualTo(5 * sizeof(int)));
        Assert.That(Marshal.SizeOf<FsrNative.Frame>(), Is.EqualTo(4 * 8 + 15 * sizeof(int) + 4));
        Assert.That(Marshal.SizeOf<FsrNative.Status>(), Is.EqualTo(4 * sizeof(int) + sizeof(ulong) + 32));
    }

    [Test]
    public void ThePluginSeesUnitysGraphicsApi()
    {
        RequirePlugin();
        Assert.That(FsrContext.Renderer, Is.EqualTo(SystemInfo.graphicsDeviceType));
    }

    // FSR runs on any GPU under Direct3D 12 or 11. Anywhere else the context has to fail with a reason.
    [UnityTest]
    public IEnumerator CreatesAContextOrReportsWhyNot()
    {
        RequirePlugin();
        using var context = new FsrContext(Size, Size, Flags);
        yield return WaitWhile(() => context.Status.Stage == FsrStage.Pending);

        FsrStatus status = context.Status;
        RequireRuntime(status);
        if (ExpectFsr)
        {
            Assert.That(status.Stage, Is.EqualTo(FsrStage.Ready), status.ToString());
            Assert.That(status.Version, Is.Not.Empty);
            Assert.That(context.Versions, Does.Contain(status.Version.TrimEnd(' ', '*')));
            Assert.That(status.VideoMemory, Is.GreaterThan(0));
        }
        else
        {
            Assert.That(status.Stage, Is.EqualTo(FsrStage.Failed), status.ToString());
            Assert.That(status.CreateResult, Is.EqualTo(FsrResult.ErrorUnsupportedGraphicsApi));
        }
    }

    // A flat image with no motion has one right answer, so the output has to match the input.
    [UnityTest]
    public IEnumerator NativeAaReproducesAFlatImage()
    {
        RequirePlugin();
        if (!ExpectFsr)
            Assert.Ignore("FSR needs Direct3D 12 or 11");

        const int frames = 8;
        var input = new Color(0.25f, 0.5f, 0.75f, 1f);
        RenderTexture color = NewTexture(RenderTextureFormat.ARGB32, input, randomWrite: false);
        RenderTexture depth = NewTexture(RenderTextureFormat.RFloat, new Color(0.5f, 0f, 0f, 0f), randomWrite: false);
        RenderTexture motion = NewTexture(RenderTextureFormat.RGHalf, Color.clear, randomWrite: false);
        RenderTexture output = NewTexture(RenderTextureFormat.ARGB32, Color.black, randomWrite: true);
        var commands = new CommandBuffer { name = "FSR test" };
        try
        {
            using var context = new FsrContext(Size, Size, Flags);
            yield return WaitWhile(() => context.Status.Stage == FsrStage.Pending);
            RequireRuntime(context.Status);
            Assert.That(context.Status.Stage, Is.EqualTo(FsrStage.Ready), context.Status.ToString());

            var frame = new FsrFrame
            {
                Color = color.GetNativeTexturePtr(),
                Depth = depth.GetNativeTexturePtr(),
                Motion = motion.GetNativeTexturePtr(),
                Output = output.GetNativeTexturePtr(),
                RenderSize = Size,
                OutputSize = Size,
                MotionScale = Vector2.one,
                CameraNear = 0.1f,
                CameraFar = float.MaxValue,
                CameraFieldOfView = 1f,
                FrameTime = 16.6f,
                Flags = FsrFrameFlags.NonLinearColorSrgb,
            };
            for (int i = 0; i < frames; i++)
            {
                // FSR does not accept a jitter of zero on both axes.
                frame.Jitter = new Vector2(i % 2 == 0 ? 0.25f : -0.25f, i % 3 == 0 ? 0.25f : -0.25f);
                frame.Reset = i == 0;
                commands.Clear();
                context.Evaluate(commands, frame);
                Graphics.ExecuteCommandBuffer(commands);
                yield return null;
            }
            yield return WaitWhile(() => context.Status.Evaluations < frames);

            FsrStatus status = context.Status;
            Assert.That(status.EvaluateResult, Is.EqualTo(FsrResult.Ok), status.ToString());

            Color result = ReadCenter(output);
            Assert.That(result.r, Is.EqualTo(input.r).Within(0.02f), result.ToString());
            Assert.That(result.g, Is.EqualTo(input.g).Within(0.02f), result.ToString());
            Assert.That(result.b, Is.EqualTo(input.b).Within(0.02f), result.ToString());
        }
        finally
        {
            commands.Release();
            foreach (RenderTexture texture in new[] { color, depth, motion, output })
            {
                texture.Release();
                Object.DestroyImmediate(texture);
            }
        }
    }

    // The plugin is only built for Windows.
    static void RequirePlugin()
    {
        if (Application.platform != RuntimePlatform.WindowsEditor)
            Assert.Ignore("FSR only runs on Windows");
    }

    // A checkout of this package may not include AMD's DLLs.
    static void RequireRuntime(FsrStatus status)
    {
        if (status.CreateResult == FsrResult.ErrorLibraryMissing)
            Assert.Ignore("amd_fidelityfx_loader_dx12.dll isn't in Plugins/x86_64");
    }

    static IEnumerator WaitWhile(System.Func<bool> waiting)
    {
        float deadline = Time.realtimeSinceStartup + TimeoutSeconds;
        while (waiting() && Time.realtimeSinceStartup < deadline)
            yield return null;
    }

    static RenderTexture NewTexture(RenderTextureFormat format, Color fill, bool randomWrite)
    {
        var texture = new RenderTexture(Size.x, Size.y, 0, format, RenderTextureReadWrite.Linear)
        {
            enableRandomWrite = randomWrite,
            hideFlags = HideFlags.HideAndDontSave,
        };
        texture.Create();
        RenderTexture previous = RenderTexture.active;
        RenderTexture.active = texture;
        GL.Clear(clearDepth: false, clearColor: true, fill);
        RenderTexture.active = previous;
        return texture;
    }

    static Color ReadCenter(RenderTexture texture)
    {
        var pixel = new Texture2D(1, 1, TextureFormat.RGBA32, mipChain: false, linear: true);
        RenderTexture previous = RenderTexture.active;
        RenderTexture.active = texture;
        pixel.ReadPixels(new Rect(Size.x / 2, Size.y / 2, 1, 1), 0, 0);
        RenderTexture.active = previous;
        Color result = pixel.GetPixel(0, 0);
        Object.DestroyImmediate(pixel);
        return result;
    }
}
