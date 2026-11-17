# ui

## Purpose

The native Win32 main window: the four tabs and their pages, layout, message
handling, the settings editors, the clip library, diagnostics, and the tray
icon.

## Ownership

- Owns: everything in `MainWindow.h` and the `MainWindow*.cpp` split,
  `DiagnosticsLog.*`, `UiHelpers.*`, `UiIds.h`, and `ClipDragDrop.cpp`.
- Does not own: pipeline behavior (`app`), clip file operations (`clips`), or
  the settings file (`settings`). It edits an `AppSettings` value and hands it
  to `RecorderController::updateSettings` through the controller queue.

## Local Contracts

- One class, split by concern across files. Put new code in the file that owns
  the concern:
  - `MainWindow.cpp` window creation, message loop, controller worker, update
    check
  - `MainWindowMessages.cpp` `windowProc`, subclass procs, custom drawing
  - `MainWindowPages.cpp` page and settings-category construction, page cache
  - `MainWindowLayout.cpp` layout, scrolling, wheel handling, single-control
    teardown (`detachPageControl`)
  - `MainWindowSettings.cpp` read/apply/dirty tracking, status-bar help text
  - `MainWindowLibrary.cpp` clip list, thumbnails, drag and drop
  - `MainWindowDiagnostics.cpp` stats display, log saving
  - `MainWindowController.cpp` hotkeys, tray, auto record, game integrations,
    keyboard shortcuts
  - `ClipDragDrop.cpp` drag-and-drop shell plumbing
- `DiagnosticsLog` requires the live logger to flush before copying a bounded
  snapshot to an exclusively created sibling; it replaces the selected
  destination by handle and only attempts to remove its own staging file on
  failure. it rejects the live log and its aliases as destinations. the
  diagnostics tail is best-effort. the saved log tail appends the session
  runtime snapshot (encoder, drop and replay counters) so a triage copy
  carries the numbers that matter.
- Control ids, timers, messages, tray ids and the shared theme colors live in
  `UiIds.h`. Never inline a control id or a color in a `.cpp`; a literal cannot
  be found, renamed, or themed. Per-page row geometry (label and control x,
  row height, section gaps) stays a function-local `constexpr` in the builder.
- `captureBackendDisplayName` is `platform/Win32Util.h`'s, and both
  `app/RecorderController.cpp` and the Diagnostics page call it rather than
  keeping their own copies: the backend name lands in
  `captureBackendStatus`, which the Diagnostics page writes to the same log the
  status bar shows. Two copies of that switch meant the status line and the
  diagnostics export could name the same backend differently.
- `trimText` and `normalizedPathKey` are `platform/Win32Util.h`'s, not this
  folder's. The sound-separation and app lists here are keyed by
  `normalizedPathKey` and must key them the same way `WasapiCapture` does.
- `UiIds.h` hands out ids in ranges: tabs at `kTabButtonBaseId + index`,
  settings categories at `kSettingsCategoryButtonBaseId + index`, per-row buttons
  at `kSoundSeparationMuteButtonBaseId`, `kSoundSeparationRemoveButtonBaseId`
  and `kAutoRecordRemoveButtonBaseId` plus the row index. Single buttons are
  listed in `kSingleOwnerDrawButtonIds`, which `isButtonId` (`UiHelpers.cpp`)
  loops. `addControl` forces `BS_OWNERDRAW` on every non-checkbox button, so a
  new button id needs a table entry there plus its `WM_COMMAND` case, or
  nothing paints it. `WM_COMMAND`
  routes a range button through its `is*ButtonId` helper; `drawButtonItem`
  only special-cases the mute button's owner-drawn icon.
- `MainWindow::create` registers two window classes: the main window
  (`kBacktrackMainWindowClassName`, shared with `Application`) and a
  `BacktrackPageHost` child per page. hosts use `WS_EX_CONTROLPARENT` for
  dialog-key traversal; hidden hosts are excluded. the page host owns its own
  painting, scrolling and hover tracking, and forwards `WM_COMMAND`,
  `WM_NOTIFY`, `WM_MEASUREITEM`, `WM_DRAWITEM` and `WM_CTLCOLOR*` to the main
  window.
- replay tags are sidecars on the saved clip; kill tags do not move it into
  another directory.
- The UI thread owns every `HWND`. Work that can block (controller actions,
  library scans, thumbnail generation, the GitHub update check, the League and
  voice services) runs on a worker and reports back through a posted message
  (`kControllerActionCompleteMessage`, `kLibraryScanReadyMessage`,
  `kClipThumbnailReadyMessage`, `kUpdateCheckCompleteMessage`,
  `kLeagueKillDetectedMessage`, `kVoiceCommandMessage`). A worker must never
  touch a control. `kControllerActionCompleteMessage` carries a heap object in
  `lParam` that the UI thread takes ownership of; a failed post means the worker
  deleted it instead. Scan and thumbnail workers publish one replaceable result
  under their mutex and post a parameterless message, so a superseded result is
  never queued twice; shutdown signals and joins both workers before window
  destruction.
- the update check uses a cancellable async WinHTTP request with a total
  deadline. shutdown signals and joins its worker before window destruction;
  pending callbacks retain their request buffers and parent handles.
- notification playback keeps asynchronous sound buffers alive until replaced.
  custom WAV loading and its volume scaling happen outside the playback mutex;
  contended optional feedback is skipped rather than blocking the caller. The
  scaler operates on the exact `data` chunk extent only (trailing chunks stay
  byte-identical) and supports PCM8 and PCM16; a format it cannot scale is
  dropped so the built-in indicator plays at the configured volume instead of
  an unattenuated custom file.
- Controller actions are serialized through one worker thread and a busy timer
  (`queueControllerAction`, `kControllerBusySoftTimeoutMs`). The timer only
  rewrites the status line; it never clears the pending flag, so a second
  action is still rejected while one is in flight. Route a new long-running
  action through that queue rather than calling `RecorderController` from the UI
  thread.
- Each page owns a persistent host window, but only Diagnostics keeps its
  controls. `switchPage` tears the previous page down and rebuilds every other
  page (`storeCurrentPageCache`, `loadPageCache`, `invalidatePageCache`),
  because Library scan and thumbnail callbacks can arrive after a tab switch.
  Leaving Library cancels the scan and releases its thumbnail cache; the clip
  snapshot and selection stay, and returning rebuilds from them and starts a
  scan. `resetPageCache` clears cached state without replacing its host. any code
  that builds controls must also add its member pointers to
  `resetPageControlMembers` and rebind them in `rebindPageControlPointers`;
  settings-body teardown keeps its own subset so category buttons and Save
  Settings survive. a stale pointer makes `readVisibleSettingsInto`
  read a dead HWND and silently flip that setting back.
- Settings dirty tracking compares `AppSettings` structs through
  `computeSettingsDirty`, which is why every settings struct in `core/Types.h`
  has a defaulted `operator==`. Do not hand-roll a per-field comparison, and do
  not touch the filesystem from the dirty pass: it runs per keystroke, so the
  clip-folder and ring-folder checks live in `ensureFoldersExist`, which only
  the save path calls. `MainWindow` starts from the application's snapshot, not
  a second load. `commitSettings` advances the saved baseline and applies
  startup registration and ui preferences only after publication succeeds.
  failed saves retain drafts for retry; a failed startup read blocks writes
  until restart so defaults cannot replace an unreadable file. immediate logging
  enabled and library view commits overlay only their field on the saved snapshot.
  page construction never replaces that snapshot from the controller.
  Declining the unsaved-settings prompt restores the saved snapshot, invalidates
  both the Capture and Settings page caches and rebuilds the live one: the cache
  entries hold discarded drafts, and invalidation alone would leave the page the
  user refused to leave with no controls.
- The output and input device combos come from `deviceChoices`, which appends
  the saved device when enumeration does not list it. Row 0 is still Default.
  Without the appended entry an unplugged pinned device reads back as an empty
  id, and the next unrelated save rewrites the setting to Default, defeating
  `WasapiCapture`'s rule that an explicit id stays pinned and never falls back to
  the default device. Accepted cost: an unplugged
  pinned entry is labelled with its raw endpoint id, because that id is the only
  value persisted and no friendly name exists for an absent device.
- Dark theme is applied by hand: `applyDarkTheme` per control (DWM dark mode
  plus `SetWindowTheme`) and the owner-draw and `WM_CTLCOLOR*` handlers, which
  return the brushes created in `create`. There is no theming API to hook.
- Layout is proportional, not DPI-based. `addControl` records a design rect,
  and `layoutCurrentPage` scales x and width from it to the available column
  width with `MulDiv`; heights and fonts are never scaled. `WM_DPICHANGED` only
  adopts the suggested window rect and relayouts. Gallery and Diagnostics share
  `movePlacedControl`: a placed-cache hit returns false even if it unhides the
  control; showing the control must not be short-circuited by a successful move.
- Diagnostics bypasses the generic two-column layout: a dedicated branch in
  `layoutCurrentPage` places its three sections (Runtime, Encoder, Recent Log)
  side by side at equal width. Each body is a read-only multiline `EDIT` that
  wraps to the column and scrolls inside itself; the subclass drops
  `DLGC_WANTALLKEYS` so Tab still moves focus.
- Gallery tile geometry is not duplicated: the painter and the click handler
  both go through `galleryTileRect` / `galleryTileColumnAt`, and both use
  `kGalleryTileGap` (including `galleryItemHeightForWidth`, so the budgeted
  row height matches the drawn tile). `clipIndexAtPoint` additionally requires
  `PtInRect` on the tile, so gap clicks and the empty part of a partial last
  row resolve to no clip. The tile index (`selectedClipIndex_`) is the only
  selection source the painter reads; mouse clicks set it in `armClipDrag`,
  keyboard row changes follow to the row's first tile in
  `onClipSelectionChanged`.
- `refreshClips()` keeps the current snapshot and starts one serial scan
  worker; it never calls `ClipManager::listClips` on the UI thread. A scan is
  identified by generation and clip directory, and `handleLibraryScanReady`
  rejects a result whose generation or directory no longer matches, or whose
  page has gone. Re-refreshing replaces the pending request rather than queuing
  another. `LibraryScanResult::durationMs` is set by the scan worker so the
  ready handler can log scan timing. A single-clip action (favorite, delete, rename) updates that one
  `clips_` entry and repaints instead. An in-place update must carry over every
  field it does not itself change - notably `duration100ns`, which only the
  shell property read populates - and re-sort the vector when the entry's sort
  key moved. `ClipManager::listClips` sorts by modified time then filename
  descending; a rename preserves modified time but changes the name, so it
  re-sorts on that comparator and then re-reads `selectedClipIndex_`. Delete
  and rename re-queue missing thumbnails through
  `refreshThumbnailsAfterLibraryEdit`; an edit that keeps the working-set range
  keeps the generation so in-flight decodes survive, while a rescan, view
  switch, or range change replaces the queue and bumps it.
  `populateClipListRows` also rebuilds the painter's per-clip derived text
  (`clipDisplayText_`: filename, duration/size/tags, lowercase thumbnail key),
  so any path that mutates `clips_` must reach it; favorite is paint-only and
  does not.
- Muted labels are themed by `mutedLabels_` membership, never by reading the
  label text: `rebuildSoundSeparationRows` registers muted labels there,
  `detachPageControl` and the page-teardown clears unregister them. The set is
  not part of `PageCache`: safe only because Settings is never cache-reused
  (only Diagnostics keeps its controls) - a future change caching Settings
  must persist it too. The Library
  title label (`isLibraryTitleLabel`, resolved by its section-heading role)
  paints transparent text on the page background: the Library has no section
  cards, so a panel-colored box behind it is a stray rectangle.
- Mouse selection goes through `armClipDrag`, which sets the listbox cursor
  with `LB_SETCURSEL` (no notification follows) and then calls
  `onClipSelectionChanged` directly, so rename text and action-button state
  stay in sync on clicks, not just on keyboard selection messages.
- `detachPageControl` (`MainWindowLayout.cpp`) is the only way to destroy a
  single page control: it removes the HWND from `pageControls_`,
  `layoutItems_`, `statusHelpTexts_` and `mutedLabels_` before
  `DestroyWindow`. The file/manual pickers share `pickFileForOpen` /
  `pickFolder` (`UiHelpers.*`); do not reintroduce per-callsite dialog
  boilerplate.

## Work Guidance

- There is no UI framework and no layout engine. Adding a settings control means
  an id in `UiIds.h`, that same id in `isSettingsControlId` (`UiHelpers.cpp`) or
  it never marks the page dirty, an `addControl` call that records its design
  rect, a read in `readVisibleSettingsInto`, and a reset plus rebind if it gets a
  member pointer (see the page-lifetime contract above). use `addCheckbox` for
  checkbox creation, `addRowLabel` / `addRowHelp` for labeled settings rows;
  keep geometry in the builder.
- Enum combo boxes share one `{value, label}` table in `UiHelpers.h` between
  the page builder (`fillEnumCombo`) and the reader (`readEnumCombo`); extend
  the table when the enum grows, never raw-cast a combo index.
- `readVisibleSettingsInto` reads controls raw and never clamps:
  `sanitizeSettings` is the sole clamp (stash, save, and dirty all run through
  it). The follow-mouse read combines the
  `multiMonitorSupport` checkbox with the `kMonitorFollowComboEntries` index so
  a checked state is exactly what sanitize keeps. Following focus is not a
  setting: multi-monitor on records the focused monitor unless the combo picks
  the cursor.
- Preferences and behavior the user expects to persist go into `AppSettings`,
  not into a local variable in a page builder.
- `Clip overlap (seconds)` is the last setting in Settings -> Capture, not
  the Capture tab. its label and numeric field share this hover/focus footer
  help: "When saving again within the replay window, save only new footage plus this much of the previous clip."
- Copy in this folder is user-facing English in a deliberately plain voice.
  Keep it short and specific.
- Keyboard handling goes through `handleShortcut` in
  `MainWindowController.cpp`, which runs before `TranslateMessage` in
  `runMessageLoop`: it captures the hotkey when a hotkey edit has focus, and on
  the Library page handles F2 (rename), Enter inside the rename box, Delete,
  F5 / Ctrl+R (refresh) and Ctrl+O (open). `runMessageLoop` routes anything
  `handleShortcut` declines through `IsDialogMessageW`, so Tab/Shift+Tab moves
  focus app-wide; the hotkey capture lets Tab/Escape through for that reason.
  In Gallery with the clip list focused, arrows move per tile and Enter opens
  the clip; F2 with no selection only sets the status hint instead of stealing
  focus. Library Delete/Favorite/Rename buttons and the rename box enable only
  while a clip is selected (`updateLibraryActionButtons`).
- The tray Exit menu prompts to save dirty settings exactly like `WM_CLOSE`;
  either path may abort the exit. auto record uses applied controller settings,
  not drafts.
- The auto record poll walks the open-app list only until it owns a recording
  of a matched app; in that stable state it liveness-checks the stored process
  id plus its membership in the applied list, and falls back to the desktop
  walk only when the match is lost. The liveness check is
  `isProcessStillRunning`, which reads exit status rather than `OpenProcess`
  success: a launcher or debugger handle keeps an exited process openable. A
  failed `OpenProcess` (an unqueryable process) counts as gone, so the poll ends
  the stale auto-record rather than recording a window it cannot prove is live.
- Auto-record ownership follows the action, not the poll: `ControllerAction`
  and its result carry `automatic`, and `handleControllerActionComplete`
  reconciles through the pure `reconcileAutoRecordOwnership` /
  `reconcileAutoRecordAfterSettings` helpers before the next poll reads the
  state. A manual start/stop between two polls must not keep automatic
  ownership (a later target exit would end the user's recording), and a
  settings restart that stopped the recording must drop ownership without
  suppressing, or automatic recording silently never resumes.
- `applyVisibleSettings` checks the controller busy flag before persisting
  anything, so a save that loses the race cannot leave disk saved without the
  recorder ever receiving `ApplySettings`.
- The two executable lists share one row core (`refreshAvailableApps`,
  `buildAppRowCommon`, `destroyAppRowCommon`, `resetAppRowHover`,
  `pickAppExecutable`, `selectedAvailableApp`, `commitAppListEdit`): a third
  list reuses it instead of copying the pattern. Row teardown resets button
  hover and help-hover state before destroying the controls, mirroring
  `clearPageControls`; `clearPageControls` also clears `sectionPanels_`, and
  the Gallery layout branch clears them on entry, so a previous page's cards
  can never paint behind the Library. `rebuildSettingsCategoryBody` restores
  focus to the rebuilt counterpart of the focused control.
- bitmap scaling in `UiHelpers.cpp` leaves the source bitmap owned by its caller.
  `downscaleBitmapToFit` returns the original on failure and deletes it only
  after successfully creating a replacement; selection or blit failure must
  not consume the original.
- `loadClipThumbnails` and `updateThumbnailWorkingSet` bound thumbnail work and
  retention to the visible Gallery rows plus one overscan row, capped by
  `kMaxLibraryThumbnails` (48). The worker queue is replaced on every working-set
  change, the cache drops every key outside the new range, and a result is
  accepted only for the current generation and range. Leaving Library releases
  every thumbnail bitmap. Single-clip edits re-queue through
  `refreshThumbnailsAfterLibraryEdit`; an edit within the same range keeps the
  generation, a range change bumps it; picker cancellations stay silent but
  genuine dialog failures are logged (`pickFileForOpen` / `pickFolder`).
- Gap clicks in Gallery select nothing: `armClipDrag` records the pre-click
  listbox cursor and `onClipSelectionChanged` restores it instead of following
  to the row's first tile. Row-level rebuilds park focus on the owning combo
  (`parkFocusOnCombo`) when the focused control died with the rows.

## Verification

`LoggerDiagnosticsTests.cpp` covers log export without a window.
`MainWindowSettingsTests.cpp` covers settings publication, preferences,
applied auto record, absent-checkbox fallbacks, the monitor-follow gate,
clip overlap placement/help/read/dirty tracking and page-control reset, an
unplugged pinned audio device surviving an unrelated save while Default still
clears it, the liveness probe ignoring a retained handle to an exited
process, and the auto-record ownership reconciliation (settings-induced stop
restarts, manual stop/start between polls keeps manual ownership).
`MainWindowNotificationTests.cpp` covers busy feedback while loading or
playback is blocked, and the notification volume scaler: the exact data extent,
PCM8 and PCM16, and the built-in fallback for a format it cannot attenuate. `MainWindowUpdateCheckTests.cpp` covers
loopback success, slow progress, stop and destruction.
`MainWindowAccessibilityTests.cpp` exercises dialog-key traversal, settings-body
lifetime, shared row help, tab chrome, placed-layout memoization and page-cache
invalidation in a visible window; it needs an interactive desktop.
`MainWindowLibraryTests.cpp` covers a blocked scan with a responsive window and page change, stale scan rejection,
view switches keeping the snapshot/selection without a rescan, and the
thumbnail working-set budget. these suites are registered with CTest.
build, then exercise the affected page manually: settings
round-trip through a restart, hotkeys from another application, tray
restore/exit, library rename and delete, and Diagnostics.

## Child DOX Index

None. This folder's children are files only.