# tests

## Purpose

Unit suites for the parts of the app that can be exercised without a capture
device or a GPU of a known vendor. Everything registered here runs on every
build.

## Ownership

- Owns: the test sources and how they are registered with CTest.
- Does not own: the code under test. A failing test points into the domain
  folder's AGENTS.md for the contract it broke.

## Local Contracts

- Plain console executables with `main()`. No test framework, no fixtures, no
  mocks library, and no shared helper file: every suite carries its own
  `expect(condition, message)` that throws, its cases are `testXxx()` free
  functions in an anonymous namespace called in order from `main()`, and
  `main()` catches `std::exception`, prints the failure, and returns 1.
- One suite is one CTest case, so a new case is a new `testXxx()` call in
  `main()`, not a new registration.
- Suites are registered in the root `CMakeLists.txt` through
  `add_backtrack_test(name SOURCES ... [LIBS ...] [DEFINES ...] [INCLUDES ...])`,
  which sets C++20, adds `src` to the include path, defines `NOMINMAX`, and adds
  the CTest entry. `NOMINMAX` is load-bearing: the linked sources call
  unparenthesized `std::min`/`std::max` (`core/Logger.cpp`,
  `replay/ReplayBuffer.cpp`, `audio/WavReader.cpp`) and `Windows.h` turns those
  into macros.
- `DEFINES` exists for the compile-time backend switches: a suite that pins a
  backend passes `BACKTRACK_HAS_NVENC=0|1` or `BACKTRACK_HAS_AMF=1` itself,
  because the test targets do not inherit the app's header detection. `INCLUDES`
  adds the vendor SDK header path for the two lifecycle suites.
- Nothing links the whole app. A suite lists the implementation files it needs,
  so a new source file under test goes into that suite's `SOURCES` in the same
  change or the link fails. The UI suites are the exception: they link the
  `BacktrackApp` object library (every app source including `main.cpp`, whose
  `wWinMain` is inert in a console binary) because a hand-listed subset of the
  window code is not maintainable. Object library, not static: every unit has to
  reach the linker, and the tests only touch a few entry points.
- Current suites and what they cover:
  - `VideoTimelineSchedulerTests` - `app/VideoTimelineScheduler.h` alone, so the
    header must stay free of Windows.h and project includes for the suite to
    compile as a single file. Cadence collapse, coalescing, and duration math
    including the `maxCoalescedDuration100ns` cap.
  - `DecodeOrderTimestampsTests` - `encoder/DecodeOrderTimestamps.h` plus
    `core/Logger.cpp`. DTS equals PTS with no reordering, the offset settles at
    the pipeline depth under B-frames, ctts offsets stay non-negative, a batched
    harvest does not lose a slot, and `reset` with the full depth
    (`bFrames ? 2 : 0` + `lookahead ? depth : 0`) is contrasted with the
    too-small depth `encoder/AGENTS.md` warns about.
  - `SpscQueueTests` - `core/SpscQueue.h` alone. FIFO order, `tryPush` failing
    on a full queue without disturbing the head, `size()` hand-computed across
    a wrap and never above capacity, `clear()`, and `resetCapacity` including
    the zero-request clamp to 1.
  - `HdrStabilityTests` - `capture/HdrStability.h` alone, so the header must
    stay free of project includes. The session-pin ratchet, the sRGB bridge
    transfer, the rejection backoff schedule, and the HDR format predicate.
  - `AudioConvertTests` - `audio/AudioConvert.cpp`, `core/Logger.cpp`:
    canonical conversion (passthrough, resample, mono duplication, 5.1 downmix,
    rejection of an unsupported format), the `applyMuteGain` primitive, and that
    a mid-stream format change still leaves every packet on one uniform format.
  - `SessionMuteStoreTests` - `audio/SessionMuteStore.cpp`, `core/Logger.cpp`.
    The journal codec, write-after-every-mute with atomic publish, stale
    pruning, the orphan sweep, and the try_lock-only crash path.
  - `ReplayBufferTests` - `replay/ReplayBuffer.cpp`, `mux/RecoveryStore.cpp`,
    `audio/AudioConvert.cpp`, `audio/WavReader.cpp`, `mux/WavWriter.cpp`,
    `encoder/BitstreamUtils.cpp`, `core/Logger.cpp`. Both backends, trimming and
    wrap reassembly, the `RecoveryStore` manifest rules, the audio gap fill
    keeping the real outage duration, the unpatched-WAV fallback, the bitstream
    classifiers, and the mux- and ring-read-failure save paths. `Mp4Muxer.cpp`
    stays out (see the stub rule below).
  - `RingFileTests` - `replay/RingFile.h` plus `core/Logger.cpp`: wrap and
    partial reads, immediate and completed errors, a real pending pipe read, and
    injected delayed completion after close.
  - `SettingsClipGuardTests` - `settings/SettingsStore.cpp`,
    `clips/ClipManager.cpp`, `core/Logger.cpp`, `LIBS propsys`: `sanitizeSettings`
    and `load` fallbacks, `save` on observable disk state, the managed-path
    gate, and `listClips` root discovery.
  - `SettingsStoreTests` - `settings/SettingsStore.cpp`, `core/Logger.cpp`:
    hard/symbolic linked settings files, staging-shaped destinations, load
    statuses, chunked parsing, failed-save retry, and app-name injection.
  - `ClipManagerTests` - `clips/ClipManager.cpp`, `core/Logger.cpp`,
    `LIBS propsys`: linked sidecars, failed tag publication, a rename race, the
    supported clip roots, and scan cancellation.
  - `AudioPersistenceTests` - `audio/AudioMix.cpp`, `audio/WavReader.cpp`,
    `mux/WavWriter.cpp`, `core/Logger.cpp`: the mix source policy (read-denied
    and locked WAVs fail, absent or empty tracks stay video-only), the full-gap
    fill, and writes past the 4 GiB RIFF boundary.
  - `Mp4MuxerTests` - `mux/Mp4Muxer.cpp`, `mux/RecoveryStore.cpp`,
    `mux/WavReader.cpp`, `mux/WavWriter.cpp`, `audio/WavReader.cpp`,
    `audio/AudioConvert.cpp`, `audio/AudioMix.cpp`, `core/Logger.cpp`,
    `LIBS mfplat mfreadwrite mfuuid ole32 shlwapi`. The only Media Foundation
    link: write-failure retention, abort, same-name retry, and `parseMuxWav`'s
    RIFF-boundary extent recovery.
  - `StorageLayoutTests` - the `ReplayBufferTests` source set plus
    `clips/ClipManager.cpp`, `LIBS propsys`: reservation collisions, the shared
    root rules, game-root and nested recovery, and that recovery muxes an owned
    work copy so the original session survives.
  - `LoggerDiagnosticsTests` - `core/Logger.cpp`, `ui/DiagnosticsLog.cpp`:
    rate limiting, pruning, stream failure, and diagnostics export.
  - `PendingDurationTrackerTests` - `app/PendingDurationTracker.h` alone.
  - `RecorderControllerCutoffTests`, `RecorderControllerLeaseTests`,
    `RecorderControllerShutdownOrderingTests` - header-inline controller
    helpers only (`RecordingCutoffTracker.h`, `CaptureFrameHandoff.h`,
    `AudioDrain.h`, `pipelineEncoderFaulted`, `EncoderFaultBackoff`), no
    controller sources.
  - `DesktopDuplicationRotationTests` - `capture/DesktopDuplicationCapture.h`'s
    header-inline rotation helpers and cursor carry, no D3D device.
  - `WgcCopyPoolTests` - `capture/WgcCopyPool.h`, `core/Logger.cpp`,
    `LIBS d3d11`: fallback resize on a WARP device.
  - `D3D11FrameScalerTests` - `capture/D3D11FrameScaler.cpp`,
    `core/Logger.cpp`, `LIBS d3d11`: the copied-input fallback and NV12 failure
    handling on WARP plus a fake video device.
  - `EncoderFeedingPoolTests` - `capture/GpuTexturePool.h` with a fake
    deep-lookahead encoder, `core/Logger.cpp`.
  - `WasapiDefaultDeviceTests` - `audio/WasapiCapture.cpp`,
    `audio/SessionMuteStore.cpp`, `core/Logger.cpp`,
    `LIBS avrt mmdevapi ole32 propsys user32 uuid windowsapp`: default-device
    switches with a fake enumerator, a pinned explicit device, switches during
    resolution and inside the packet callback, and registration/stop races.
  - `DiscordRichPresenceTests` and `LeagueOfLegendsIntegrationTests` - each
    includes its worker `.cpp` directly (to replace the pipe or the HTTP GET), so
    it must not be linked again; `core/Logger.cpp` plus `winhttp` for LoL.
  - `WinHttpJsonTests` - `integrations/WinHttpJson.h` (header-only),
    `core/Logger.cpp`, `LIBS winhttp ws2_32`: response cap, total deadline that
    progress does not reset, cancellation, and callback buffer ownership.
  - `NvencStubTests` - `encoder/NvencEncoder.cpp`, `core/Logger.cpp`,
    `DEFINES BACKTRACK_HAS_NVENC=0`: the unavailable-backend stub.
  - `NvencLifecycleTests` - includes `NvencEncoder.cpp` itself, plus
    `encoder/EncoderCommon.cpp`, `encoder/BitstreamUtils.cpp`, `core/Logger.cpp`,
    `DEFINES BACKTRACK_HAS_NVENC=1`, the SDK include path, `LIBS d3d11`. Drives
    the real encoder with a fake NVENC API and WARP textures: mapping per use,
    B-frame token matching including out-of-order retirement, repeated input,
    rejection, reset, lock/unmap failure, shutdown retention, and the early
    non-blocking lock statuses (`LOCK_BUSY`/`OUT_OF_MEMORY` retry, a stuck lock
    still escalates after the deadline).
  - `AmfEncoderTests` - includes `encoder/AmfEncoder.cpp` itself, plus
    `encoder/EncoderCommon.cpp`, `encoder/BitstreamUtils.cpp`, `core/Logger.cpp`,
    `DEFINES BACKTRACK_HAS_AMF=1`, the AMF include path, `LIBS ole32`. Registered
    only when the AMF headers are found.
  - `AudioDenoiseTests` - `audio/RnnoiseDenoiser.cpp`, `LIBS rnnoise`. Built
    only when `BACKTRACK_NOISE_SUPPRESSION=ON`; the suite itself has no
    `#ifdef`, the CMake `if` around `add_backtrack_test` is the whole gate.
  - `MainWindowAccessibilityTests`, `MainWindowLibraryTests`,
    `MainWindowNotificationTests`, `MainWindowSettingsTests`,
    `MainWindowUpdateCheckTests` - real windows over the `BacktrackApp` object
    library. Page navigation and shortcuts, the library scan and thumbnail
    worker, controller-action busy feedback, settings draft/commit/auto-record,
    and the update check against a loopback HTTP server (`LIBS ws2_32`).
- Stubs live inside the test source, inside `namespace backtrack` so they
  satisfy the production declaration. `ReplayBufferTests` and
  `StorageLayoutTests` define `Mp4Muxer::muxToMp4` and
  `removeEmptyOutputReservation` themselves so `Mp4Muxer.cpp` stays out of the
  build; that is why those suites do not need Media Foundation. The stub mux
  returns true unless a test sets the failure flag, so the mux-failure branch is
  reachable; an always-true stub makes it dead code. `ReplayBufferTestAccess` is
  the same idea for a fault the class cannot reach through its public API: it
  calls the private `breakRingForTesting` hook. `SettingsClipGuardTests` stubs
  `defaultClipDirectory()` so `platform/Win32Util.cpp` (DXGI, avrt, winmm)
  stays out. Follow those patterns rather than linking more of the app or
  weakening production code to make a fault reachable.
- The mux stub writes the recovered MP4 straight to the output path, so it must
  drop the empty hidden reservation first, exactly as the real muxer does. The
  CRT cannot open a hidden file (`EACCES`), and a reservation left in place makes
  every recovery look like a mux failure.
- The stubs in `ReplayBufferTests` are process-global, so a case that asserts the
  muxer was *not* reached has to clear `g_lastMuxedInputs` first. Without that it
  reads a value an earlier case left behind and passes or fails for the wrong
  reason. Recovery muxes an owned work copy of the streams, so a recovery case
  keys failure injection on the recovered *output* name, not the input path.
- A test that pins Win32 behavior has to match what the API actually returns, not
  what the comment above it assumes. `MoveFileExW` with
  `MOVEFILE_REPLACE_EXISTING` over a destination another handle holds open
  returns `ERROR_ACCESS_DENIED`, not `ERROR_SHARING_VIOLATION` (verified
  in-process and cross-process, for every `FILE_SHARE_*` combination), so
  `RecoveryStore::publishWithRetry` retries all three. The held-open case is
  `testManifestPublishSurvivesSharingViolation`: it releases after 100 ms, well
  inside the production budget of 4 attempts over ~2.1 s, and goes red if
  `ERROR_ACCESS_DENIED` is dropped from that retry set.
- What is still manual: real NVENC/AMF hardware, WGC and Desktop Duplication
  capture, HDR tone mapping and cursor appearance, WASAPI endpoint switching,
  the MP4 write itself, and the end-to-end recording/replay flows. Those checks
  live in the owning domain's Verification section. The lifecycle suites only
  prove the encoder's bookkeeping against a fake API.
- Tests are CMake and CTest only. `backtrack.vcxproj` has no test targets, so a
  change that only builds under MSBuild is not verified here.

## Work Guidance

- Test the contract, not the implementation. Prefer a case that fails if a
  documented guarantee breaks (canonical conversion, keyframe strictness, ring
  wrap reassembly, cadence collapse) over one that pins an internal detail.
- Keep a suite self-contained and fast; these run on every build.
- Anything needing real hardware or an interactive desktop is a manual check
  recorded in the owning domain's Verification section, not a test here.

## Verification

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

## Child DOX Index

None. This folder's children are files only.
