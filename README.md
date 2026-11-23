# FSR

AMD FSR upscaling and native anti-aliasing for Unity's built-in render pipeline on Windows x64.

The package calls FSR through AMD's FSR API, from the FSR SDK 2.4.0. FSR records its work with the graphics API's
own objects, which C# can't reach, so the package includes a small native plugin, `RetooledFSR.dll`, that Unity calls
on the render thread. The plugin loads AMD's `amd_fidelityfx_loader_dx12.dll` from its own folder and makes every SDK
call there. `FsrContext` is the C# side.

The FSR API picks the FSR version for the GPU. Recent AMD GPUs get FSR 4, which uses machine learning. Every other
GPU gets FSR 3.1. `context.Status.Version` says which one is running. A newer AMD graphics driver can also replace
the version in this package with its own.

## Requirements

- Direct3D 12, on a Unity version that has the `IUnityGraphicsD3D12v8` plugin interface. Any GPU vendor works.
- Vulkan is not supported yet. The SDK has Vulkan DLLs, but the plugin doesn't use them. FSR doesn't run on
  Direct3D 11, OpenGL, Linux or macOS.

On anything else a context fails, and `context.Status` says why.

## Use

```csharp
// Native anti-aliasing: the render size equals the output size.
var flags = FsrContextFlags.DepthInverted | FsrContextFlags.DepthInfinite;
var context = new FsrContext(size, size, flags);

// Each frame, once context.Status.Stage is Ready:
commands.Clear();
context.Evaluate(commands, frame);

// When done:
context.Dispose();
```

Calls are queued to the render thread and return at once. `context.Status` shows how far the render thread has got.
`Evaluate` records one frame into a command buffer. The recorded commands hold that frame's inputs, so record them
again every frame.

To upscale, pass a render size below the output size. AMD's quality modes are render sizes of 1/1.5, 1/1.7, 1/2 and
1/3 of the output size on each axis.

### Inputs

| Input | What FSR expects |
| --- | --- |
| Color | The jittered image. Linear values by default. For sRGB-encoded values, set `FsrContextFlags.NonLinearColorSpace` on the context and `FsrFrameFlags.NonLinearColorSrgb` on each frame. Set `FsrContextFlags.HighDynamicRange` if values exceed one. |
| Depth | A one-channel float texture. Set `FsrContextFlags.DepthInverted` if nearer surfaces have larger values, and `FsrContextFlags.DepthInfinite` if the projection has no far plane. |
| Camera | The near plane, far plane and vertical field of view that the depth was rendered with. FSR turns depth back into distances with them. Pass `float.MaxValue` as an infinitely far plane. |
| Motion | Two-channel float vectors. `FsrFrame.MotionScale` turns them into the offset from a pixel to its previous position, as a fraction of the image. Unity's motion vectors need a scale of (-1, -1). |
| Jitter | The distance that the projection jitter moved the image, in render pixels, between -0.5 and 0.5, in a Halton sequence. AMD asks for 8 samples at native resolution and `ceil(8 * scale²)` when upscaling. The two components must never both be zero. |
| Output | A texture with `enableRandomWrite`. |

Pixel coordinates have their origin at the top left of the texture. A Unity render texture's rows run the same way as
its UV coordinates, so Unity's jitter and motion vectors need no vertical flip.

FSR 3.1 can also use a reactive mask and a transparency mask to reduce ghosting behind particles. The package doesn't
pass them yet.

## How it works with Unity

- The plugin asks Unity to move the input textures into the shader-resource state and the output into the
  unordered-access state, so Unity adds the barriers and keeps tracking the states. FSR binds its own descriptor
  heaps and pipeline, and Unity restores its own state after the plugin event.
- FSR frees a context's textures as soon as the context is destroyed, even if the GPU is still running that
  context's last frames. So the plugin keeps a disposed context until Unity's frame fence shows that those frames are
  done, and frees it then.
- The FSR DLLs load when the first context is created. A game that never creates a context never loads them.
- Unity 6000.7 ships its own, older `amd_fidelityfx_loader_dx12.dll` and `amd_fidelityfx_upscaler_dx12.dll` next to
  the editor and player executables. AMD's loader looks next to the executable before it looks in its own folder,
  so it would pair this package's loader with Unity's upscaler. The plugin loads this package's upscaler DLL first,
  and the loader then uses that one. If Unity's own FSR support has already loaded its upscaler DLL, the loader uses
  Unity's.

## Debugging

- The FSR API's errors and warnings go to the Unity log with an `[FSR]` prefix. `FsrContext.LogLevel` sets which.
- `FsrContextFlags.DebugChecking` makes FSR check every frame's inputs and warn about problems, such as a frame time
  that is not in milliseconds. It always warns about a missing exposure texture when automatic exposure is off.
- `FsrContextFlags.DebugVisualization` with `FsrFrameFlags.DrawDebugView` draws FSR's motion vectors and other
  internal images over the output.

## Building the plugin

Needs CMake, Ninja, LLVM's `clang-cl` and the Windows SDK. The plugin needs four headers from the SDK's
`Kits/FidelityFX` folder, at the same paths under `Native~/fidelityfx`:

- `api/include/ffx_api.h`
- `api/include/ffx_api_types.h`
- `api/include/dx12/ffx_api_dx12.h`
- `upscalers/include/ffx_upscale.h`

```powershell
Native~/build.ps1 -Test
```

The script copies `RetooledFSR.dll` into `Plugins/x86_64`. Close the Unity editor first if it has loaded the plugin,
because it keeps the DLL open.

`-Test` also runs `FsrHarness.exe`, which evaluates FSR at native resolution on a Direct3D 12 device of its own,
without Unity.

## Licenses

- `amd_fidelityfx_loader_dx12.dll` and `amd_fidelityfx_upscaler_dx12.dll` in `Plugins/x86_64` are AMD's signed
  builds from the SDK's `Kits/FidelityFX/signedbin` folder. They and the headers in `Native~/fidelityfx` are under
  the MIT license in `Native~/fidelityfx/license.md`. AMD requires applications to call the FSR API through its
  signed DLLs.
- The headers in `Native~/unity` are Unity's, under the Unity Companion License in `Native~/unity/LICENSE.md`.
