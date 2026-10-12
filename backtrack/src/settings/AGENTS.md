# settings

## Purpose

Persistence for `AppSettings`: reading and writing the settings file, clamping
untrusted values, and the enum name mappings that cross the file boundary.

## Ownership

- Owns: `SettingsStore`, `sanitizeSettings`, the pure `validateEncoderConfig`
  for the `VideoSettings` ranges, the hotkey-safety rule
  (`supportedHotkeyModifiers`, `isSafeBareHotkey`, `isSafeGlobalHotkey`), and
  the `xxxName` / `xxxFromName` pair for every settings enum except `LogLevel`.
- Does not own: the field definitions or their defaults (`core/Types.h`), the
  controls that edit them (`ui`), or `logLevelName` / `logLevelFromName`, which
  are `core/Logger.h` because the logger needs them too.

## Local Contracts

- The file is a flat `key=value` list with dotted keys (for example
  `library.viewMode`), UTF-8 without a BOM, at
  `%LOCALAPPDATA%\Backtrack\settings.ini` (`Application::run` constructs the
  store; `platform/localAppDataPath` owns the folder). There is no section
  syntax, no quoting, and no schema version field. `#` starts a comment, a line
  without `=` is ignored, and a repeated key keeps the last value.
- Parsing never throws and never trusts the file. Every read falls back to the
  current value of the struct, which starts at the `core/Types.h` default, so a
  missing or malformed key is not an error. `readUInt` accepts digits only and
  rejects anything above `uint32_t`. `readBool` accepts `1/true/yes/on` and
  `0/false/no/off` in any case, and `library.viewMode` matches `gallery` /
  `list` in any case. Enum reads go through the `xxxFromName` functions, which
  map an unrecognized name to a known value instead of failing the load.
- `sanitizeSettings` is the trust boundary. It runs on every load **and** on
  every save, so a hand-edited file is normalized on the next start and an
  out-of-range value can never reach disk. The `VideoSettings` ranges and the
  adaptive B/I-frame gates live in the pure `validateEncoderConfig`, which
  `sanitizeSettings` calls, so the load/save path and live encoder use share
  one expression per range. It clamps:
  - width to 16-7680 and height to 16-4320, both forced to an even value
  - fps 1-240, bitrate 1000-200000 kbps, GOP 1-30 s, AQ strength 1-15,
    reference frames 1-4, replay seconds 1-3600, frame queue limit 1-8
  - input and output volume 0-200 %, notification volume 0-100 %
  - `video.encoderLookaheadDepth` is capped at 31 with no lower bound
  It also masks hotkey modifiers, resolves derived settings (adaptive
  B-frames need B-frames, adaptive I-frames need lookahead, follow-mouse needs
  multi-monitor and follow-focused), caps each executable list at 128 entries,
  dedupes them by `platform/Win32Util.h:normalizedPathKey` (the same key the
  sound-separation mute list uses), fills a blank name from the path
  stem, and replaces an empty `clipDirectory` with `defaultClipDirectory()`.
  A new setting needs a clamp here if it can be out of range.
- A non-`Custom` `resolutionMode` **overwrites** `video.width` and
  `video.height` with the preset size. The stored dimensions only matter in
  `Custom` mode, so a hand-edited size under a preset is discarded on load.
- A duplicate hotkey chord is not an error but is unusable: when
  `saveReplay` resolves to the same chord as `startStop`, only `saveReplay` is
  reset to its default. A virtual key of 0 means unbound and is left alone.
- Save creates the settings directory when missing, then serializes into a
  sibling `settings.ini.tmp` and swaps it over the live file with
  `std::filesystem::rename`. The live file is only ever replaced whole, so a
  crash or an I/O failure cannot leave a partial one: the previous settings
  survive until the swap lands. This reversed an earlier decision to truncate
  and rewrite in one open. That truncated the file before the first byte was
  written, so a full disk or a lock from a sync client zeroed it, `load` then
  fell back to defaults for every key, and `Application::run` saved those
  defaults back - the whole configuration gone behind a "Settings saved"
  status line. Do not go back to a truncating open, and do not add slow work
  between the serialize and the swap: `save` runs on the UI thread. This is a
  process-crash guarantee, not a power-loss one: unlike the recovery manifest,
  `save` does not `FlushFileBuffers` the temp file or pass
  `MOVEFILE_WRITE_THROUGH`, so a power cut between the write and the swap can
  still land the rename over bytes that never reached the medium.
- MSVC's `std::filesystem::rename` is
  `MoveFileEx(MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING)`, so it
  replaces an existing `settings.ini` instead of failing because the
  destination is there. The `COPY_ALLOWED` flag is why the temp must stay a
  sibling: across volumes that flag degrades the swap to a non-atomic copy and
  delete, which would reintroduce the torn file this rule exists to prevent. A
  failed temp write or a failed swap logs at `Error` (never throttled) and
  removes the temp file, leaving no litter.
- `save` returns `void`. Reporting success would touch four call sites across
  `app` and `ui` for a failure the `Error` log already records; do not add a
  return value without a caller that acts on it.
- `load` does not warn about an empty or unreadable file. It has two callers
  and only the first one runs before the logger is initialized
  (`Application::run`; `MainWindow`'s constructor runs after). A warning is
  still not worth adding there: an empty file is the normal first-run state and
  after the destructive save-back above it is indistinguishable from a
  user who never had settings.
- Keys with a legacy spelling are still read so an upgrade does not reset a
  setting: `library.galleryView` for `library.viewMode`,
  `integrations.leagueOfLegends.autoKillClips`, `gpu.losslessIdleCoalescing`,
  and a boolean `integrations.discordRichPresence`. Unknown keys are ignored,
  which is what makes an outright rename silent.
- Every enum crossing the file boundary needs a `xxxName` / `xxxFromName` pair
  here, even when only a hand-edit produces the value today. The `xxxName` pair
  is the persisted spelling; the UI combo boxes carry their own display labels
  and select by enum value, so a name change is a file-format change while a
  label change is not.
- `Application::run` loads once at startup and immediately saves the same value
  back, which is what normalizes a hand-edited file through
  `sanitizeSettings`.

## Work Guidance

- Field keys are stable user-facing text. Renaming one silently resets that
  setting for existing users, so treat a rename as a migration: keep reading the
  old key.
- Defaults exist exactly once, in the `core/Types.h` struct definitions. Do not
  repeat them in the parse path.
- Keep `sanitizeSettings` exhaustive rather than defensive per call site; a
  second validation layer in the UI will drift from this one. The UI reads
  controls raw and lets `sanitizeSettings` clamp on stash, save, and dirty.
- Related: `core/AGENTS.md` records the five places a new setting must be added.

## Verification

`SettingsClipGuardTests` (CTest) links this folder's `SettingsStore.cpp` and
covers the clamp ranges, the resolution-preset override, enum fallbacks, app
list dedupe, and hotkey masking. Extend it when the clamp ranges or the hotkey
rule change.

The only `platform/Win32Util.h` symbols this folder uses are the `inline`
`trimText` and `normalizedPathKey`, plus `defaultClipDirectory()`, which the
suite stubs. `Win32Util.cpp` itself stays out of the build - that is why the two
helpers are header-only.

The publish rule is covered on observable disk state, not the internal call
sequence: `testFailedSavePreservesTheOldFileAndLeavesNoTemp` holds
`settings.ini` open so the swap fails and asserts the previous file survives
byte for byte with no staging file left, then that the same save lands once the
lock is gone; `testUtf8RoundTripsThroughSaveAndLoad` asserts a second save over
an existing file leaves no `.tmp` and still round trips through `load`, and that
no BOM is written.

Anything the suite cannot reach still needs a manual check: set a value in the
app, confirm it survives a restart, then hand-edit the file with an absurd value
(0 fps, 99999 bitrate, `F12` as a hotkey) and confirm the next start clamps it
and logs nothing fatal. A save while a sync client holds `settings.ini` open is
the only way to see the swap fail: the log must name the Win32 error and the
previous settings must still be there.

## Child DOX Index

None. This folder's children are files only.
