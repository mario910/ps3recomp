# Optional source-pinned shader dependencies; no runtime executables/downloads.
# SPIRV-Tools/Headers match glslang 15.1.0's own known_good.json.
include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
set(SPIRV_TOOLS_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(SPIRV_HEADERS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL ON CACHE BOOL "" FORCE)
set(ENABLE_OPT ON CACHE BOOL "" FORCE)
FetchContent_Declare(ps3_vk_headers
    URL https://codeload.github.com/KhronosGroup/Vulkan-Headers/tar.gz/952f776f6573aafbb62ea717d871cd1d6816c387
    URL_HASH SHA256=4c744e80c908bcc65c2057482f44e57d7e3d748f6eae4cd759fd593018f4ce35
    SOURCE_SUBDIR header-only)
FetchContent_Declare(ps3_spirv_headers
    URL https://codeload.github.com/KhronosGroup/SPIRV-Headers/tar.gz/3f17b2af6784bfa2c5aa5dbb8e0e74a607dd8b3b
    URL_HASH SHA256=2301e11e5c77213258d6863bf4e6c607a8c6431fa8336e98ac6a2131bd6284f8
    SOURCE_SUBDIR header-only)
FetchContent_Declare(ps3_spirv_tools
    URL https://codeload.github.com/KhronosGroup/SPIRV-Tools/tar.gz/4d2f0b40bfe290dea6c6904dafdf7fd8328ba346
    URL_HASH SHA256=41481a45441d92b2404aa06bdecbb0302f22636335be4e19023632c83fa89aa1)
FetchContent_Declare(ps3_glslang
    URL https://codeload.github.com/KhronosGroup/glslang/tar.gz/1062752a891c95b2bfeed9e356562d88f9df84ac
    URL_HASH SHA256=46a05bba7675a3ab1912a0908183b9eee8daa06bb8047943bac54a6f0de2cfce)
FetchContent_MakeAvailable(ps3_vk_headers ps3_spirv_headers)
set(SPIRV-Headers_SOURCE_DIR "${ps3_spirv_headers_SOURCE_DIR}")
FetchContent_MakeAvailable(ps3_spirv_tools ps3_glslang)
set(PS3RECOMP_VULKAN_INCLUDE_DIR "${ps3_vk_headers_SOURCE_DIR}/include" CACHE PATH "Pinned Vulkan headers" FORCE)
if(NOT TARGET glslang::glslang OR NOT TARGET glslang::SPIRV OR NOT TARGET SPIRV-Tools-opt)
    message(FATAL_ERROR "Pinned Vulkan shader dependencies did not provide required targets")
endif()
