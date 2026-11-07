# clips

## Purpose

The clip library on disk: listing what is in the clip directory, and the rename,
delete, favorite, and tag operations the UI offers.

## Ownership

- Owns: `ClipManager`, `ClipInfo`, `ClipStorage.h`'s shared storage-root rules,
  `AtomicWrite.h`'s sibling-file publication helper, and the sidecar file
  layout (favorite markers, tag files). settings also uses the publication helper.
- Does not own: writing new clips (`mux`), the directory choice
  (`AppSettings::clipDirectory`, `AppSettings::organizeClipsByGame`), or the UI's list and gallery views.

## Local Contracts

- `ClipManager` owns the sidecar layout. A clip is a `.mp4` file, and its
  sidecars are named after the **full file name**: `<name>.mp4.favorite` (an
  empty file whose existence means favorite) and `<name>.mp4.tags` (one
  normalized tag per line). Neither carries the hidden attribute; only the
  muxer's output reservation does. `renameClip` attempts to move both sidecars
  with the media.
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
  `.mp4` files. legacy `.publish[.n].mp4` files directly in `.backtrack_tmp`
  are excluded because they can be incomplete. results are sorted by modified
  time then filename descending; stale duration cache entries are dropped. it
  accepts a cancellation predicate, checked before each directory entry, root,
  and final result; a cancelled scan returns empty. cancellation cannot
  interrupt a shell property read already in progress.
- Durations come from the shell `System.Media.Duration` property, which opens a
  COM handler per file and parses the MP4 `moov` atom. The read is
  `GPS_BESTEFFORT` on purpose: `GPS_FASTPROPERTIESONLY` skips the MP4 handler
  and reports zero for every clip. That is why durations are cached per file
  identity (size plus modified time). Do not remove the cache to "simplify" the
  loop.
- The cache is not locked: calls on one `ClipManager` instance must be
  serialized. The UI scan worker owns its own instance; `setDirectory` and
  `listClips` on the window's instance stay on the UI thread.
- `removeClip` deletes the media file first and only removes the sidecars once
  the deletion is confirmed, so a clip that survives a failed delete (locked
  file, sharing violation) keeps its favorite and tags. The delete is a
  permanent unlink; there is no Recycle Bin and no confirmation inside
  `ClipManager`.
- `renameClip` validates the new stem before touching anything: it rejects an
  empty or whitespace-only stem, `\ / : * ? " < > |`, a trailing dot or space,
  and the reserved device names `CON`, `PRN`, `AUX`, `NUL`, `COM1`-`COM9`,
  `LPT1`-`LPT9`. it keeps the source extension and moves the media with an atomic
  fail-if-exists operation; sidecars move only after the media succeeds. a
  sidecar that fails to move is logged, not rolled back; the media rename still
  reports success. existing destination sidecars may be replaced.
- `setFavorite` creates an empty marker exclusively or leaves an existing file
  untouched. `addTag` trims, strips control characters, caps the tag at 64
  characters, drops empties, and replaces the whole tag entry through an
  exclusively created sibling. existing symlink and hardlink referents are not
  written through. there is no remove-tag path.
- `atomicWriteSibling` retains an exclusive handle to its fresh staging file
  through publication and attempts cleanup by that handle on failure. it does
  not reuse or remove preexisting staging names.
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
rename, supported clip roots, and scan cancellation. symbolic link cases skip
when link creation is unavailable. `StorageLayoutTests` (CTest) covers the
shared root rules, and `SettingsClipGuardTests` covers the listing rules.
legacy clip names matching `.publish[.n].mp4` directly in `.backtrack_tmp` are
ambiguous and remain unlisted.

The rest is manual: record a clip, then exercise list, gallery, rename (with
favorite and tags intact), favorite toggle, and delete against both an empty and
a populated clip directory, including one containing files Backtrack did not
create and clips in per-game subfolders (with the game-folders setting on) and
the `clips` subdirectory.

## Child DOX Index

None. This folder's children are files only.
