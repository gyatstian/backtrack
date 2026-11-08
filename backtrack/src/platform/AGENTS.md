# platform

## Purpose

Shared Win32 helpers used by several domains: monitor and DXGI adapter
enumeration, hardware-encode adapter selection, foreground window and
application queries, MMCSS thread profiles, high-resolution timing, app paths,
Windows startup registration, microphone privacy cleanup, and the constants
`app` and `ui` share.

## Ownership

- Owns: everything in `platform/Win32Util.*`.
- Does not own: any pipeline behavior. This folder answers questions about the
  environment; it does not capture, encode, or record.

## Local Contracts

- Before adding a Win32 helper somewhere else, check whether it belongs here.
  These are shared precisely because monitor indexing, MMCSS registration, and
  timer resolution have to agree across modules. An existing exception:
  `audio/WasapiCapture.cpp` registers the `Pro Audio` MMCSS profile through its
  own scoped wrapper because the profile name is chosen at the call site.
  String helpers that two modules must agree on byte-for-byte belong here too -
  see `normalizedPathKey`.
- Monitor index is a cross-module contract: `enumerateMonitors` returns entries in
  `EnumDisplayMonitors` order, `monitorFromIndex` indexes that same order, and
  `AppSettings::monitorIndex` stores it. Reordering the enumeration silently
  changes which monitor the user's setting means.
- `enumerateMonitors` reports native pixel size and the primary flag. It does not
  apply a scaling factor; do not add one here.
- `monitorHasAdvancedColor` is best effort: true only for
  `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020` (PQ) or
  `DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709` (linear scRGB), false when the
  output cannot be resolved (no `IDXGIOutput6`, failed `GetDesc1`, anything
  else). Callers treat SDR as the safe default, which is why it
  must never be allowed to report HDR optimistically.
  `DesktopDuplicationCapture::queryAdvancedColor` uses the same predicate.
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
  frames, not the freshest. `enableMmcssForThread(taskName, priority)` is the
  general entry for any other profile.
- `foregroundApplicationName` prefers the exe's version-resource `FileDescription`
  (then `ProductName`), falling back to the exe stem and then the window title.
  Chromium forks often keep the upstream exe name while branding the version
  resource, so the stem alone misnames them. It feeds replay clip names (and,
  when per-game folders are on, their parent folder) and Discord Rich Presence;
  all change together. Both foreground queries share one validity gate (gone,
  hidden, minimized, or ours is neither a game to name nor a surface to test),
  so the naming and the fullscreen test can never disagree on what is foreground.
- `updateWindowsStartupRegistration` and `pruneStaleMicrophoneConsentEntries`
  both write to HKCU and nowhere else: the `Run` and `StartupApproved\Run` keys,
  and the microphone `CapabilityAccessManager\ConsentStore`. `Application::run`
  calls both once, and the UI calls `updateWindowsStartupRegistration` again
  whenever the setting is saved. Keep them cheap and failure-tolerant: every
  path logs and returns, none throws.
- `kBacktrackMainWindowClassName` and `backtrackActivationMessage()` are the two
  symbols both `app` and `ui` depend on. `kBacktrackSingleInstanceMutexName` and
  `kBacktrackStartupArgument` are `app`'s alone. Changing a shared one is a
  cross-module change.
- `trimText` and `normalizedPathKey` are `inline` in the header, not exported
  from the `.cpp`: `SettingsClipGuardTests` links `SettingsStore.cpp` with
  `defaultClipDirectory()` stubbed precisely so `Win32Util.cpp` (DXGI, avrt,
  winmm) stays out, so an out-of-line definition would drag that whole file into
  the suite. Keep them header-only.
- `normalizedPathKey` is a cross-module contract, not a formatting helper.
  `WasapiCapture` keys the live audio session by it, `RecorderController` and
  the UI key the user's requested mute by it, and `SettingsStore` dedupes the
  stored app list by it. Those keys have to match or the checkbox is on, the app
  is listed, and audio still plays into the recording - with no error and no log
  line. Do not extend what it normalizes (environment expansion, 8.3 short
  names, trimming) without changing every key at once.
- `ui/UiHelpers.h`'s former `trimText` / `executableKey` are gone; both live
  here now. `UiHelpers.cpp` includes this header for them.

## Work Guidance

- These are stateless free functions whose only project dependencies are
  `core/Logger.h` (for `hresultToString`) and `core/Types.h`. Keep it that way; a
  helper that needs `RecorderController` belongs elsewhere.
- Return plain Win32 types and `bool`. No exceptions across this boundary.

## Verification

No automated suite, and none will link `Win32Util.cpp` as it stands: it drags
DXGI, avrt and winmm into the test, so `SettingsClipGuardTests` stubs
`defaultClipDirectory()` instead. Manual: build, then confirm monitor selection
and multi-monitor support in the app, that startup registration adds and
removes the run entry, and that no exception fires at startup with the
diagnostics log.

## Child DOX Index

None. This folder's children are files only.