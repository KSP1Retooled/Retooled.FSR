# Builds RetooledFSR.dll and the test harness. Needs CMake, Ninja, LLVM's clang-cl and the Windows SDK.
#
#   build.ps1            builds the plugin and copies it into Plugins/x86_64
#   build.ps1 -Test      also runs the harness, which needs a GPU with Direct3D 12
#
# A Unity editor that has loaded the plugin keeps the DLL open, so close it before building.
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [switch]$Test
)

$ErrorActionPreference = 'Stop'
$native = $PSScriptRoot
$build = Join-Path $native "build/$Configuration"
$plugins = Join-Path $native '../Plugins/x86_64'

if (-not (Test-Path (Join-Path $native 'fidelityfx/upscalers/include/ffx_upscale.h'))) {
    throw "The FSR SDK headers are missing from $native/fidelityfx. See the README for the files to copy there."
}

# CMake also needs LLVM's resource compiler and manifest tool, so the whole folder goes on PATH.
if (-not (Get-Command clang-cl -ErrorAction SilentlyContinue)) {
    $llvm = 'C:\Program Files\LLVM\bin'
    if (-not (Test-Path (Join-Path $llvm 'clang-cl.exe'))) { throw "clang-cl was not found on PATH or in $llvm" }
    $env:PATH = "$llvm;$env:PATH"
}

cmake -S $native -B $build -G Ninja `
    "-DCMAKE_BUILD_TYPE=$Configuration" `
    -DCMAKE_CXX_COMPILER=clang-cl `
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }

cmake --build $build
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }

if ($Test) {
    # The harness loads FSR from its own folder, so it runs from a folder that holds the SDK's DLLs.
    $run = Join-Path $build 'harness'
    New-Item -ItemType Directory -Force $run | Out-Null
    Copy-Item (Join-Path $build 'FsrHarness.exe') $run -Force
    foreach ($name in 'amd_fidelityfx_loader_dx12.dll', 'amd_fidelityfx_upscaler_dx12.dll') {
        Copy-Item (Join-Path $plugins $name) $run -Force
    }
    & (Join-Path $run 'FsrHarness.exe')
    if ($LASTEXITCODE -ne 0) { throw 'The harness failed' }
}

Copy-Item (Join-Path $build 'RetooledFSR.dll') $plugins -Force
Write-Host "Copied RetooledFSR.dll to $(Resolve-Path $plugins)"
