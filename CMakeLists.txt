cmake_minimum_required(VERSION 3.24)

project(Backtrack VERSION 0.1.0 LANGUAGES CXX RC)

file(GLOB_RECURSE BACKTRACK_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/backtrack/src/*.cpp")
# Hook DLL has its own target; do not link into the host exe.
list(FILTER BACKTRACK_SOURCES EXCLUDE REGEX "GameCaptureHook\\.cpp$")
list(FILTER BACKTRACK_SOURCES EXCLUDE REGEX "GameCaptureGl\\.cpp$")

set(BACKTRACK_RESOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/backtrack/src/backtrack.rc")

add_executable(Backtrack WIN32 ${BACKTRACK_SOURCES} ${BACKTRACK_RESOURCES})

# Microsoft Detours (trampoline hooking) for the injected capture DLL.
# Detours ships with an nmake build, not CMake, so fetch the source and wrap
# the small set of translation units in a static library. The disolx*.cpp
# disassembler backends are #included by disasm.cpp, not compiled directly.
include(FetchContent)
FetchContent_Declare(detours
    GIT_REPOSITORY https://github.com/microsoft/Detours.git
    GIT_TAG 4b8c659f549b0ab21cf649377c7a84eb708f5e68) # v4.0.1
FetchContent_MakeAvailable(detours)
add_library(detours STATIC
    "${detours_SOURCE_DIR}/src/detours.cpp"
    "${detours_SOURCE_DIR}/src/disasm.cpp"
    "${detours_SOURCE_DIR}/src/image.cpp"
    "${detours_SOURCE_DIR}/src/modules.cpp"
    "${detours_SOURCE_DIR}/src/creatwth.cpp")
target_include_directories(detours PUBLIC "${detours_SOURCE_DIR}/src")
if (MSVC)
    # Detours source is not warning-clean under /W4; keep it quiet.
    target_compile_options(detours PRIVATE /w)
endif()

add_library(BacktrackGameCapture SHARED
    "${CMAKE_CURRENT_SOURCE_DIR}/backtrack/src/capture/game/GameCaptureHook.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/backtrack/src/capture/game/GameCaptureGl.cpp")
target_compile_features(BacktrackGameCapture PRIVATE cxx_std_20)
target_include_directories(BacktrackGameCapture PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/backtrack/src")
target_compile_definitions(BacktrackGameCapture PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN)
target_link_libraries(BacktrackGameCapture PRIVATE d3d11 d3d12 dxgi opengl32 psapi detours)
# MSVC Release defaults include /O2. Enable IPO for this target so CMake adds
# /GL to compilation and /LTCG at link time without affecting Debug builds.
if (MSVC)
    set_property(TARGET BacktrackGameCapture PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()
set_target_properties(BacktrackGameCapture PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/RelWithDebInfo"
    OUTPUT_NAME "BacktrackGameCapture")
add_dependencies(Backtrack BacktrackGameCapture)

target_compile_features(Backtrack PRIVATE cxx_std_20)
target_include_directories(Backtrack PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/backtrack/src")
target_compile_definitions(Backtrack PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN)

find_path(NVENC_INCLUDE_DIR
    NAMES nvEncodeAPI.h
    PATHS
        "${CMAKE_CURRENT_SOURCE_DIR}/backtrack/src/Interface"
        "$ENV{NV_CODEC_SDK_PATH}/Interface"
        "$ENV{NV_CODEC_SDK}/Interface"
        "$ENV{ProgramFiles}/NVIDIA Corporation/NVIDIA Video Codec SDK/Interface")

if (NVENC_INCLUDE_DIR)
    target_include_directories(Backtrack PRIVATE "${NVENC_INCLUDE_DIR}")
    target_compile_definitions(Backtrack PRIVATE BACKTRACK_HAS_NVENC=1)
    message(STATUS "NVENC headers found: ${NVENC_INCLUDE_DIR}")
else()
    target_compile_definitions(Backtrack PRIVATE BACKTRACK_HAS_NVENC=0)
    message(WARNING "nvEncodeAPI.h was not found. Backtrack will build, but recording requires the NVIDIA Video Codec SDK headers.")
endif()

# AMD AMF is loaded at runtime through amfrt64.dll; only the public headers are
# needed at compile time. They are bundled under backtrack/src/Interface/AMF but
# an external AMF SDK can be pointed at via AMF_SDK_PATH.
find_path(AMF_INCLUDE_DIR
    NAMES core/Factory.h
    PATHS
        "${CMAKE_CURRENT_SOURCE_DIR}/backtrack/src/Interface/AMF"
        "$ENV{AMF_SDK_PATH}/amf/public/include"
        "$ENV{AMF_SDK_PATH}/include")

if (AMF_INCLUDE_DIR)
    # The bundled headers use "AMF/..." include prefixes, so expose the parent of
    # the AMF directory. src/Interface is already on the include path for the
    # bundled copy; add the external parent when a custom SDK path is used.
    get_filename_component(AMF_INCLUDE_PARENT "${AMF_INCLUDE_DIR}" DIRECTORY)
    target_include_directories(Backtrack PRIVATE "${AMF_INCLUDE_PARENT}")
    target_compile_definitions(Backtrack PRIVATE BACKTRACK_HAS_AMF=1)
    message(STATUS "AMF headers found: ${AMF_INCLUDE_DIR}")
else()
    target_compile_definitions(Backtrack PRIVATE BACKTRACK_HAS_AMF=0)
    message(WARNING "AMF headers were not found. Backtrack will build, but AMD hardware encoding requires the AMF SDK headers.")
endif()

target_link_libraries(Backtrack PRIVATE
    d3d11
    dxgi
    dxguid
    mfplat
    mfreadwrite
    mfuuid
    windowsapp
    dwmapi
    uxtheme
    ole32
    advapi32
    avrt
    mmdevapi
    comctl32
    shell32
    shlwapi
    propsys
    user32
    gdi32
    uuid
    winmm
    winhttp
    sapi
    psapi)

if (MSVC AND DEFINED CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION AND DEFINED ENV{WindowsSdkDir})
    target_include_directories(Backtrack PRIVATE
        "$ENV{WindowsSdkDir}/Include/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/cppwinrt")
endif()
