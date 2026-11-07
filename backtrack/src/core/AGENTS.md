# core

## Purpose

Types shared across every domain: pipeline and settings structs, the logger,
the lock-free queue used between threads, and the app version constant.

## Ownership

- Owns: `core/Types.h`, `core/Clock.h`, `core/Logger.*`, `core/SpscQueue.h`,
  `core/AppVersion.h`.
- Does not own: any behavior that belongs to a domain. `Types.h` describes data,
  it does not act on it.

## Local Contracts

- `Types.h` holds the settings structs (`VideoSettings`, `ReplaySettings`,
  `GpuOptimizationSettings`, `HotkeySettings`, `GameIntegrationSettings`,
  `AppSettings`) and the wire structs (`GpuFrame`, `EncodedPacket`,
  `AudioPacket`, `CaptureTarget`, plus the stats and capability structs).
  `RecordingStats::hdrActive` / `hdrRejected` / `hdrFlipCount` / `hdrFormat` /
  `hdrPinned` / `hdrBridgedFrames` are
  diagnostics-only mirrors with
  no settings backing (HDR stays automatic): active means HDR frames are
  flowing, rejected means tone-mapping was refused or an HDR desktop is read as
  SDR, flip count tracks hdr/format flips per pipeline run, format is the last
  observed frame format, pinned means the capture session locked the single
  HDR mapping, bridged counts the SDR surfaces normalized into it. The Diagnostics page renders them as one `HDR:` line and `saveLog`
  appends the same chain to the saved copy.
- Every settings struct has a defaulted `operator==`. The UI dirty check
  (`MainWindow::computeSettingsDirty`) compares whole structs, so a new
  non-comparable member silently breaks "unsaved changes" detection.
- Defaults live in the struct definition, once. `SettingsStore` reads and writes
  values; it does not carry a second copy of the defaults.
- Adding a setting is one change across five sites: the field in `Types.h`, the
  parse branch and the serialize branch in `SettingsStore.cpp`, the id in
  `ui/UiIds.h`, and the control plus its read in `ui` (`UiHelpers.cpp` also
  needs the id in `isSettingsControlId` or the page never goes dirty). An
  enum-valued setting also needs a `Name` / `FromName` pair in
  `settings/SettingsStore.h`; a value that can be out of range also needs a
  clamp in `sanitizeSettings`. A field behind `BACKTRACK_HAS_NOISE_SUPPRESSION`
  is wrapped in that `#ifdef` at every site that touches it.
- `kHundredNanosecondsPerSecond` and `secondsTo100ns` are the only conversion
  helpers, plus `core/Clock.h`'s QPC/steady-clock converters (`qpcTo100ns`,
  `qpcNow100ns`, `steadyNow100ns`, `steadyTimePoint100ns`,
  `steadyPointFrom100ns`). Timestamps are signed 100 ns units and may legitimately be zero or
  negative, so nothing in this folder may use a sentinel value; see
  `src/AGENTS.md` for the boundary rule.
- `Logger` is a process singleton, safe from any thread. `initialize` once at
  startup, then `setMinLevel`; it renames any previous log to `<path>.old` and
  clears the rate-limit and recent-line state. It keeps both a file and a
  200-line `recentLines` deque for the Diagnostics page. Rate limiting is keyed
  on the message text alone: level, subsystem, and thread id are recorded for
  the summary line but are not part of the key, so the same text from two
  callers shares one budget of 3 lines per rolling 10 s window. suppressed
  occurrences produce one `(rate limited N occurrences ...)` line when that
  window rolls over or an expired key is pruned; the summary uses the last
  occurrence's level, subsystem, and thread. `Error` is never throttled. the
  file stream is buffered, not flushed per line; `flush` reports whether the
  file stream was open and flushed successfully. flush before export or a fatal
  exit.
- `SpscQueue<T>` is header-only and lock-free with exactly one producer and one
  consumer. `tryPush` returns false when full and leaves the caller's item
  untouched, so the overflow policy belongs to the caller, which must count it;
  `tryPop` returns false when empty. `size()` is advisory and clamped, never
  exact. The ring storage is fixed for the queue's lifetime: `resetCapacity`
  only lowers an atomic logical limit (clamped to the storage) and never
  reassigns the backing vector, so `size()`/`capacity()` are safe to call from
  any thread, including while a reset runs (the UI diagnostics timer reads
  `frameQueue_.size()` during a pipeline restart). `resetCapacity`/`clear` are
  still legal only when both ends are stopped; `clear` moves the consumer's
  index, so it needs the consumer stopped too.
- `AppVersion.h`'s `kCurrentAppVersion` is the app's only version string;
  `MainWindow::startUpdateCheck` compares it against the latest GitHub release.
  The check is opt-out: `AppSettings::checkForUpdates` (default on) gates it in
  `MainWindow::create`. Update the version there; do not add a second version
  string elsewhere.

## Work Guidance

- Keep `Types.h` free of behavior and of heavy includes. It already pulls in
  `Windows.h`, `d3d11.h`, and `dxgi1_2.h`; adding more is a cost paid by every
  translation unit.
- A struct that only one domain uses belongs in that domain's header.
- Comments on these structs explain units and validity rules (100 ns timestamps,
  `hasDts`, when a counter may be zero). Keep that style when extending them.

## Verification

`SpscQueueTests` covers the queue; no suite covers `AppVersion.h` directly.
`LoggerDiagnosticsTests` (CTest) covers logger rate limiting, pruning, stream
failure, and diagnostics export. existing suites also link `Logger.cpp`.

## Child DOX Index

None. This folder's children are files only.