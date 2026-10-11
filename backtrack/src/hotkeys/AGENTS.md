# hotkeys

## Purpose

Global start/stop recording and save-replay hotkeys that work while another
application has focus, including while a game does.

## Ownership

- Owns: `HotkeyService`, the hotkey ids, the keyboard hook, and the modifier
  tracking that goes with it.
- Does not own: what recording or saving does. The service only reports which
  hotkey fired; `MainWindowController.cpp` decides the action.

## Local Contracts

- Two mechanisms feed the same ids (`kStartStopId`, `kSaveReplayId`):
  `RegisterHotKey` first, and a `WH_KEYBOARD_LL` fallback for when the target
  already owns the combination. Games frequently do, which is why the fallback
  exists.
- The hook is process-wide and reaches the service through a static
  `activeService_`. Only one instance may be registered; `registerHotkeys`
  unregisters first so a settings change can re-register.
- Modifier state is tracked manually (`ctrlDown_`, `altDown_`, `shiftDown_`,
  seeded at registration) because a low-level hook cannot rely on
  `GetKeyState`. A one-shot (non-repeating) check plus a tick timestamp keeps a
  held key from firing repeatedly.
- Failures are not silent: `lastErrorMessage()` is surfaced in the UI when the
  hotkeys could not be registered.
- Hotkeys come from `AppSettings::hotkeys` and pass through
  `sanitizeSettings`, which masks unsupported modifiers and rejects combinations
  Windows reserves. Do not add a binding here that the sanitizer would reject.
- The handler must swallow the key only when it acted on it, so unrelated keys
  reach the foreground application unchanged.

## Work Guidance

- Keep the hook proc fast and allocation-free on the hot path; it runs on the
  input thread of the system.
- A new hotkey needs an id constant here, a `HotkeySettings` field, and a
  handler in `MainWindowController.cpp`.

## Verification

No automated test. Check that each hotkey fires while a different application is
focused, that a game holding the combination still reaches Backtrack through the
hook fallback, and that holding the key fires once.

## Child DOX Index

None. This folder's children are files only.