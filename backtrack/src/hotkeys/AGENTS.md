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
  `RegisterHotKey` (with `MOD_NOREPEAT`), and a `WH_KEYBOARD_LL` hook. Both are
  installed whenever a hotkey is bound; they are not alternatives. A virtual key
  of `0` means unbound and is skipped by both.
- Install the hook unconditionally, before `RegisterHotKey`, and do not gate it
  on `RegisterHotKey` failing. A fullscreen game or its anti-cheat installs its
  own low-level hook and can consume a key without chaining
  `CallNextHookEx`, which stops this hook from ever running; the same game stops
  owning the hook once minimized, so the hotkey comes back. That is a reported
  fullscreen replay-save failure, so the hook is the mechanism that works when
  `RegisterHotKey` alone is not enough. `shouldHandleHotkeyMessage` then drops
  the duplicate within `kRegisteredDuplicateWindowMs` when both fire.
- The hook is process-wide and reaches the service through a static
  `activeService_`. Only one instance may be registered; `registerHotkeys`
  unregisters first so a settings change can re-register. It is skipped only
  when no hotkey is bound at all. The destructor clears `activeService_` when it
  still points at the dying instance: `WM_DESTROY` unregisters first today, but
  the hook proc runs on the system's input thread and would otherwise be left
  dereferencing freed memory.
- Modifier state is tracked manually (`ctrlDown_`, `altDown_`, `shiftDown_`,
  seeded at registration) because a low-level hook cannot rely on
  `GetKeyState`. A one-shot (non-repeating) check plus a tick timestamp keeps a
  held key from firing repeatedly.
- The hook proc always chains through `CallNextHookEx` and never consumes a key,
  so it cannot suppress anything. A hotkey fired this way reaches the
  foreground application as well as Backtrack.
- A `RegisterHotKey` failure on its own is not an error: the hook already
  covers it, so the message is logged at `warning` and `lastErrorMessage()` stays
  empty. `lastErrorMessage()` is only filled, and `registerHotkeys` only returns
  false, when the hook install failed too. The UI shows it through
  `withHotkeyWarning`.
- Hotkeys come from `AppSettings::hotkeys`; the defaults live in `HotkeySettings`
  (`core/Types.h`) and nowhere else. `sanitizeSettings` masks unsupported
  modifiers, rejects combinations Windows reserves, and rejects a save-replay
  binding identical to the start/stop one. Do not add a binding here that the
  sanitizer would reject.

## Work Guidance

- Keep the hook proc fast and allocation-free on the hot path; it runs on the
  input thread of the system.
- A new hotkey needs an id constant here, a `HotkeySettings` field, a
  `sanitizeSettings` rule, and a handler in `MainWindowController.cpp`.

## Verification

No automated test. Check that each hotkey fires while a different application is
focused, that a game holding the combination still reaches Backtrack through the
hook fallback, and that holding the key fires once.

## Child DOX Index

None. This folder's children are files only.