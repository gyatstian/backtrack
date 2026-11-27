# clips

## Purpose

The clip library on disk: listing what is in the clip directory, and the rename,
delete, favorite, and tag operations the UI offers.

## Ownership

- Owns: `ClipManager`, `ClipInfo`, `ClipStorage.h`'s shared storage-root rules,
  and the sidecar file layout (favorite markers, tag files).
- Does not own: writing new clips (`mux`), the directory choice
  (`AppSettings::clipDirectory`, `AppSettings::organizeClipsByGame`), or the UI's list and gallery views.

## Local Contracts

- `ClipManager` owns the sidecar layout. A clip is a `.mp4` file, and its
  sidecars are named after the **full file name**: `<name>.mp4.favorite` (an
  empty file whose existence means favorite) and `<name>.mp4.tags` (one
  normalized tag per line). Neither carries the hidden attribute; only the
  muxer's output reservation does. `renameClip` attempts to move both sidecars
  with the media. `listClips` reads sidecar presence from the same directory
  pass and opens only `.tags` files that exist.
- `isManagedClipPath` gates every operation that touches disk, not just
  `removeClip` and `renameClip`: `setFavorite` and `addTag` go through it too.
  A path passes only when it is non-empty, ends in `.mp4` (case-insensitive),
  canonicalizes to an existing regular file, and sits strictly inside the
  canonicalized `directory_`. That is the only thing standing between a
  malformed path and deleting a user's unrelated file. A path that does not yet
  exist is refused, so the gate cannot be used to authorize a create.
- `ClipStorage` defines the library root, its immediate game folders, and each
  game's legacy `clips` child as clip and recovery output roots. it skips child
  reparse points and escapes dot-prefixed generated game names. existing dot
  folders remain discoverable because they can contain game clips. output-name
  reservation avoids existing media files and orphaned sidecars.
- `listClips()` scans those roots non-recursively and keeps regular, nonempty
  `.mp4` files. the one pass over the library root also discovers the game
  folders and their legacy `clips` children, under the same rules
  `ClipStorage::clipRoots` applies to recovery discovery (no reparse points, one
  level deep); keep the two in step. legacy `.publish[.n].mp4` files directly in
  `.backtrack_tmp` are excluded because they can be incomplete. results are
  sorted by modified time then filename descending; stale duration cache entries
  are dropped. it accepts a cancellation predicate, checked before each
  directory entry, root, and final result; a cancelled scan returns empty.
  cancellation cannot interrupt a shell property read already in progress.
- `listContents()` is the folder-browsing view of the same rules: the clips
  directly inside the current directory plus one `ClipFolder` per immediate
  subdirectory, sorted by name. dot-prefixed folders are omitted (and skipped
  by the recursive clip count) by default unless `includeHiddenFolders` is set;
  `listClips()` stays unchanged so legacy dot folders remain discoverable for
  flat listing and recovery. each folder's `clips` is a recursive count of the
  clips below it, so a folder row can say how much it holds without listing it.
  `scanDirectory` is the shared pass behind both; the legacy `clips` child is
  only folded in by `listClips`, never by `listContents`, so a folder row never
  points at a legacy child of another row.
- Durations come from the shell `System.Media.Duration` property, which opens a
  COM handler per file and parses the MP4 `moov` atom. The read is
  `GPS_BESTEFFORT` on purpose: `GPS_FASTPROPERTIESONLY` skips the MP4 handler
  and reports zero for every clip. That is why durations are cached per file
  identity (size plus modified time). Do not remove the cache to "simplify" the
  loop.
- The cache is not locked: calls on one `ClipManager` instance must be
  serialized. The UI scan worker owns its own instance; `setDirectory` and
  `listClips` on the window's instance stay on the UI thread.
- `removeClip` sends the media file to the Recycle Bin (`IFileOperation` on a
  short-lived sta thread) and only removes the sidecars, permanently, once the
  media is gone, so a clip that survives a failed delete (locked file, sharing
  violation) keeps its favorite and tags. a drive without a Recycle Bin gets the
  shell's permanent-delete warning; declining it is a failure. only an
  unavailable `IFileOperation` falls back to a permanent unlink.
- `clipRenameTarget` and `clipRenameCollides` are the rename rules; the ui
  validates with them before queueing a rename. a stem is rejected when empty
  or whitespace-only, containing `\ / : * ? " < > |` or a control character,
  ending in a dot or space, or naming a reserved device (`CON`, `PRN`, `AUX`,
  `NUL`, `COM1`-`COM9`, `LPT1`-`LPT9`). a trailing source extension is dropped
  and the source extension kept. the same name is a successful no-op; a
  case-only change is not a collision. the media moves with an atomic
  fail-if-exists operation; sidecars move only after it succeeds. a sidecar
  that fails to move is logged, not rolled back; the media rename still reports
  success. existing destination sidecars may be replaced, and an orphan
  destination sidecar is removed when the source has none.
- `setFavorite` creates an empty marker exclusively or leaves an existing file
  untouched. `addTag` trims, strips control characters, caps the tag at 64
  characters, drops empties, and replaces the whole tag entry through an
  exclusively created sibling. existing symlink and hardlink referents are not
  written through. there is no remove-tag path.
- the muxer stages each MP4 inside an owned session before publication. the
  output name is held by a hidden zero-byte reservation (`reserveClipOutput` in
  `clips/ClipStorage.h` and `removeEmptyOutputReservation` in
  `mux/Mp4Muxer.h`); legacy ambiguous reservations are retained.

## Work Guidance

- Tag and favorite operations are metadata only. Never rewrite or transcode a
  clip from this folder.
- Keep new sidecars behind the same full-file-name convention. A sidecar named
  after the stem instead of the file name is invisible to `listClips`.

## Verification

`SettingsClipGuardTests` (CTest) links this folder's `ClipManager.cpp` and
covers the managed-path gate through `removeClip`: `..` traversal, a sibling
directory, an absolute path outside the directory, a non-`.mp4` file inside it,
a path that does not exist, and the empty path. Extend it when the gate's
conditions change.

`ClipManagerTests` (CTest) covers linked sidecars and staging collisions, failed
tag publication, a destination created between rename precheck and move, normal
rename, nonfatal failures of either sidecar move, mixed-case clip and staging
names, supported clip roots, scan cancellation, rename name rules (no-op,
case-only, typed extension, control characters, orphan sidecars), and scan
sidecar flags. symbolic link cases skip
when link creation is unavailable. `StorageLayoutTests` (CTest) covers the
shared root rules, and `SettingsClipGuardTests` covers the listing rules.
legacy clip names matching `.publish[.n].mp4` directly in `.backtrack_tmp` are
ambiguous and remain unlisted.

The rest is manual: record a clip, then exercise list, gallery, rename (with
favorite and tags intact), favorite toggle, and delete against both an empty and
a populated clip directory, including one containing files Backtrack did not
create and clips in per-game subfolders (with the game-folders setting on) and
the `clips` subdirectory. check that delete lands in the Recycle Bin.

## Child DOX Index

None. This folder's children are files only.
