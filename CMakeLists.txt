cmake_minimum_required(VERSION 3.24)

project(Backtrack VERSION 0.1.0 LANGUAGES C CXX RC)

# Vendored RNNoise (BSD-3, see backtrack/src/third_party/rnnoise/README.md),
# used by the optional microphone noise suppression feature. Off by default;
# when enabled it compiles as a plain C11 static library (generic scalar
# kernels are fast enough for one mono stream, ~1% of one core).
option(BACKTRACK_NOISE_SUPPRESSION "Include RNNoise microphone noise suppression" OFF)

file(GLOB_RECURSE BACKTRACK_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/backtrack/src/*.cpp")
if (NOT BACKTRACK_NOISE_SUPPRESSION)
    list(FILTER BACKTRACK_SOURCES EXCLUDE REGEX "RnnoiseDenoiser\\.cpp$")
endif()

if (BACKTRACK_NOISE_SUPPRESSION)
    file(GLOB BACKTRACK_RNNOISE_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/backtrack/src/third_party/rnnoise/src/*.c")
    add_library(rnnoise STATIC ${BACKTRACK_RNNOISE_SOURCES})
    target_compile_features(rnnoise PRIVATE c_std_11)
    target_include_directories(rnnoise PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/backtrack/src/third_party/rnnoise/include")
    target_include_directories(rnnoise PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/backtrack/src/third_party/rnnoise/src")
    if (MSVC)
        # Third-party C code: keep its own (benign double->float) warnings out of
        # the app's warning budget.
        target_compile_options(rnnoise PRIVATE /W3)
    endif()
endif()

set(BACKTRACK_RESOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/backtrack/src/backtrack.rc")

add_executable(Backtrack WIN32 ${BACKTRACK_SOURCES} ${BACKTRACK_RESOURCES})

target_compile_features(Backtrack PRIVATE cxx_std_20)
# backtrack.vcxproj compiles these sources at Level3. MSVC's default is /W1, so
# without this every /W2+ diagnostic is invisible in the build that runs ctest.
target_compile_options(Backtrack PRIVATE /W3)
# /wd4996, and only 4996. The SDK's sphelper.h calls the deprecated
# GetVersionExW on our behalf from voice/VoiceCommandService.cpp; we neither own
# that header nor can drop it without losing ISpVoice. backtrack.vcxproj
# disables the same single diagnostic so the two definitions stay equivalent in
# effect. Every other warning keeps the full budget.
target_compile_options(Backtrack PRIVATE /wd4996)
target_include_directories(Backtrack PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/backtrack/src")
target_compile_definitions(Backtrack PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN)
if (BACKTRACK_NOISE_SUPPRESSION)
    target_compile_definitions(Backtrack PRIVATE BACKTRACK_HAS_NOISE_SUPPRESSION=1)
    target_link_libraries(Backtrack PRIVATE rnnoise)
endif()

find_path(NVENC_INCLUDE_DIR    NAMES nvEncodeAPI.h
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
    d3dcompiler
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
    version
    sapi
    psapi)

if (MSVC AND DEFINED CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION AND DEFINED ENV{WindowsSdkDir})
    target_include_directories(Backtrack PRIVATE
        "$ENV{WindowsSdkDir}/Include/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/cppwinrt")
endif()

# --- Unit tests ---
# Plain console executables with main(); register them with CTest so a bare
# `ctest` in the build directory compiles and runs every suite.
enable_testing()

function(add_backtrack_test name)
    cmake_parse_arguments(TEST "" "" "SOURCES;LIBS" ${ARGN})
    add_executable(${name} ${TEST_SOURCES})
    target_compile_features(${name} PRIVATE cxx_std_20)
    # Same budget as the app target and backtrack.vcxproj: the suites compile the
    # production sources, so a /W1 build here hides exactly what the gate must catch.
    target_compile_options(${name} PRIVATE /W3)
    target_include_directories(${name} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/backtrack/src")
    # Shared sources (Logger, ReplayBuffer) use unparenthesized std::min/std::max;
    # without this the Windows.h macros break them.
    target_compile_definitions(${name} PRIVATE NOMINMAX)
    if (TEST_LIBS)
        target_link_libraries(${name} PRIVATE ${TEST_LIBS})
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# VideoTimelineScheduler is header-only: the test alone is sufficient.
add_backtrack_test(VideoTimelineSchedulerTests
    SOURCES backtrack/tests/VideoTimelineSchedulerTests.cpp)

# DecodeOrderTimestamps is header-only too, but it logs through core/Logger.cpp
# when its desync watchdog fires, so that is the one source it needs.
add_backtrack_test(DecodeOrderTimestampsTests
    SOURCES
        backtrack/tests/DecodeOrderTimestampsTests.cpp
        backtrack/src/core/Logger.cpp)

# SpscQueue is header-only with no project includes at all.
add_backtrack_test(SpscQueueTests
    SOURCES backtrack/tests/SpscQueueTests.cpp)

# AudioConvertTests exercises audio/AudioConvert.cpp, which logs via core/Logger.cpp.
add_backtrack_test(AudioConvertTests
    SOURCES
        backtrack/tests/AudioConvertTests.cpp
        backtrack/src/audio/AudioConvert.cpp
        backtrack/src/core/Logger.cpp)

# SessionMuteStoreTests exercises audio/SessionMuteStore.cpp without WASAPI:
# the journal codec, write-after-every-mute, stale pruning, the orphan sweep
# (including the still-muted check and the rewrite of unreached entries), and
# the try_lock-only crash path. Logger.cpp provides the UTF-8 codec.
add_backtrack_test(SessionMuteStoreTests
    SOURCES
        backtrack/tests/SessionMuteStoreTests.cpp
        backtrack/src/audio/SessionMuteStore.cpp
        backtrack/src/core/Logger.cpp)

# ReplayBufferTests stubs Mp4Muxer::muxToMp4 and removeEmptyOutputReservation
# at link time (defined inside the test source) and links everything around
# them: the real replay buffer, WAV reader/writer, recovery store, converter,
# bitstream header helpers, and logger. Mp4Muxer.cpp itself stays out - it
# would pull Media Foundation.
add_backtrack_test(ReplayBufferTests
    SOURCES
        backtrack/tests/ReplayBufferTests.cpp
        backtrack/src/replay/ReplayBuffer.cpp
        backtrack/src/mux/RecoveryStore.cpp
        backtrack/src/audio/AudioConvert.cpp
        backtrack/src/audio/WavReader.cpp
        backtrack/src/mux/WavWriter.cpp
        backtrack/src/encoder/BitstreamUtils.cpp
        backtrack/src/core/Logger.cpp)

# SettingsClipGuardTests covers the two functions that stand between a bad
# settings file and a broken encoder config, and a malformed path and a deleted
# user file. The parse path is the other half of that boundary: every key
# falling back, the comment and repetition rules, the legacy key spellings, and
# a full save()/load() round trip. ClipManager's path gate is private, so it is
# exercised through the destructive operations it guards. SettingsStore.cpp only
# needs defaultClipDirectory() from platform/Win32Util.cpp, which the test stubs
# to keep Win32Util (and its DXGI/avrt/winmm deps) out of the suite. propsys is
# the only non-default link library, for the shell property reads ClipManager
# does.
add_backtrack_test(SettingsClipGuardTests
    SOURCES
        backtrack/tests/SettingsClipGuardTests.cpp
        backtrack/src/settings/SettingsStore.cpp
        backtrack/src/clips/ClipManager.cpp
        backtrack/src/core/Logger.cpp
    LIBS propsys)

if (BACKTRACK_NOISE_SUPPRESSION)
    # AudioDenoiseTests exercises audio/RnnoiseDenoiser.cpp against the vendored
    # rnnoise static library.
    add_backtrack_test(AudioDenoiseTests
        SOURCES
            backtrack/tests/AudioDenoiseTests.cpp
            backtrack/src/audio/RnnoiseDenoiser.cpp
        LIBS rnnoise)
endif()

