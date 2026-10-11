# mux

## Purpose

Turning a recording into a playable file: the MP4 muxer and its Media Foundation
finalize step, the WAV writers for the audio tracks, and the crash recovery
store that keeps a killed recording recoverable.

## Ownership

- Owns: `Mp4Muxer`, `RecoveryStore`, `WavWriter`,
  `removeEmptyOutputReservation`.
- Does not own: the audio mixing decision (`audio/AudioMix.h`), the output name
  reservation policy (that lives in `RecorderController::nextClipPath`), or the
  replay buffer that feeds it.

## Local Contracts

- Recording is two stages. `startRecording` opens an elementary video stream and
  temporary WAVs; `finalize` muxes them into the MP4 and returns the published
  path. `abort` gives up without publishing.
- Publishing is staged: mux into `<stem>.publish[.n].mp4`, then move into place.
  A sharing violation (antivirus, indexer, Explorer preview) rotates the staging
  variant instead of failing the save. `stagingVariant` is zero-based and retry
  wrappers own the rotation.
- Incremental crash recovery is a contract, not an optimization:
  `appendManifestSampleLocked` appends one sample line to `recovery.tsv` and
  flushes at GOP boundaries, so a killed process loses at most the current
  partial GOP. Do not buffer manifest writes, and do not rename the temp
  directory without rewriting the manifest, or `recoverLatestFailedRecording`
  stops being able to rebuild the clip.
- `RecoveryStore::sweepOrphanedRecoveryData` is startup maintenance: it first
  gives the newest valid manifest a chance to become a clip, then deletes
  leftovers older than `maxAge` (24 h by default). It runs on a worker thread at
  startup and must not race with new recordings; `RecorderController` serializes
  it.
- Audio timeline rules:
  - PTS regression within one recording is warned about, not corrected.
  - A packet whose format differs from the WAV the writer opened is dropped, not
    appended. Appending wrong-rate bytes corrupts the timeline silently.
  - A temporary-WAV write error marks the recording failed so `finalize` reports
    it instead of publishing truncated audio.
- `removeEmptyOutputReservation` deletes only a hidden, zero-byte file, which is
  how a claimed clip name is distinguished from a user's own empty `.mp4`. Real
  content is never removed. Any change here risks deleting user data.
- `MuxedInputs` is the shared hand-off shape between the muxer, the recovery
  store, and `audio/AudioMix.h`. Extending it means updating all three.

## Work Guidance

- The staging and recovery paths exist because the app can be killed mid-save at
  any moment. Treat "the data is still recoverable" as the requirement when
  touching finalize, the manifest, or the temp directory layout.
- Prefer extending the manifest line format over adding a second file; anything
  not in `recovery.tsv` is invisible to recovery.
- Errors surfaced from here reach the user through the Diagnostics page and the
  status bar, so `lastError()` text should say what failed and what to try.

## Verification

- `ReplayBufferTests` (CTest) links `RecoveryStore.cpp` and `WavWriter.cpp` and
  covers the surrounding flows.
- `Mp4Muxer.cpp` has no unit test by design (it pulls in Media Foundation);
  verify it by recording and saving a replay, then confirm
  `recoverLatestFailedRecording` rebuilds a clip from a killed session.

## Child DOX Index

None. This folder's children are files only.