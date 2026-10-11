# ui

## Purpose

The native Win32 main window: tabs and pages, layout, message handling, the
settings editors, the clip library, diagnostics, and the tray icon.

## Ownership

- Owns: everything in `MainWindow.h` and the `MainWindow*.cpp` split,
  `UiHelpers.*`, `UiIds.h`, and `ClipDragDrop.cpp`.
- Does not own: pipeline behavior (`app`), clip file operations (`clips`), or
  the settings file (`settings`). It edits an `AppSettings` value and hands it
  to `RecorderController::updateSettings`.

## Local Contracts

- One class, split by concern across files. Put new code in the file that owns
  the concern:
  - `MainWindow.cpp` window creation, message loop, controller worker, update check
  - `MainWindowMessages.cpp` `windowProc`, subclass procs, custom drawing
  - `MainWindowPages.cpp` page and settings-category construction, page cache
  - `MainWindowLayout.cpp` layout, scrolling, wheel handling
  - `MainWindowSettings.cpp` read/apply/dirty tracking, tooltips, status text
  - `MainWindowLibrary.cpp` clip list, thumbnails, drag and drop
  - `MainWindowDiagnostics.cpp` stats display, log saving
  - `MainWindowController.cpp` hotkeys, tray, auto record, game integrations
  - `ClipDragDrop.cpp` drag-and-drop shell plumbing
- Every control id, layout constant, timer, message, and color lives in
  `UiIds.h`. Never inline an id or a hex color in a `.cpp`; a literal cannot be
  found, renamed, or themed.
- The UI thread owns every `HWND`. Work that can block (controller actions,
  thumbnail generation, the GitHub update check) runs on a worker and reports
  back through a posted message (`kControllerActionCompleteMessage`,
  `kClipThumbnailReadyMessage`, `kUpdateCheckCompleteMessage`,
  `kLeagueKillDetectedMessage`, `kVoiceCommandMessage`). A worker must never
  touch a control.
- Controller actions are serialized through one worker thread and a busy timer
  (`queueControllerAction`, `kControllerBusySoftTimeoutMs`). Route a new
  long-running action through that queue rather than calling
  `RecorderController` from the UI thread.
- Pages are cached: switching pages tears down controls and rebinds pointers
  (`storeCurrentPageCache`, `loadPageCache`, `rebindPageControlPointers`). Code
  that builds controls must run the rebind, or it will use stale pointers.
- Settings dirty tracking compares `AppSettings` structs through
  `computeSettingsDirty`, which is why every settings struct in `core/Types.h`
  has a defaulted `operator==`. Do not hand-roll a per-field comparison.
- Dark theme colors are centralized in `UiIds.h` and applied manually via
  `applyDarkTheme` and the subclass procs. There is no theming API to hook.
- The window class name and single-instance mutex name are constants in
  `platform/Win32Util.h` and are part of the contract with `Application`.

## Work Guidance

- There is no UI framework and no layout engine. Adding a control means an id, a
  build function, a layout entry, and a read in `MainWindowSettings.cpp`.
- Preferences and behavior the user expects to persist go into `AppSettings`,
  not into a local variable in a page builder.
- Copy in this folder is user-facing English in a deliberately plain voice.
  Keep it short and specific.

## Verification

No automated UI test. Build, then exercise the affected page manually: settings
round-trip through a restart, hotkeys from another application, tray
restore/exit, library rename and delete, and the Diagnostics page.

## Child DOX Index

None. This folder's children are files only.