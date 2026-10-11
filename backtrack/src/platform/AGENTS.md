# platform

## Purpose

Shared Win32 helpers used by several domains: display and adapter enumeration,
MMCSS thread profiles, high-resolution timing, app paths, startup registration,
and the window class and single-instance constants.

## Ownership

- Owns: everything in `platform/Win32Util.*`.
- Does not own: any pipeline behavior. This folder answers questions about the
  environment; it does not capture, encode, or record.

## Local Contracts

- Before adding a Win32 helper somewhere else, check whether it belongs here.
  These are shared precisely because monitor indexing, MMCSS registration, and
  timer resolution have to agree across modules.
- Monitor index is a cross-module contract: `enumerateMonitors` returns entries in
  `EnumDisplayMonitors` order, `monitorFromIndex` indexes that same order, and
  `AppSettings::monitorIndex` stores it. Reordering the enumeration silently
  changes which monitor the user's setting means.
- `enumerateMonitors` reports native pixel size and the primary flag. It does not
  apply a scaling factor; do not add one here.
- `monitorHasAdvancedColor` is best effort and returns false when the output
  cannot be resolved. Callers treat SDR as the safe default, which is why it
  must never be allowed to report HDR optimistically.
- `dxgiOutputForMonitor` searches every adapter, and the returned
  `DxgiOutputLocation` names the adapter a D3D device must be created on for
  Desktop Duplication to work. Adapter choice is not interchangeable with the
  encoder adapter.
- `HighResolutionTimerScope` changes a process-wide setting. Scope it to an
  active capture or encode session; leaving it raised while idle costs power and
  the win is not worth it.
- MMCSS helpers return a `HANDLE` the caller must pass to
  `disableMmcssForThread`. `enableMmcssForCaptureThread` and
  `enableMmcssForEncodeThread` exist because the encode loop specifically needs
  to stay above background load so its drop-newest queue sheds the oldest replay
  frames, not the freshest.
- `updateWindowsStartupRegistration` and `pruneStaleMicrophoneConsentEntries`
  write to HKCU and to local app data. They run once at startup from
  `Application::run`; keep them cheap and failure-tolerant.
- The constants `kBacktrackMainWindowClassName`,
  `kBacktrackSingleInstanceMutexName`, and `kBacktrackStartupArgument` are
  consumed by both `Application` and `ui`. Changing one is a cross-module change.

## Work Guidance

- These are stateless free functions with no dependency on the rest of the app.
  Keep it that way; a helper that needs `RecorderController` belongs elsewhere.
- Return plain Win32 types and `bool`. No exceptions across this boundary.

## Verification

No automated suite. Manual: build, then confirm monitor selection and multi
monitor support in the app, that startup registration adds and removes the run
entry, and that no exception fires at startup with the diagnostics log.

## Child DOX Index

None. This folder's children are files only.