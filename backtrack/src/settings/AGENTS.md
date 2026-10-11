# settings

## Purpose

Persistence for `AppSettings`: reading and writing the settings file, clamping
untrusted values, and the enum name mappings shared with the UI.

## Ownership

- Owns: `SettingsStore`, `sanitizeSettings`, and every
  `xxxName` / `xxxFromName` pair.
- Does not own: the field definitions or their defaults (`core/Types.h`), or the
  controls that edit them (`ui`).

## Local Contracts

- The file is a flat `key=value` list with dotted keys (for example
  `library.viewMode`), UTF-16, at `%LOCALAPPDATA%\Backtrack\settings.ini`. There
  is no section syntax and no schema version field.
- Parsing never throws and never trusts the file. Every read has a fallback
  that comes from the struct default, malformed values fall back rather than
  abort, and `readBool` accepts `1/true/yes/on` and `0/false/no/off` in any case.
- `sanitizeSettings` is the trust boundary. It runs on every load and must clamp
  anything a hand-edited or corrupt file could push out of range (dimensions to
  even values, fps, bitrate, GOP length, AQ strength, reference frames, replay
  seconds, frame queue limit, volume percentages), dedupe the sound-separation and
  auto-record app lists by name, mask unsupported hotkey modifiers, and reject
  hotkeys Windows reserves (`isSafeGlobalHotkey`). A new setting needs a clamp
  here if it can be out of range.
- Defaults exist exactly once, in the `core/Types.h` struct definitions. Do not
  repeat them in the parse path.
- `save` truncates and rewrites the live file in one open, so keep it to a
  serialize-and-close with no slow work in between.
- Every enum crossing the file boundary has a `Name` / `FromName` pair here,
  and the UI combo boxes are driven by those same names. A new enum needs the
  pair even if the value is only written by hand today.
- `Application::run` loads once at startup and immediately saves the same value
  back, which normalizes a hand-edited file through `sanitizeSettings`.

## Work Guidance

- Field keys are stable user-facing text. Renaming one silently resets that
  setting for existing users, so treat a rename as a migration.
- Keep `sanitizeSettings` exhaustive rather than defensive per call site; a
  second validation layer in the UI will drift from this one.
- Related: `core/Types.h` records the five places a new setting must be added.

## Verification

No dedicated suite. Practical check: set a value in the app, confirm it survives
a restart, then hand-edit the file with an absurd value (0 fps, 99999
bitrate, `F12` as a hotkey) and confirm the next start clamps it and logs
nothing fatal.

## Child DOX Index

None. This folder's children are files only.