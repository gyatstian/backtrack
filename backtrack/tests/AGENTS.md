# tests

## Purpose

Unit suites for the parts of the app that can be exercised without a GPU, a
capture device, or a window.

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
  `add_backtrack_test(name SOURCES ... [LIBS ...])`, which sets C++20, adds
  `src` to the include path, defines `NOMINMAX`, and adds the CTest entry.
  `NOMINMAX` is load-bearing: the linked sources call unparenthesized
  `std::min`/`std::max` (`core/Logger.cpp`, `replay/ReplayBuffer.cpp`,
  `audio/WavReader.cpp`) and `Windows.h` turns those into macros.
- Nothing links the whole app, so each suite lists the implementation files it
  needs. A new source file under test goes into that suite's `SOURCES` list in
  the same change, or the link fails.
- Current suites and what they cover:
  - `VideoTimelineSchedulerTests` - `app/VideoTimelineScheduler.h` alone, so
    the header must stay free of Windows.h and project includes for the suite
    to compile as a single file.
  - `DecodeOrderTimestampsTests` - `encoder/DecodeOrderTimestamps.h` plus
    `core/Logger.cpp` (the desync watchdog logs). DTS equals PTS with no
    reordering, the DTS/PTS offset settles at the pipeline depth under
    B-frames, ctts offsets stay non-negative, the 96-output desync watchdog
    realigns and does not fire early or accumulate across bursts, and `reset`
    with the full depth (`bFrames ? 2 : 0` + `lookahead ? depth : 0`) is
    contrasted with the too-small depth `encoder/AGENTS.md` warns about.
  - `SpscQueueTests` - `core/SpscQueue.h` alone. FIFO order, `tryPush` failing
    on a full queue without disturbing the head, `size()` hand-computed across
    a wrap and never above capacity, `clear()`, and `resetCapacity` including
    the zero-request clamp to 1.
  - `AudioConvertTests` - `audio/AudioConvert.cpp`, `core/Logger.cpp`:
    canonical conversion (passthrough, resample, mono duplication, 5.1
    downmix, rejection of an unsupported format) and that a mid-stream format
    change still leaves every packet on one uniform format.
  - `ReplayBufferTests` - `replay/ReplayBuffer.cpp`, `mux/RecoveryStore.cpp`,
    `audio/AudioConvert.cpp`, `audio/WavReader.cpp`, `mux/WavWriter.cpp`,
    `encoder/BitstreamUtils.cpp`, `core/Logger.cpp`. Covers both the RAM and the
    disk backend (the disk cases use small ring capacities and push more than
    the ring holds, to force wrap and extent reassembly), `RecoveryStore`
    manifest recovery (round trip, tail trim, torn line, non-contiguous tail,
    a declared-count shortfall, rejection of damaged metadata, a publish that
    survives a scanner holding `recovery.tsv`, and a directory with no manifest
    that must stay unrecoverable), the replay audio gap-fill cap, the
    unpatched-WAV-header fallback, the bitstream cut-point and parameter-set
    classifiers (including a NAL ending at the buffer's last byte), and the
    mux-failure and ring-read-failure save paths.
  - `SettingsClipGuardTests` - `settings/SettingsStore.cpp`,
    `clips/ClipManager.cpp`, `core/Logger.cpp`, `LIBS propsys`:
    `sanitizeSettings` (both ends of every range, resolution presets, app-list
    dedupe, hotkey masking and reset of unsafe or duplicated bindings),
    `SettingsStore::load` (every key falling back on a missing or malformed
    value, repeated keys, comments and lines without `=`, clamping through
    `load`, the four legacy key spellings, unknown keys, the case-insensitive
    `library.viewMode`, and a UTF-8 round trip through `save` that asserts no
    BOM and no leftover `settings.ini.tmp`), `save` itself on observable disk
    state (a save that cannot swap leaves the old file byte-for-byte intact, and
    a second save over an existing file still round trips), an unknown
    `xxxFromName` name falling back instead of aborting, `ClipManager`'s
    managed-path gate, and `listClips` finding clips in per-game subfolders
    (and the legacy `clips` subdirectory) while skipping `.backtrack_tmp`
    staging files.
  - `AudioDenoiseTests` - `audio/RnnoiseDenoiser.cpp`, `LIBS rnnoise`. Built
    only when `BACKTRACK_NOISE_SUPPRESSION=ON`; the suite itself has no
    `#ifdef`, the CMake `if` around `add_backtrack_test` is the whole gate.
- Stubs live inside the test source, inside `namespace backtrack` so they
  satisfy the production declaration. `ReplayBufferTests` defines
  `Mp4Muxer::muxToMp4` and `removeEmptyOutputReservation` itself so
  `Mp4Muxer.cpp` stays out of the build; that is why the suite does not need
  Media Foundation. The stub mux returns true unless a test sets the failure
  flag, so the mux-failure branch is reachable; an always-true stub makes it
  dead code. `ReplayBufferTestAccess` is the same idea for a fault the class
  cannot reach through its public API: it calls the private
  `breakRingForTesting` hook. `SettingsClipGuardTests` stubs
  `defaultClipDirectory()` so `platform/Win32Util.cpp` (DXGI, avrt, winmm)
  stays out, and reaches the private `ClipManager::isManagedClipPath` through
  `removeClip`, asserting both the refusal and that the rejected file is still
  on disk. `SettingsStore.cpp`'s other two `Win32Util` uses are the `inline`
  `trimText` and `normalizedPathKey` from the header, which is why they are
  inline and not exported. Follow those patterns rather than linking more of the
  app or weakening production code to make a fault reachable.
- The stubs in `ReplayBufferTests` are process-global, so a case that asserts the
  muxer was *not* reached has to clear `g_lastMuxedInputs` first. Without that it
  reads a value an earlier case left behind and passes or fails for the wrong
  reason.
- A test that pins Win32 behavior has to match what the API actually returns, not
  what the comment above it assumes. `MoveFileExW` with
  `MOVEFILE_REPLACE_EXISTING` over a destination another handle holds open
  returns `ERROR_ACCESS_DENIED`, not `ERROR_SHARING_VIOLATION` (verified
  in-process and cross-process, for every `FILE_SHARE_*` combination), so
  `RecoveryStore::publishWithRetry` retries all three. The held-open case is
  `testManifestPublishSurvivesSharingViolation`: it releases after 100 ms, well
  inside the production budget of 4 attempts over ~2.1 s, and goes red if
  `ERROR_ACCESS_DENIED` is dropped from that retry set.
- No suite covers the encoder backends, `mux/Mp4Muxer` (only its stub runs),
  capture, the UI, or the UI-side settings flow. Changes there are verified by
  building and by hand.
- Tests are CMake and CTest only. `backtrack.vcxproj` has no test targets, so a
  change that only builds under MSBuild is not verified here.

## Work Guidance

- Test the contract, not the implementation. Prefer a case that fails if a
  documented guarantee breaks (canonical conversion, keyframe strictness, ring
  wrap reassembly, cadence collapse) over one that pins an internal detail.
- Keep a suite self-contained and fast; these run on every build.
- Anything needing a GPU, a device, or a window is a manual check recorded in
  the owning domain's Verification section, not a test here.

## Verification

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release
```

## Child DOX Index

None. This folder's children are files only.