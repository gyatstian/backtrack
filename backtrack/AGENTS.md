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
    registration, option `BACKTRACK_NOISE_SUPPRESSION` (default OFF).
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
- Compile-time switches, all owned by the build files; never set one from
  source:
  - `BACKTRACK_HAS_NVENC` / `BACKTRACK_HAS_AMF`: header detection, always
    defined as `=1` or `=0`. CMake resolves them with `find_path`, the vcxproj
    with `Exists()` on the same files under `src\Interface`. Absent headers
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
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release
```

The generator is `"Visual Studio 17 2022"` because Build Tools 2022 (17.14) is
the installed toolchain; `cmake` lives inside that install and is not on
`PATH`. The app target also needs the `C++ ATL for latest v143 build tools`
component (`<sapi.h>` pulls in `atlbase.h`); without it only the app fails, the
test targets still build.

With noise suppression on, add `-DBACKTRACK_NOISE_SUPPRESSION=ON`; that adds
`AudioDenoiseTests` to the six unconditional suites.

## Child DOX Index

- `src` - application source, one folder per domain, shared conventions
- `tests` - plain console unit suites registered with CTest