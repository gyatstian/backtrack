# settings

## Purpose

Persistence for `AppSettings`: reading and writing the settings file, clamping
untrusted values, and the enum name mappings that cross the file boundary.

## Ownership

- Owns: `SettingsStore`, `sanitizeSettings`, the pure `validateEncoderConfig`
  for the `VideoSettings` ranges, the hotkey-safety rule
  (`supportedHotkeyModifiers`, `isSafeBareHotkey`, `isSafeGlobalHotkey`), and
  the `xxxName` / `xxxFromName` pair for every settings enum.
- Does not own: the field definitions or their defaults (`core/Types.h`), the
  controls that edit them (`ui`), or the logger's on/off state (`core/Logger.h`).

## Local Contracts

- The file is a flat `key=value` list with dotted keys (for example
  `library.viewMode`), UTF-8 without a BOM, at
  `%LOCALAPPDATA%\Backtrack\settings.ini` (`Application::run` constructs the
  store; `platform/localAppDataPath` owns the folder). There is no section
  syntax, no quoting, and no schema version field. `#` starts a comment, a line
  without `=` is ignored, and a repeated key keeps the last value.
- `load` reports `Missing`, `Failed`, or `Loaded` through its optional status
  argument. missing files and failed reads return sanitized defaults; a read
  error discards the entire partial input. an empty file is loaded successfully.
  the read excludes concurrent writers. every key falls back to the current
  struct value, so a missing or malformed key is not an error. `readUInt` accepts digits only and
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
  stem, replaces CR/LF in names with spaces, and replaces an empty
  `clipDirectory` with `defaultClipDirectory()`. a new setting needs a clamp
  here if it can be out of range.
- A non-`Custom` `resolutionMode` **overwrites** `video.width` and
  `video.height` with the preset size. The stored dimensions only matter in
  `Custom` mode, so a hand-edited size under a preset is discarded on load.
- an invalid or duplicate `saveReplay` chord falls back to its default, or is
  unbound when that default matches `startStop`. a virtual key of 0 means
  unbound and is left alone.
- `save` creates the settings directory when missing and publishes the complete
  serialization through `clips/AtomicWrite.h`. the helper exclusively creates a
  fresh sibling, writes through its held handle, and replaces the destination
  entry by handle. it never truncates an existing staging file or writes through
  a linked destination; on failure it attempts to remove only its own staging
  file. a failed publish logs at `Error` and leaves the previous settings in
  place. old `settings.ini.tmp` files are not assumed to be owned.
- publication is a process-crash guarantee, not a power-loss guarantee: `save`
  does not flush file buffers or request write-through before replacement. do
  not add slow work between serialization and publication: `save` runs on the
  UI thread.
- `save` returns true only after the whole file is published. `normalizeLoaded`
  saves only a loaded snapshot and returns true when normalization is skipped.
  callers must retain drafts and baselines when publication fails.
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
- `Application::run` normalizes a successfully loaded file at startup. it
  does not create a missing file or save defaults after a failed read.

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
covers the clamp ranges, resolution presets, enum fallbacks, app-list dedupe,
and hotkey masking. `backtrack/tests/SettingsStoreTests.cpp` adds settings-only
regressions, including linked destinations and staging collisions, but is not
yet registered with CTest. symbolic link cases skip when link creation is
unavailable.

The only `platform/Win32Util.h` symbols this folder uses are the `inline`
`trimText` and `normalizedPathKey`, plus `defaultClipDirectory()`, which the
suite stubs. `Win32Util.cpp` itself stays out of the build - that is why the two
helpers are header-only.

The publish rule is covered on observable disk state: locked saves preserve the
previous file and can be retried; linked destinations and staging collisions
leave their external targets byte-exact. UTF-8 settings round trip without a BOM.

Manual check: save a value in the app and confirm it survives a restart; then
hand-edit an out-of-range value and confirm the next start clamps it. a failed
publish must log the Win32 error and leave the previous settings intact.

## Child DOX Index

None. This folder's children are files only.
