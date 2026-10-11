# mux

## Purpose

Turning a recording into a playable file: the MP4 container write through
Media Foundation's MPEG-4 sink writer, the WAV writer behind each audio track,
and the crash recovery store that keeps a killed recording recoverable.

## Ownership

- Owns: `Mp4Muxer`, `RecoveryStore`, `WavWriter`,
  `removeEmptyOutputReservation`.
- Does not own: the audio mixing decision and the mux PCM target format
  (`audio/AudioMix.h`), the output name reservation policy (that lives in
  `RecorderController::nextClipPath`), or the replay buffer that feeds it.

## Local Contracts

- Recording is two stages. `startRecording` opens the elementary video stream
  and starts the incremental recovery manifest; each track's WAV opens lazily on
  its first audio packet, because the writer's format comes from that packet.
  `finalize` muxes into the MP4 and returns the published path, or `{}` with
  `lastError()` set. `abort` gives up without publishing. A mux failure keeps
  the temp directory and its manifest for recovery; a write failure or a
  recording with no video deletes it.
- Audio that arrives before the first video packet is held in `pendingAudio_`
  (at most 512 packets) and flushed when that packet anchors the timeline, so
  short recordings and pre-keyframe audio are not dropped.
- Publishing is staged. The MP4 is written to
  `<output dir>/.backtrack_tmp/<stem>.publish[.n].mp4`, flushed to disk, then
  moved into place with `MOVEFILE_WRITE_THROUGH`, so a crash can never leave a
  partial MP4 at the clip path. The publish is fail-if-exists: anything already
  at the target that is not our own empty hidden reservation is never
  overwritten. `stagingVariant` is zero-based and `muxToMp4WithRetry` owns the
  rotation - it retries any failure up to four attempts with growing backoff,
  rotating the variant only on `ERROR_SHARING_VIOLATION` /
  `ERROR_LOCK_VIOLATION` (antivirus, indexer, Explorer preview).
- Audio and video share one 100 ns timeline. `MFSampleExtension_DecodeTimestamp`
  is set only when some sample has a DTS differing from its PTS, and both tracks
  are measured from the same `timeBase` (the first sample's DTS, or its PTS when
  there is none), so PTS-DTS lands exactly as the ctts composition offset.
  Audio sample times start at `videoCompositionDelay100ns` - the first video
  sample's PTS minus its DTS, capped at 2 s as a plausibility guard, 0 without
  reordering - which is the first video sample's own sample time, so B-frame
  reordering holds audio back by the same amount it holds back the picture.
- System and microphone audio are mixed into one PCM track by
  `buildMixedPcmAudio` and encoded as a single AAC stream. If that stream cannot
  be configured the save falls back to video-only rather than failing; if the
  mix cannot be written (disk full) it does fail, because the source WAVs were
  readable and the alternative is a silently truncated audio track.
- Incremental crash recovery is a contract, not an optimization:
  `appendManifestSampleLocked` appends one `sample` line to `recovery.tsv` per
  written packet and flushes at GOP boundaries, so the manifest is at most the
  current partial GOP stale. The elementary stream's own buffer can lag further
  than that, which is what the end-of-stream trim in recovery exists for. An
  audio track registers itself in the manifest as soon as its WAV opens. Keep
  the GOP-boundary flush.
- The manifest names elementary streams by bare file name and is resolved
  relative to its own directory, so every stream must stay beside
  `recovery.tsv` in the same `.backtrack_tmp/<stem>/` folder. `writeManifest`
  publishes atomically through `recovery.tsv.tmp`; anything that truncates those
  streams underneath it (the replay save path does) must delete both files
  first and republish a fresh manifest before muxing, or a stale table would
  describe bytes that no longer exist. The publish retries a sharing or lock
  violation on the same bounded budget as the staged MP4 (`publishWithRetry`),
  because the manifest is already written and flushed when it runs and a
  scanner holding `recovery.tsv` would otherwise cost a complete recording. A
  `false` return therefore means a real write failure (full disk, denied or
  vanished folder), not a lost race - callers may treat the directory as
  unrecoverable and delete it, and must not report it to the user as preserved.
- A 7-column manifest carries no DTS-validity flag, so `hasDts` is inferred as
  `dts100ns != pts100ns` (their writers stored the presentation time whenever
  the decode time was invalid). Column 8 carries the explicit flag and wins.
- Recovery tolerance is asymmetric on purpose. A wrong magic line or a damaged
  metadata value rejects the whole manifest; a torn or non-contiguous sample
  tail keeps the valid prefix, and samples reaching past the end of the
  elementary stream are dropped. Every number must parse as a complete token, so
  a line torn mid-digits cannot become a plausible-but-wrong offset.
- `recoverLatestFailedRecording` rebuilds exactly one clip: the newest valid
  manifest under `<clip dir>/.backtrack_tmp/`, published to a claimed output
  name (`<stem>.mp4`, then `<stem>_recovered.mp4`, then
  `<stem>_recovered_<n>.mp4`). On success it deletes its directory; on failure it
  keeps it for the manual "Recover Failed" action.
- `RecoveryStore::sweepOrphanedRecoveryData` is startup maintenance: it first
  gives the newest valid manifest a chance to become a clip, then deletes
  entries under `.backtrack_tmp` (leftover session directories and stale
  staging files) plus hidden zero-byte regular files in the clip directory,
  all older than `maxAge` (24 h by default). It runs on a worker thread at
  startup and must not race with new recordings; `RecorderController` serializes
  it.
- Video PTS regression with no decode timestamps is warned about once, not
  corrected: the MP4 timeline cannot represent it, and the sample table is the
  honest record. A DTS that lands before `timeBase` is warned about and clamped
  to the base instead of wrapping into `UINT64`; decode order makes that
  unreachable for real input, so the clamp never changes a valid file.
- Audio timeline rules, applied by `Mp4Muxer::writeTimelineAudioPacket` and
  again by `ReplayBuffer`'s `writeTimelineAudio`:
  - A packet whose format differs from the WAV the writer opened is dropped, not
    appended. Appending wrong-rate bytes corrupts the timeline silently.
  - A PTS gap is filled with silence, capped at 10 s per gap; past the cap the
    pacing pointer resyncs to the incoming packet and the timeline is compressed
    rather than inflated with hours of silence. Keep both sites on this rule;
    they drifted once and the replay path had no cap at all.
  - A temporary-WAV write error fails the save: `finalize` reports it for a
    recording, `saveTo` returns it for a replay, rather than publishing
    truncated audio.
- `WavWriter` is the single WAV implementation for the recording tracks, the
  replay tracks, and the mixed PCM copy in `audio/AudioMix.cpp`. It patches the
  RIFF and data sizes on `close`, so a process killed before that leaves zeroed
  fields - `parseWav` recovers from that by falling back to the real file size.
  Passing the 4 GiB RIFF limit truncates audio on purpose and is not a write
  error, so long recordings still save.
- `removeEmptyOutputReservation` deletes only a hidden, zero-byte regular file,
  which is how a claimed clip name is distinguished from a user's own empty
  `.mp4`. Real content is never removed. Any change here risks deleting user
  data.
- `MuxedInputs` is the shared hand-off shape between the muxer, the recovery
  store, and `audio/AudioMix.h`. Extending it means updating all three.
- `RecoveryStore::flushFileToDisk` is the one disk-flush helper, declared in
  `RecoveryStore.h` and used by both publishes (the staged MP4 and the manifest).
  It takes a `DWORD&` out-param so a sharing violation stays distinguishable from
  a full disk or a denied write; the manifest publish logs that error, because
  `writeManifest`'s `false` return tells its caller the directory is
  unrecoverable and the Win32 reason is the only evidence of why.

## Work Guidance

- The staging and recovery paths exist because the app can be killed mid-save at
  any moment. Treat "the data is still recoverable" as the requirement when
  touching finalize, the manifest, or the temp directory layout.
- Prefer a trailing column on the existing manifest line over a second file:
  the reader ignores unknown keys and shorter lines, and anything not in
  `recovery.tsv` is invisible to recovery.
- Errors surfaced from here reach the user through the Diagnostics page and the
  status bar, so `lastError()` text should say what failed and what to try.

## Verification

- `ReplayBufferTests` (CTest) links the real `RecoveryStore.cpp` and
  `WavWriter.cpp` and stubs `Mp4Muxer::muxToMp4` at link time. It covers the
  manifest round trip and its atomic publish, the end-of-stream tail trim, a
  torn final line, a non-contiguous tail, a declared-count shortfall, rejection
  of damaged metadata, and a WAV whose header was never patched.
- `Mp4Muxer.cpp` has no unit test by design (it pulls in Media Foundation);
  verify it by recording and saving a replay, then confirm
  `recoverLatestFailedRecording` rebuilds a clip from a killed session.

## Child DOX Index

None. This folder's children are files only.