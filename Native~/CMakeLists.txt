cmake_minimum_required(VERSION 3.24)
project(RetooledFSR LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# Only the Vulkan headers are used. Unity supplies the loader at runtime. They come from the Vulkan SDK, or from a
# checkout of KhronosGroup/Vulkan-Headers whose include folder is passed as Vulkan_INCLUDE_DIR.
find_path(Vulkan_INCLUDE_DIR vulkan/vulkan.h HINTS "$ENV{VULKAN_SDK}/Include" REQUIRED)

# The FSR DLLs are loaded at runtime, so only the SDK's headers are needed here. The headers include each other by
# relative path, so the fidelityfx folder keeps the SDK's layout.
add_library(FsrCore STATIC src/Fsr.cpp src/FsrVulkan.cpp src/Sidecar.cpp)
target_include_directories(FsrCore PUBLIC src PRIVATE fidelityfx ${Vulkan_INCLUDE_DIR})
target_compile_definitions(FsrCore PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX VK_NO_PROTOTYPES)

add_library(RetooledFSR SHARED src/Plugin.cpp)
target_include_directories(RetooledFSR PRIVATE unity ${Vulkan_INCLUDE_DIR})
target_compile_definitions(RetooledFSR PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX VK_NO_PROTOTYPES)
target_link_libraries(RetooledFSR PRIVATE FsrCore)
set_target_properties(RetooledFSR PROPERTIES OUTPUT_NAME Retooled.FSR.Native)

# Runs the same code on its own device, without Unity. See test/Harness.cpp.
add_executable(FsrHarness test/Harness.cpp)
target_compile_definitions(FsrHarness PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(FsrHarness PRIVATE FsrCore d3d11 d3d12 dxgi)
