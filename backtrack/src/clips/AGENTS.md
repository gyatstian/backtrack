# clips

## Purpose

The clip library on disk: listing what is in the clip directory, and the rename,
delete, favorite, and tag operations the UI offers.

## Ownership

- Owns: `ClipManager`, `ClipInfo`, and the sidecar file layout of the clip
  directory (favorite markers, tag files).
- Does not own: writing new clips (`mux`), the directory choice
  (`AppSettings::clipDirectory`), or the UI's list and gallery views.

## Local Contracts

- `ClipManager` owns the directory layout. A clip is `<stem>.mp4` plus sidecar
  files next to it: a hidden favorite marker and a tag file. Anything that
  creates or renames a clip must move or recreate those sidecars with it.
- `isManagedClipPath` gates every destructive operation. `removeClip` and
  `renameClip` must refuse paths outside the managed directory; this is the only
  thing standing between a malformed path and deleting a user's unrelated file.
- `listClips()` runs on the UI thread. Durations come from a shell property read
  that opens a COM handler per file, so they are cached per file identity (size
  plus modified time). Do not remove the cache to "simplify" the loop.
- The cache is not locked because `listClips` is single-threaded by contract. If
  you call it from anywhere else, add locking there rather than here.
- A clip the muxer is still publishing into can appear in the list while it is
  incomplete. The staging and reservation handling (`mux/RecoveryStore.h`,
  `removeEmptyOutputReservation`) is what keeps that from mattering; do not
  second-guess a file whose size is still changing.

## Work Guidance

- Prefer updating `mtime` and size over rescanning: they are what identifies the
  cache entry.
- Tag and favorite operations are metadata only. Never rewrite or transcode a
  clip from this folder.

## Verification

No automated suite. Manual check: record a clip, then exercise list, gallery,
rename (with favorite and tags intact), favorite toggle, and delete against both
an empty and a populated clip directory, including one containing files
Backtrack did not create.

## Child DOX Index

None. This folder's children are files only.