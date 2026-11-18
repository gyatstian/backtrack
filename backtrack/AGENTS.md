# backtrack

## Purpose

The executable project: MSVC project files, resources, and the test suite that
lives beside the sources. `backtrack.slnx` is the Visual Studio entry point,
`backtrack.vcxproj` the single C++ project it contains.

## Ownership

- Owns: build definition for both build systems, the application icon
  (`src/backtrack.rc`), and test wiring.
- Does not own: source-level rules. Those live in `src` and its children.

## Local Contracts

- Two build definitions must stay equivalent:
  - `CMakeLists.txt` at the repository root: target `Backtrack`, CTest
    registration, option `BACKTRACK_NOISE_SUPPRESSION` (default OFF). The app
    sources compile once into the `BacktrackApp` object library; `Backtrack`
    links it and so do the window suites, so an app-only source change must not be
    moved out of that library. Every define that changes a header's layout
    (`BACKTRACK_HAS_*`, `UNICODE`, `WIN32_LEAN_AND_MEAN`) is `PUBLIC` on that
    library: the consumers compile their own translation units against the same
    headers, and a define they cannot see is a struct-layout mismatch that
    crashes at run time. `BACKTRACK_HAS_NOISE_SUPPRESSION` moves a member of
    `app/AudioWriter.h`, which is exactly that case.
  - `backtrack/backtrack.vcxproj` + `.filters`: project property
    `BacktrackNoiseSuppression` (default false).
- New `.cpp` / `.h` under `src` is picked up automatically by both
  (`file(GLOB_RECURSE ... CONFIGURE_DEPENDS)` in CMake, `src\**\*.cpp` and
  `src\**\*.h` item wildcards in the vcxproj). A `.cpp` that needs a build
  condition of its own must be excluded from the vcxproj wildcard with
  `Exclude=` and listed separately, or the wildcard compiles it anyway;
  `src\audio\RnnoiseDenoiser.cpp` is the only such entry. The RNNoise `.c`
  files fall outside that wildcard and are listed explicitly with a
  `Condition=`, so adding or removing one is a vcxproj edit on top of the
  CMake `file(GLOB ... third_party/rnnoise/src/*.c)`.
- Two sources may share a file name (`src\audio\WavReader.cpp` and
  `src\mux\WavReader.cpp` do). The vcxproj keeps the source folder in
  `ObjectFileName` for that reason; the default `$(IntDir)%(Filename).obj` makes
  one translation unit silently replace the other, and the link then reports the
  missing symbols. CMake already emits a per-file object name for duplicates.
- Compile-time switches, all owned by the build files; never set one from
  source:
  - `BACKTRACK_HAS_NVENC` / `BACKTRACK_HAS_AMF`: header detection, always
    defined as `=1` or `=0`. CMake probes ordered candidate roots for the same
    prefixed paths the sources include (`Interface/nvEncodeAPI.h`,
    `AMF/core/Factory.h`): an explicit `NV_CODEC_SDK_PATH` / `AMF_SDK_PATH`
    root wins over the vendored headers, which are the last candidate. The
    vcxproj only ever sees the vendored files (`Exists()` under
    `src\Interface`), so an external SDK is a CMake-only route. Absent headers
    compile the backend to a stub reporting unavailable.
  - `BACKTRACK_HAS_NOISE_SUPPRESSION`: enables the RNNoise wrapper, its UI
    checkbox, and `AudioDenoiseTests`. All three must agree, so toggle the
    build flag, not the `#ifdef`s.
- Warning settings differ between the two definitions and must stay equivalent
  in effect. `backtrack.vcxproj` sets `SDLCheck=true` (`/sdl`), which promotes
  deprecation diagnostics to errors, and the SDK's `sphelper.h` calls the
  deprecated `GetVersionExW` on our behalf. The app therefore fails with
  `error C4996` while compiling `voice/VoiceCommandService.cpp`. One
  unconditional `ItemDefinitionGroup` disables C4996, which is what keeps the
  vcxproj building and matches CMake, which never passes `/sdl`. Do not turn
  `/sdl` off instead: that gives up the security checks for every other
  warning.
- CMake exposes `src` as the include root and adds a `NOMINMAX` define to the
  test targets because shared sources use unparenthesized `std::min`/`max`.
  Keep that define on any test target you add.
- The executable is a GUI subsystem app (`WIN32`), so entry is `wWinMain` in
  `src/main.cpp`.
- Output paths: `build/Release/Backtrack.exe` (CMake) or
  `x64/Release/backtrack.exe` (MSBuild).

## Work Guidance

- When adding a source file that a test needs to link, add it to that test's
  `add_backtrack_test(... SOURCES ...)` list in the same change, or the test
  will fail to link in a way that looks like a missing implementation.
- `Mp4Muxer.cpp` is deliberately excluded from `ReplayBufferTests` because it
  pulls in Media Foundation; the test stubs its entry points instead. Preserve
  that split when growing the test.
- Keep the RNNoise static library at `/W3` in CMake: its benign
  `double` to `float` warnings must not consume the app's warning budget.

## Verification

```powershell
cmake -S . -B build -G "Ninja Multi-Config"
cmake --build build --config Release --parallel
ctest --test-dir build -C Release
```

Run these from a shell that has the MSVC developer environment
(`vcvars64.bat`); a bare shell cannot find the C++ standard library headers.
`"Visual Studio 17 2022"` only configures where the `v143` toolset is
installed, so it is not the portable route. Toolchain paths and ATL
requirements are in the root `AGENTS.md`.

Every registered suite builds unconditionally except `NvencLifecycleTests` and
`AmfEncoderTests`, added when the NVENC and AMF headers are found (both are
vendored), and `AudioDenoiseTests`, which needs
`-DBACKTRACK_NOISE_SUPPRESSION=ON`.

## Child DOX Index

- `src` - application source, one folder per domain, shared conventions
- `tests` - plain console unit suites registered with CTest