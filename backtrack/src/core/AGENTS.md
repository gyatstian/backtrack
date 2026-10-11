# core

## Purpose

Types shared across every domain: pipeline and settings structs, the logger,
the lock-free queue used between threads, and the app version constant.

## Ownership

- Owns: `core/Types.h`, `core/Logger.*`, `core/SpscQueue.h`,
  `core/AppVersion.h`.
- Does not own: any behavior that belongs to a domain. `Types.h` describes data,
  it does not act on it.

## Local Contracts

- `Types.h` holds the settings structs (`VideoSettings`, `ReplaySettings`,
  `GpuOptimizationSettings`, `HotkeySettings`, `GameIntegrationSettings`,
  `AppSettings`) and the wire structs (`GpuFrame`, `EncodedPacket`,
  `AudioPacket`, `CaptureTarget`, plus the stats and capability structs).
- Every settings struct has a defaulted `operator==`. The UI dirty check
  (`MainWindow::computeSettingsDirty`) compares whole structs, so a new
  non-comparable member silently breaks "unsaved changes" detection.
- Defaults live in the struct definition, once. `SettingsStore` reads and writes
  values; it does not carry a second copy of the defaults.
- Adding a setting is a five-place edit, all in one change: the field in
  `Types.h`, a `Name`/`FromName` pair in `settings/SettingsStore.h`, the parse
  and serialize branches in `SettingsStore.cpp`, a clamp in `sanitizeSettings`,
  and the control plus its id in `ui`. Conditional fields are wrapped in
  `#ifdef BACKTRACK_HAS_NOISE_SUPPRESSION` at every one of those five places.
- `Logger` is a process singleton, safe from any thread. `initialize` once at
  startup, then `setMinLevel`. It keeps both a file and a bounded
  `recentLines` deque for the Diagnostics page, and rate-limits per message
  text in a rolling window. Flush before any fatal exit.
- `SpscQueue<T>` is header-only and lock-free with exactly one producer and one
  consumer. `tryPush` drops the newest item when full and `tryPop` returns
  false when empty; callers must count the difference. `size()` is advisory
  only. `resetCapacity` is legal only when both ends are stopped.
- `AppVersion.h`'s `kCurrentAppVersion` is not referenced by any source today.
  Update it when release versioning comes back; do not add a second version
  string elsewhere.

## Work Guidance

- Keep `Types.h` free of behavior and of heavy includes. It already pulls in
  `Windows.h`, `d3d11.h`, and `dxgi1_2.h`; adding more is a cost paid by every
  translation unit.
- A struct that only one domain uses belongs in that domain's header.
- Comments on these structs explain units and validity rules (100 ns timestamps,
  `hasDts`, when a counter may be zero). Keep that style when extending them.

## Verification

No test covers this folder directly; `Logger.cpp` is linked into
`AudioConvertTests` and `ReplayBufferTests`, so a build of those suites is the
practical check. `ReplayBufferTests` exercises `SpscQueue` transitively through
the pipeline it links.

## Child DOX Index

None. This folder's children are files only.