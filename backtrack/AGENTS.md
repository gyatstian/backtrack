# backtrack

## Purpose

The executable project: MSVC project files, resources, and the test suite that
lives beside the sources. `backtrack.slnx` is the Visual Studio entry point,
`backtrack.vcxproj` the single C++ project it contains.

## Ownership

- Owns: build definition for both build systems, the application manifest /
  icon / version resources, and test wiring.
- Does not own: source-level rules. Those live in `src` and its children.

## Local Contracts

- Two build definitions must stay equivalent:
  - `CMakeLists.txt` at the repository root: target `Backtrack`, CTest
    registration, option `BACKTRACK_NOISE_SUPPRESSION` (default OFF).
  - `backtrack/backtrack.vcxproj` + `.filters`: project property
    `BacktrackNoiseSuppression`, explicit `<ClCompile>` entries per file.
- New `.cpp` / `.h` under `src` is picked up automatically by both
  (`file(GLOB_RECURSE ... CONFIGURE_DEPENDS)` in CMake, `src\**\*.cpp` and
  `src\**\*.h` item wildcards in the vcxproj). The RNNoise `.c` files are the
  exception: they are listed per file in both, so a change to that set is a
  two-file edit.
- Compile-time switches, all defined by the build files and never in source:
  - `BACKTRACK_HAS_NVENC` / `BACKTRACK_HAS_AMF`: header detection. Absent
    headers compile the backend to a stub reporting unavailable.
  - `BACKTRACK_HAS_NOISE_SUPPRESSION`: enables the RNNoise wrapper, its UI
    checkbox, and `AudioDenoiseTests`. All three must agree, so toggle the
    build flag, not the `#ifdef`s.
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
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release
```

With noise suppression on, add `-DBACKTRACK_NOISE_SUPPRESSION=ON` and expect a
fifth suite, `AudioDenoiseTests`.

## Child DOX Index

- `src` - application source, one folder per domain, shared conventions
- `tests` - plain console unit suites registered with CTest