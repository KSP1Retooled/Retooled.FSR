using System;
using UnityEngine;
using UnityEngine.Rendering;

namespace Retooled.FSR;

/// <summary>Has the plugin free the textures of disposed contexts once the GPU no longer uses them.</summary>
/// <remarks>
/// FSR frees a context's textures as soon as the context is destroyed, but the GPU may still be running that
/// context's last frames. So the plugin keeps a disposed context until Unity's frame fence shows that those frames
/// are done. It checks the fence when it gets a collect event, which this class sends once a frame for a few frames
/// after each disposal. A context that is still in use after that is freed with the graphics device.
/// </remarks>
internal static class FsrCollector
{
    const int FramesAfterDispose = 16;

    static CommandBuffer commands;

    static int remaining;

    internal static void Start()
    {
        bool collecting = remaining > 0;
        remaining = FramesAfterDispose;
        if (collecting)
            return;

        commands ??= new CommandBuffer { name = "FSR collect" };
        commands.Clear();
        commands.IssuePluginEventAndData(FsrContext.EventFunction, (int)FsrNative.Event.Collect, IntPtr.Zero);
        Application.onBeforeRender += Collect;
    }

    static void Collect()
    {
        Graphics.ExecuteCommandBuffer(commands);
        if (--remaining == 0)
            Application.onBeforeRender -= Collect;
    }
}
