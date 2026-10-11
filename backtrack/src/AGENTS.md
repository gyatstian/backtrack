# backtrack/src

## Purpose

All application source, one folder per domain. This folder owns the file itself
(`main.cpp`, `resource.h`, `backtrack.rc`, `backtrack.ico`) and the conventions
every child shares.

## Ownership

- Owns: include style, namespace, logging entry points, unit conventions, and
  the domain folder layout.
- Does not own: any domain's internals. Those are in the child AGENTS.md files.

## Local Contracts

- Includes resolve from `src`: `#include "capture/ICaptureSource.h"`. Never
  write `../` includes or absolute paths.
- Everything is in `namespace backtrack`.
- Timestamps crossing a module boundary are 100 ns units: `pts100ns`,
  `dts100ns`, `duration100ns`. Convert from `std::chrono` with
  `kHundredNanosecondsPerSecond` (core/Types.h). Milliseconds exist only inside
  a Win32 timeout call.
- PTS is the authoritative timeline anchor. DTS is present only when the encoder
  emitted B-frames and is always paired with an explicit `hasDts` flag, because
  zero and negative timestamps are legal values and no sentinel exists.
- All logging goes through `core/Logger.h` with a subsystem label
  (`Logger::instance().info(L"mux", ...)`). The logger rate-limits repeated
  message text, so a hot loop can log unconditionally; do not add your own
  suppression unless the message text itself varies per event.
- Long-lived Win32 or SDK handles live behind a `struct Impl` +
  `std::unique_ptr` so headers stay cheap to include (see `WgcCaptureSource`,
  `NvencEncoder`, `AmfEncoder`).
- Threads are named with `platform/Win32Util.h:setThreadDescriptionSafe` and
  given an MMCSS profile with `enableMmcssFor*Thread`. Reuse those helpers
  instead of calling Win32 directly.
- Direct3D calls go through `capture/D3DDevice`, whose immediate-context mutex
  every per-operation user must hold.
- No exceptions escape a thread procedure. Wrap new worker bodies the way
  `RecorderController`'s `*Impl` methods are wrapped.
- No new third-party dependency. Win32, D3D11, DXGI, Media Foundation, WASAPI,
  SAPI, and WinHTTP are already linked.

## Work Guidance

- Comments explain why: a vendor quirk, a timing constraint, a memory-ordering
  requirement. Skip comments that restate the next line.
- Keep a new type next to the module that owns it. Shared types belong in
  `core/Types.h` only when two domains need them.
- Prefer editing an existing function over adding a sibling wrapper. A new
  public method needs a caller in the same change or it is dead code.
- When a behavior becomes stable and non-obvious, record it in the child
  AGENTS.md for that domain, not here.

## Verification

`cmake --build build --config Release` plus `ctest --test-dir build -C Release`
from the repository root. Pure-logic changes should come with an
`add_backtrack_test` entry in the root `CMakeLists.txt`.

## Child DOX Index

- `app` - application lifetime, `RecorderController`, video timeline scheduler
- `audio` - WASAPI capture, ingest canonicalization, mixing, RNNoise wrapper
- `capture` - capture sources, D3D11 device, GPU frame scaler
- `clips` - clip library management
- `core` - shared types, settings structs, logger, SPSC queue
- `encoder` - `IEncoder`, NVENC and AMF backends, bitstream helpers
- `hotkeys` - global recording and replay hotkeys
- `integrations` - Discord Rich Presence, League of Legends integration
- `mux` - MP4 muxing, WAV writing, crash recovery
- `platform` - shared Win32 helpers
- `replay` - rolling replay buffer
- `settings` - settings persistence and sanitization
- `ui` - native Win32 main window
- `voice` - SAPI voice commands
- `Interface` - vendored NVIDIA / AMD SDK headers
- `third_party` - vendored RNNoise