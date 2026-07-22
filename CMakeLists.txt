cmake_minimum_required(VERSION 3.24)

project(Backtrack VERSION 0.1.0 LANGUAGES CXX RC)

file(GLOB_RECURSE BACKTRACK_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/backtrack/src/*.cpp")

set(BACKTRACK_RESOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/backtrack/src/backtrack.rc")

add_executable(Backtrack WIN32 ${BACKTRACK_SOURCES} ${BACKTRACK_RESOURCES})

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
    winmm)

if (MSVC AND DEFINED CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION AND DEFINED ENV{WindowsSdkDir})
    target_include_directories(Backtrack PRIVATE
        "$ENV{WindowsSdkDir}/Include/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/cppwinrt")
endif()
