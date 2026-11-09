# DOX framework

- DOX is highly performant AGENTS.md hierarchy installed here
- Agent must follow DOX instructions across any edits

## Project

### Purpose

Backtrack is a Windows screen recorder and instant-replay tool. It captures a
monitor or window (Windows Graphics Capture or DXGI Desktop Duplication),
encodes on the GPU (NVENC on NVIDIA, AMD AMF on AMD), keeps a rolling replay
buffer of encoded packets, and muxes saved replays into MP4. Native Win32 UI,
C++20, no UI framework, no runtime dependency except the optional vendored
RNNoise.

### Ownership

- This file owns: project-wide rules, build entry points, and the top-level
  Child DOX Index.
- This file does not own: any module's internal contracts. Every
  `backtrack/src/*` folder and `backtrack/tests` owns its own rules through its
  child AGENTS.md.
- Nothing may weaken the DOX framework section below.

### Local Contracts

- Two build systems describe the same project: `CMakeLists.txt` (root) and
  `backtrack/backtrack.vcxproj` + `backtrack.slnx`. A change to sources,
  feature flags, or link libraries must be reflected in both.
- `backtrack/src` is the only include root. Include as
  `#include "domain/Header.h"`, never with a relative path.
- All code lives in `namespace backtrack`.
- Target-wide defines (`UNICODE`, `_UNICODE`, `NOMINMAX`,
  `WIN32_LEAN_AND_MEAN`) come from the build files. Do not redefine them in
  source, and do not use `min`/`max` from `Windows.h` (use
  `std::min`/`std::max` with parentheses).
- Runtime paths: clips in `%USERPROFILE%\Videos\Backtrack` by default,
  settings and logs under `%LOCALAPPDATA%\Backtrack`.
- Vendor SDKs are loaded at runtime from System32 (`nvEncodeAPI64.dll`,
  `amfrt64.dll`), never linked. A missing backend must degrade to "unavailable
  with a reason", never to a build or startup failure.

### Work Guidance

- Comment the reason, not the mechanics. Existing comments exist where a
  timing, memory, or vendor workaround would otherwise look arbitrary.
- Do not add third-party dependencies. Prefer Win32, D3D11, Media Foundation,
  SAPI, and WinHTTP, all already linked.
- Do not add a dependency to a module that has no AGENTS.md of its own; create
  one in the same change.

### Verification

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release
```

MSBuild path: `MSBuild.exe backtrack.slnx /p:Configuration=Release
/p:Platform=x64 /m`. There is no CI; a green local build plus `ctest` is the
gate.

Toolchain, verified on 2026-10-11: the only installed Visual Studio is Build
Tools 2022 (17.14, channel `VisualStudio.17.Release`), so the generator is
`"Visual Studio 17 2022"`. `cmake` is not on `PATH`; it ships inside the
install at `Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`
under `%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools`, or use
`LaunchDevCmd.bat`. The app target needs the `C++ ATL for latest v143 build
tools` component: `voice/VoiceCommandService.cpp` includes `<sapi.h>`, whose
`sphelper.h` includes `atlbase.h`. Without ATL installed the test targets still
build and pass, but the app fails with `C1083 atlbase.h not found`.

`backtrack.vcxproj` pins `PlatformToolset` to `v143` to match that install. Do
not raise it from a machine with a newer Visual Studio; the vcxproj is the only
build definition that names a toolset, and CMake picks up whatever is installed.

Build directories are generated output and do not survive a move: `build/`
(CMake) and `x64/` plus `backtrack/x64/` (MSBuild output and intermediates)
all record absolute paths. A copied or renamed checkout must delete `build/`
before configuring, or CMake aborts with "the current CMakeCache.txt directory
... is different than the directory ... where CMakeCache.txt was created".

## Core Contract

- AGENTS.md files are binding work contracts for their subtrees
- Work products, source materials, instructions, records, assets, and durable docs must stay understandable from the nearest applicable AGENTS.md plus every parent AGENTS.md above it

## Read Before Editing

1. Read the root AGENTS.md
2. Identify every file or folder you expect to touch
3. Walk from the repository root to each target path
4. Read every AGENTS.md found along each route
5. If a parent AGENTS.md lists a child AGENTS.md whose scope contains the path, read that child and continue from there
6. Use the nearest AGENTS.md as the local contract and parent docs for repo-wide rules
7. If docs conflict, the closer doc controls local work details, but no child doc may weaken DOX

Do not rely on memory. Re-read the applicable DOX chain in the current session before editing.

## Update After Editing

Every meaningful change requires a DOX pass before the task is done.

Update the closest owning AGENTS.md when a change affects:

- purpose, scope, ownership, or responsibilities
- durable structure, contracts, workflows, or operating rules
- required inputs, outputs, permissions, constraints, side effects, or artifacts
- user preferences about behavior, communication, process, organization, or quality
- AGENTS.md creation, deletion, move, rename, or index contents

Update parent docs when parent-level structure, ownership, workflow, or child index changes. Update child docs when parent changes alter local rules. Remove stale or contradictory text immediately. Small edits that do not change behavior or contracts may leave docs unchanged, but the DOX pass still must happen.

## Hierarchy

- Root AGENTS.md is the DOX rail: project-wide instructions, global preferences, durable workflow rules, and the top-level Child DOX Index
- Child AGENTS.md files own domain-specific instructions and their own Child DOX Index
- Each parent explains what its direct children cover and what stays owned by the parent
- The closer a doc is to the work, the more specific and practical it must be

## Child Doc Shape

- Create a child AGENTS.md when a folder becomes a durable boundary with its own purpose, rules, responsibilities, workflow, materials, or quality standards
- Work Guidance must reflect the current standards of the project or user instructions; if there are no specific standards or instructions yet, leave it empty
- Verification must reflect an existing check; if no verification framework exists yet, leave it empty and update it when one exists

Default section order:

- Purpose
- Ownership
- Local Contracts
- Work Guidance
- Verification
- Child DOX Index

## Style

- Keep docs concise, current, and operational
- Document stable contracts, not diary entries
- Put broad rules in parent docs and concrete details in child docs
- Prefer direct bullets with explicit names
- Do not duplicate rules across many files unless each scope needs a local version
- Delete stale notes instead of explaining history
- Trim obvious statements, repeated rules, misplaced detail, and warnings for risks that no longer exist

## Closeout

1. Re-check changed paths against the DOX chain
2. Update nearest owning docs and any affected parents or children
3. Refresh every affected Child DOX Index
4. Remove stale or contradictory text
5. Run existing verification when relevant
6. Report any docs intentionally left unchanged and why

## User Preferences

When the user requests a durable behavior change, record it here or in the relevant child AGENTS.md

## Child DOX Index

- `backtrack` - the executable project: build definitions, resources, and the
  test suite. Its index names the source domains; walk the chain rather than
  reading a flattened copy of it here.

Rules that reach every domain, so they are not repeated in the children:

- The build files own the value of `UNICODE`, `_UNICODE`, `NOMINMAX`,
  `WIN32_LEAN_AND_MEAN`, `BACKTRACK_HAS_NVENC`, `BACKTRACK_HAS_AMF`, and
  `BACKTRACK_HAS_NOISE_SUPPRESSION`. Never set one of them from source.
- Vendor runtimes load from System32 at runtime and are never linked.
- The README is user documentation. Keep it consistent with a source domain when
  a user-visible default, path, or feature changes.
