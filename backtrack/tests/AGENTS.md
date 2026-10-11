# tests

## Purpose

Unit suites for the parts of the app that can be exercised without a GPU, a
capture device, or a window.

## Ownership

- Owns: the four test sources and how they are registered with CTest.
- Does not own: the code under test. A failing test points into the domain
  folder's AGENTS.md for the contract it broke.

## Local Contracts

- Plain console executables with `main()`. No test framework, no fixtures, no
  mocks library. Failure is signalled by an `expect(condition, message)` helper
  that throws, and the suite reports and exits non-zero.
- Suites are registered in the root `CMakeLists.txt` through
  `add_backtrack_test(name SOURCES ... [LIBS ...])`, which sets C++20, adds `src`
  to the include path, defines `NOMINMAX`, and adds a CTest entry.
- Each suite lists exactly the implementation files it needs, because nothing
  links the whole app. Adding a function to a source under test means adding that
  file to the suite's `SOURCES` list, or the link fails.
- Current suites and what they cover:
  - `VideoTimelineSchedulerTests` - `app/VideoTimelineScheduler.h`, header only,
    compiles standalone with `cl /std:c++20 /EHsc /I backtrack\src`.
  - `AudioConvertTests` - `audio/AudioConvert.cpp` plus `core/Logger.cpp`.
  - `ReplayBufferTests` - `replay/ReplayBuffer.cpp`, `mux/RecoveryStore.cpp`,
    `audio/AudioConvert.cpp`, `audio/WavReader.cpp`, `mux/WavWriter.cpp`,
    `encoder/BitstreamUtils.cpp`, `core/Logger.cpp`.
  - `AudioDenoiseTests` - `audio/RnnoiseDenoiser.cpp`, only when
    `BACKTRACK_NOISE_SUPPRESSION=ON`.
- Stubs live inside the test source. `ReplayBufferTests` defines
  `Mp4Muxer::muxToMp4` and `removeEmptyOutputReservation` itself so
  `Mp4Muxer.cpp` stays out of the build; that is why it does not need Media
  Foundation. Follow that pattern rather than linking more of the app.
- Tests are CMake and CTest only. The Visual Studio solution has no test
  targets, so a change that only builds under MSBuild is not verified here.

## Work Guidance

- Test the contract, not the implementation. Prefer a case that fails if a
  documented guarantee breaks (canonical conversion, keyframe strictness, ring
  wrap reassembly, cadence collapse) over one that pins an internal detail.
- Keep a suite self-contained and fast; these run on every build.
- Anything needing a GPU, a device, or a window is a manual check recorded in the
  owning domain's Verification section, not a test here.

## Verification

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release
```

## Child DOX Index

None. This folder's children are files only.