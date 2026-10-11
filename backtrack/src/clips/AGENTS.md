# clips

## Purpose

The clip library on disk: listing what is in the clip directory, and the rename,
delete, favorite, and tag operations the UI offers.

## Ownership

- Owns: `ClipManager`, `ClipInfo`, and the sidecar file layout of the clip
  directory (favorite markers, tag files).
- Does not own: writing new clips (`mux`), the directory choice
  (`AppSettings::clipDirectory`, `AppSettings::organizeClipsByGame`), or the UI's list and gallery views.

## Local Contracts

- `ClipManager` owns the sidecar layout. A clip is a `.mp4` file, and its
  sidecars are named after the **full file name**: `<name>.mp4.favorite` (an
  empty file whose existence means favorite) and `<name>.mp4.tags` (one
  normalized tag per line). Neither carries the hidden attribute; only the
  muxer's output reservation does. Anything that renames a clip must move both
  sidecars with it, which `renameClip` does.
- `isManagedClipPath` gates every operation that touches disk, not just
  `removeClip` and `renameClip`: `setFavorite` and `addTag` go through it too.
  A path passes only when it is non-empty, ends in `.mp4` (case-insensitive),
  canonicalizes to an existing regular file, and sits strictly inside the
  canonicalized `directory_`. That is the only thing standing between a
  malformed path and deleting a user's unrelated file. A path that does not yet
  exist is refused, so the gate cannot be used to authorize a create.
- `listClips()` runs on the UI thread, scans `directory_` and every immediate
  child directory except dot-directories (per-game folders from
  `AppSettings::organizeClipsByGame` and the legacy `clips` subdirectory),
  non-recursively, and keeps only regular `.mp4` files. `.backtrack_tmp`
  staging directories are skipped by the dot rule, so in-progress
  `.publish.mp4` files never list as clips. Zero-byte files are skipped: they
  are output-name reservations, not clips. Results are sorted by modified time
  descending, then filename descending. Cache entries for files not in the
  current listing are dropped so the cache cannot grow across deletes and
  renames.
- Durations come from the shell `System.Media.Duration` property, which opens a
  COM handler per file and parses the MP4 `moov` atom. The read is
  `GPS_BESTEFFORT` on purpose: `GPS_FASTPROPERTIESONLY` skips the MP4 handler
  and reports zero for every clip. That is why durations are cached per file
  identity (size plus modified time). Do not remove the cache to "simplify" the
  loop.
- The cache is not locked because `listClips` is single-threaded by contract.
  If you call it from anywhere else, add locking there rather than here.
- `removeClip` deletes the media file first and only removes the sidecars once
  the deletion is confirmed, so a clip that survives a failed delete (locked
  file, sharing violation) keeps its favorite and tags. The delete is a
  permanent unlink; there is no Recycle Bin and no confirmation inside
  `ClipManager`.
- `renameClip` validates the new stem before touching anything: it rejects an
  empty or whitespace-only stem, `\ / : * ? " < > |`, a trailing dot or space,
  and the reserved device names `CON`, `PRN`, `AUX`, `NUL`, `COM1`-`COM9`,
  `LPT1`-`LPT9`. It keeps the source extension and refuses when the target
  already exists. A sidecar that fails to move is logged, not rolled back; the
  media rename still reports success.
- `addTag` trims, strips control characters, caps the tag at 64 characters,
  drops empties, and rewrites the whole tag file. There is no remove-tag path.
- A clip the muxer is still publishing into can appear in the list while it is
  incomplete. Publishing is staged into `<stem>.publish[.n].mp4` and the
  output name is held by a hidden zero-byte reservation
  (`reserveClipOutputName` in `app/RecorderController.cpp`,
  `removeEmptyOutputReservation` in `mux/Mp4Muxer.h`, and the recovery sweep in
  `mux/RecoveryStore.h`) so an in-progress save never collides with a listed
  name. Do not second-guess a file whose size is still changing.

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

The rest is manual: record a clip, then exercise list, gallery, rename (with
favorite and tags intact), favorite toggle, and delete against both an empty and
a populated clip directory, including one containing files Backtrack did not
create and clips in per-game subfolders (with the game-folders setting on) and
the `clips` subdirectory.

## Child DOX Index

None. This folder's children are files only.
