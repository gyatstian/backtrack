# core

## Purpose

Types shared across every domain: pipeline and settings structs, the logger,
the lock-free queue used between threads, and the app version constant.

## Ownership

- Owns: `core/Types.h`, `core/TimeUnits.h`, `core/Clock.h`, `core/EncodingNames.h`,
  `core/Logger.*`, `core/LogContext.h`, `core/LogTiming.h`, `core/SpscQueue.h`,
  `core/AppVersion.h`.
- Does not own: any behavior that belongs to a domain. `Types.h` describes data,
  it does not act on it.

## Local Contracts

- `Types.h` holds the settings structs (`VideoSettings`, `ReplaySettings`,
  `GpuOptimizationSettings`, `HotkeySettings`, `GameIntegrationSettings`,
  `AppSettings`) and the wire structs (`GpuFrame`, `EncodedPacket`,
  `AudioPacket`, `CaptureTarget`, plus the stats and capability structs).
  `RecordingStats::hdrActive` / `hdrRejected` / `hdrFlipCount` / `hdrFormat` are
  diagnostics-only mirrors with
  no settings backing (HDR stays automatic): active means HDR frames are
  flowing, rejected means tone-mapping was refused or an HDR desktop is read as
  SDR, flip count tracks hdr/format flips per pipeline run, format is the last
  observed frame format. The Diagnostics page renders them as one `HDR:` line and `saveLog`
  appends the same chain to the saved copy.
- Every settings struct has a defaulted `operator==`. The UI dirty check
  (`MainWindow::computeSettingsDirty`) compares whole structs, so a new
  non-comparable member silently breaks "unsaved changes" detection.
- `AudioPacket::bytes` is a `std::pmr::vector` so the WASAPI capture thread
  allocates payloads from a pool instead of the CRT heap. A pmr vector binds its
  resource at construction, so `ensureAudioPacketPool()`
  (`audio/WasapiCapture.h`) must run before any `AudioPacket` exists: `wWinMain`
  calls it before constructing `Application`, whose `RecorderController` builds
  the audio queues. The pool is process-lifetime and never released:
  `synchronized_pool_resource::release()` deallocates every block it owns,
  including those backing live allocations, and `WasapiCapture::stop()`
  therefore does not call it - a stopping capture's packets can still be held by
  the audio writer, the other track's capture, or a RAM-backed replay.
- Defaults live in the struct definition, once. `SettingsStore` reads and writes
  values; it does not carry a second copy of the defaults.
- Adding a setting is one change across five sites: the field in `Types.h`, the
  parse branch and the serialize branch in `SettingsStore.cpp`, the id in
  `ui/UiIds.h`, and the control plus its read in `ui` (`UiHelpers.cpp` also
  needs the id in `isSettingsControlId` or the page never goes dirty). An
  enum-valued setting also needs a `Name` / `FromName` pair: the `Name` side for
  codec/preset/mode lives in `core/EncodingNames.h` (shared with the encoders
  and the diagnostics page), the rest and all `FromName` parsing stay in
  `settings/SettingsStore.h`; a value that can be out of range also needs a
  clamp in `sanitizeSettings`. A field behind `BACKTRACK_HAS_NOISE_SUPPRESSION`
  is wrapped in that `#ifdef` at every site that touches it.
- `kHundredNanosecondsPerSecond` and `secondsTo100ns` live in
  `core/TimeUnits.h` (`<chrono>`/`<cstdint>` only), together with the
  `SteadyClock` alias, so a timestamp-only header converts units without
  including `core/Types.h`. `core/Clock.h` holds the QPC/steady-clock converters
  (`qpcTo100ns`, `qpcNow100ns`, `steadyNow100ns`, `steadyTimePoint100ns`,
  `steadyPointFrom100ns`) and includes TimeUnits.h, never Types.h. Nothing else
  belongs in TimeUnits.h. Timestamps are signed 100 ns units and may legitimately
  be zero or negative, so nothing in this folder may use a sentinel value; see
  `src/AGENTS.md` for the boundary rule.
- `Logger` is a process singleton, safe from any thread. `initialize` once at
  startup, then `setEnabled` (logging defaults on; when off no line is written
  anywhere); it renames any previous log to `<path>.old` and clears the
  rate-limit, recent-line, and pending-group state on every exit path. A
  `initialize` that cannot create its directory leaves no stale state and an
  empty `currentPath()`; one that cannot rename the previous log aside opens
  `<path>.<pid>.log` instead of truncating a log it could not archive. The
  minimum level is fixed at `Trace` (most verbose) so no call site is ever
  filtered. `writeHeader` logs
  the project version, date, environment, and OS build once at startup (the per-line
  timestamp is `HH:mm:ss.SSS`); `writeFooter` logs a shutdown summary of
  error/warning/rate-limited counts. It keeps both a file and a 200-line
  `recentLines` deque for the Diagnostics page. `recentLines` reports the
  deferred duplicate group's text without emitting it, so a Diagnostics poll
  (~1/s) never ends the group or spends its rate-limit budget; flush,
  `writeFooter`, and export stay the only emitters.
- Consecutive-duplicate grouping runs before the rate limiter: back-to-back
  identical lines (same level, subsystem, and message) collapse into one `(Nx)`
  line, emitted when a different message arrives, on `flush`, or on export. The
  windowed rate limiter still bounds interleaved/hot spam that grouping cannot
  merge. Both are keyed on the message text alone: level, subsystem, and thread id
  are recorded for the summary line but are not part of the key, so the same text
  from two callers shares one budget of 3 lines per rolling 10 s window. suppressed
  occurrences produce one `(rate limited N occurrences ...)` line when that window
  rolls over or an expired key is pruned; the summary uses the last occurrence's
  level, subsystem, and thread. `Error` is never throttled (but still groups). the
  file stream is buffered, not flushed per line; `flush` first emits any deferred
  duplicate group and reports whether the file stream was open and flushed
  successfully. flush before export or a fatal exit.
- Task correlation and breadcrumbs come from `core/LogContext.h` (header-only):
  `LogContext::ScopedTask` labels the calling thread's task, and `Logger` appends
  `task=<label>` to every line from it. It is neither copyable nor movable: the
  destructor ends the task, so a copy would pop an outer label. A per-thread 25-entry breadcrumb ring
  records silent state transitions (`LogContext::breadcrumb`); on ERROR the logger
  attaches the `trail=...` (relative-ms breadcrumbs) and a best-effort `stack=...`
  of return addresses (needs PDBs to symbolize). Task label, trail, and stack are
  captured at write time, so a line flushed after its task scope ends keeps its
  context. `core/LogTiming.h` (header-only) provides `ScopedLatencyWarn` /
  `logLatencyWarn` for latency-budget WARNs on hot paths; their messages flow
  through the same text-keyed rate limiter, so no per-callsite suppression state
  is needed.
- `redactSecrets` masks values of known credential key names (`token=`, `password=`,
  ...) on every written line, defensively - no call site logs secrets today. A
  line with no `=` returns before the lowercase copy and the per-key scans, since
  no key can match it and this runs under the mutex capture and encode share.
  Paths and window titles are NOT auto-redacted; instrumentation sites must avoid logging
  them (replay save lines log the output path, which embeds the foreground window
  title).
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
  index, so it needs the consumer stopped too. `clear` and `tryPop` assign into a
  default-constructed `T`, so `T` must be default-constructible and move- or
  copy-assignable; `static_assert`s in the header state both.
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
failure, diagnostics export, the archive fallback that keeps an unarchivable
previous log, the fixed `Trace` minimum level that keeps a trace line in the
log, and that a `recentLines` poll does not end the pending group.
existing suites also link `Logger.cpp`.

## Child DOX Index

None. This folder's children are files only.