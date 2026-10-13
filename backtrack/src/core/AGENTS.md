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
  `RecordingStats::hdrActive` / `hdrRejected` are diagnostics-only mirrors with
  no settings backing (HDR stays automatic): active means HDR frames are
  flowing, rejected means tone-mapping was refused or an HDR desktop is read as
  SDR. The Diagnostics page renders them as one `HDR:` line and `saveLog`
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
  callers shares one budget of 3 lines per rolling 10 s window, and what was
  suppressed surfaces as a single `(rate limited N occurrences ...)` line when
  the window rolls over. `Error` is never throttled. The file stream is
  buffered, not flushed per line; flush before any fatal exit.
- `SpscQueue<T>` is header-only and lock-free with exactly one producer and one
  consumer. `tryPush` returns false when full and leaves the caller's item
  untouched, so the overflow policy belongs to the caller, which must count it;
  `tryPop` returns false when empty. `size()` is advisory and clamped, never
  exact. `resetCapacity` is legal only when both ends are stopped; `clear`
  moves the consumer's index, so it needs the consumer stopped too.
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

No suite covers `SpscQueue.h` or `AppVersion.h`: the queue's only user is
`RecorderController`, which no test links. `Logger.cpp` is linked into
`AudioConvertTests`, `ReplayBufferTests`, and `SettingsClipGuardTests`, so
building those three is the practical check for this folder; `Types.h` and
`AppVersion.h` are compile-checked by whichever of them include them.

## Child DOX Index

None. This folder's children are files only.