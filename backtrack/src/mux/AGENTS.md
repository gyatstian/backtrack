# mux

## Purpose

Turning a recording into a playable file: the MP4 container write through
Media Foundation's MPEG-4 sink writer, the WAV writer behind each audio track,
the read-back of the mixed track the mux writes into the MP4, and the crash
recovery store that keeps a killed recording recoverable.

## Ownership

- Owns: `Mp4Muxer`, `RecoveryStore`, `WavWriter`, `WavReader` (the mux-side
  read-back and its large-file extent recovery), `removeEmptyOutputReservation`.
- Does not own: the audio mixing decision and the mux PCM target format
  (`audio/AudioMix.h`), the WAV parser itself (`audio/WavReader.h`), normal
  output-name reservation (`clips/ClipStorage.h`, used by `RecorderController`),
  or the replay buffer that feeds it.

## Local Contracts

- `startRecording` opens a video stream and incremental manifest; WAV tracks
  open lazily on their first audio packet. `finalize` publishes an MP4 or
  returns `{}` with `lastError()` set. write and mux failures retain the
  session, including a usable incremental prefix when a rewrite fails; without
  verifiable timing, the error points to the streams for manual recovery.
  `abort` explicitly discards only an active recording. a recording with no
  encoded video may be discarded.
- the recording stop cut is a decode-order prefix, owned here:
  `setCutPts100ns` writes packets at or below the cutoff and latches a bounded
  hold at the first packet above it (later pre-cutoff B pictures included, since
  they follow their reference in decode order), and `finalize` writes the held
  prefix up to the last held packet at or below the cutoff. Dropping the
  post-cutoff reference while keeping those B pictures produces a tail no
  decoder can decode; a hold overflow drops the tail and keeps the written
  prefix, which is still decodable. Audio is not held: a track slightly longer
  than the video is valid.
- Audio that arrives before the first video packet is held in `pendingAudio_`
  (at most 512 packets) and flushed when that packet anchors the timeline, so
  short recordings and pre-keyframe audio are not dropped.
- `RecoveryStore::createUniqueDirectory` exclusively creates each recording or
  replay session under `<output dir>/.backtrack_tmp/` and each MP4 staging
  attempt inside its session. never reuse an existing directory. the staged
  MP4 is flushed and moved with `MOVEFILE_WRITE_THROUGH`; the final move is
  fail-if-exists except for an empty hidden name reservation. mux retries use a
  bounded backoff and rotate the preferred staging variant for sharing or lock
  violations.
- Audio and video share one 100 ns timeline. `MFSampleExtension_DecodeTimestamp`
  is set only when some sample has a DTS differing from its PTS, and both tracks
  are measured from the same `timeBase` (the first sample's DTS, or its PTS when
  there is none, `muxTimeBase100ns` in `MuxedInputs.h`), so PTS-DTS lands exactly
  as the ctts composition offset. Audio sample times start at
  `videoCompositionDelay100ns` - the first video sample's PTS minus its DTS, 0
  without reordering - which is the first video sample's own sample time, so
  B-frame reordering holds audio back by the same amount it holds back the
  picture. Do not cap that delay independently of the video: a shorter audio
  offset than the video's decode lead desynchronises the tracks.
- Every sample time the mux writes is `value - timeBase`, and recovery reads its
  timestamps from a file on disk. `int64DifferenceFits` bounds that subtraction:
  `readRecoveryManifest` closes the table before the first sample whose span is
  unrepresentable, and `writeVideoSamples` refuses one instead of letting the
  subtraction wrap into a plausible-looking sample time.
- System and microphone audio are mixed into one PCM track by
  `buildMixedPcmAudio` and encoded as a single AAC stream. If that stream cannot
  be configured the save falls back to video-only rather than failing; if the
  mix cannot be written (disk full) or an expected source WAV cannot be read
  back it does fail, because the alternative is a silently truncated or
  audio-less track.
- Incremental crash recovery is a contract, not an optimization:
  `appendManifestSampleLocked` appends one `sample` line to `recovery.tsv` per
  written packet and flushes at GOP boundaries, so the manifest is at most the
  current partial GOP stale. The elementary stream's own buffer can lag further
  than that, which is what the end-of-stream trim in recovery exists for. An
  audio track registers itself in the manifest as soon as its WAV opens. Keep
  the GOP-boundary flush.
- manifest streams live beside `recovery.tsv` and are resolved relative to it.
  `writeManifest` atomically publishes through `recovery.tsv.tmp`, retrying
  sharing, lock, and access-denied failures. a failed rewrite must not replace
  or delete an incremental manifest. `preserveFailedRecording` validates its
  prefix and only creates a manifest if none exists; it retains streams even
  when timing cannot be verified.
- A 7-column manifest carries no DTS-validity flag, so `hasDts` is inferred as
  `dts100ns != pts100ns` (their writers stored the presentation time whenever
  the decode time was invalid). Column 8 carries the explicit flag and wins.
- `RecoveryStore::discover` treats a declared sample count as a claim, never as
  capacity: the sample vector grows with the lines actually parsed. A manifest
  claiming ten million samples must not reserve half a gigabyte before one media
  byte is validated.
- Recovery tolerance is asymmetric on purpose. A wrong magic line or a damaged
  metadata value rejects the whole manifest; a torn or non-contiguous sample
  tail keeps the valid prefix, and samples reaching past the end of the
  elementary stream are dropped. Every number must parse as a complete token, so
  a line torn mid-digits cannot become a plausible-but-wrong offset. numeric
  parsing rejects overflow and unsigned signs; signed timestamps may be negative.
- `recoverLatestFailedRecording` selects the newest valid manifest across the
  `ClipStorage` roots and publishes to its original output root with an
  exclusively claimed name. `hasRecoverableRecording` uses the same discovery,
  stopping at the first candidate without staging or muxing. candidates need
  a video sample with bytes on disk and skip already recovered sessions.
  startup attempts every recoverable session once;
  `RecorderController` serializes the sweep against saves and manual recovery.
  the sweep has no age limit: sessions are never pruned by age, so a failed
  recovery keeps its streams and `ensureFreeDiskSpace` is what protects later
  saves.
- recovery muxes from an exclusively created work directory, hard-linking or
  copying inputs so mixed audio cannot overwrite legacy content. original
  sessions and legacy ambiguous hidden reservations remain untouched. a completed
  session gets an exclusive marker when possible; it records the published
  clip's identity, so the sweep skips the session while that identity still
  matches it and also while the recorded output path holds no file at all (a
  delete or rename through the library means the user already acted on the
  result), and a different file at that path - a restored or recreated clip -
  makes it recoverable again. a missing or invalid marker
  can cause a later duplicate recovery, never deletion of its streams.
- Video PTS regression with no decode timestamps is warned about once, not
  corrected: the MP4 timeline cannot represent it, and the sample table is the
  honest record. A DTS that lands before `timeBase` is warned about and clamped
  to the base instead of wrapping into `UINT64`; decode order makes that
  unreachable for real input, so the clamp never changes a valid file.
- Audio timeline rules, applied once in `paceAudioPacket`
  (`mux/AudioTimeline.h`) and called from both `Mp4Muxer::writeTimelineAudioPacket`
  (open-ended: `kNoAudioWindowEnd`) and `ReplayBuffer`'s `writeTimelineAudio`
  (window-bounded by the replay `[start, end]`):
  - A packet whose format differs from the WAV the writer opened is dropped, not
    appended. Appending wrong-rate bytes corrupts the timeline silently.
  - A PTS gap is filled with the full gap's silence so the saved audio keeps
    the real gap duration; the timeline is never compressed, so post-outage
    audio stays aligned with the unchanged video track.
  - A temporary-WAV write or finalization error fails the save: `finalize`
    reports it for a recording, `saveTo` returns it for a replay, rather than
    publishing truncated audio.
  - `paceAudioPacket` accumulates gap-fill statistics into its `AudioGapStats&`
    argument (fill events, silence bytes, max gap ms). Both callers own one
    (`AudioTimelineState::gapStats` on the recording side, a local in
    `ReplayBuffer::writeTimelineAudio`) and log one info line at
    finalize/save when any gaps were filled; the per-packet path itself never
    logs.
- `WavWriter::open` resets the previous stream and accepts its own `path()` as
  the next path; preserve that path before clearing writer state.
- `WavWriter` is the single WAV implementation for the recording tracks, the
  replay tracks, and the mixed PCM copy in `audio/AudioMix.cpp`. It patches the
  RIFF and data sizes in `patchHeader` from `bool close()`. `close()` reports a
  header-patch or final-flush failure, and that fails the save for a recording
  and for a replay instead of publishing shortened audio and deleting the
  session. A process killed before that leaves zeroed
  fields - `parseWav` recovers from that by falling back to the real file size.
  Writes past the 4 GiB RIFF boundary keep going (large-file internal audio) and
  the header clamps both sizes to the uint32 maximum on `close`, so the data
  chunk declares "runs to EOF". `parseWav` trusts the declared extent while it
  is plausible and therefore caps such a file at the boundary; every reader that
  must see the whole payload extends a data size field of uint32 maximum to the
  real file size - `audio/AudioMix.cpp` for the mix sources and `parseMuxWav`
  (`mux/WavReader.h`) for the mixed track the mux writes into the MP4. A smaller
  declared data size means real trailing chunks and is never extended. The mux
  reads the mixed track through `parseMuxWav`, and a mixed track that cannot be
  read back fails the save instead of publishing a truncated or audio-less
  track.
- `removeEmptyOutputReservation` deletes only a hidden, zero-byte regular file
  at a claimed output path. those attributes cannot prove identity if another
  process replaces the reservation.
- `MuxedInputs` (`mux/MuxedInputs.h`) is the shared hand-off shape between the muxer, the recovery
  store, and `audio/AudioMix.h`. Extending it means updating all three.
  `effectiveDts` / `hasEffectiveDts` (`mux/MuxedInputs.h`) is the one DTS-validity
  rule both the recording path and `ReplayBuffer::saveTo` use
  (`hasDts && dts <= pts`, else PTS), called as `(pts, dts, flag)`. The same
  header owns the canonical duration clamp/coalesce
  (`coalesceVideoDuration100ns`, 60 s ceiling) both writers use. The `dts != pts` test stays where
  detection, not validity, is meant: the sink's `hasDecodeTimestamps` flag and
  the legacy-manifest `hasDts` inference in `RecoveryStore`.
- `RecoveryStore::flushFileToDisk` flushes the staged MP4 and manifest; its
  `DWORD&` reports the Win32 error. a failed manifest rewrite does not imply
  that an earlier manifest is unusable.
- `RecoveryStore::ensureFreeDiskSpace` is the one free-space preflight for both
  save paths (`Mp4Muxer::muxToMp4` with real file sizes, `ReplayBuffer::saveTo`
  with snapshot byte totals before writing streams).

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

- `StorageLayoutTests` (CTest) covers recovery availability, nested recovery,
  and collision preservation: recovery muxes an owned work copy, so the
  original sessions and their streams are retained. a completed recovery is
  marked so the sweep does not repeat it, even after the recovered clip is
  deleted or renamed.
- `ReplayBufferTests` (CTest) links the real `RecoveryStore.cpp` and
  `WavWriter.cpp` and stubs `Mp4Muxer::muxToMp4` at link time. It covers the
  manifest round trip and its atomic publish, the end-of-stream tail trim, a
  torn final line, a non-contiguous tail, a declared-count shortfall, rejection
  of damaged metadata, a WAV whose header was never patched, and that a PTS gap
  keeps its full duration in the saved replay audio.
- `AudioPersistenceTests` (CTest) covers `paceAudioPacket`'s full-gap fill (a
  long outage keeps its duration on the saved timeline), frame-aligned silence
  across chunk boundaries, reopening the writer's own path, and writes past the
  4 GiB RIFF boundary.
- `Mp4MuxerTests` (CTest, the only Media Foundation-linked suite) covers
  recording write-failure retention, inactive abort, same-name retry,
  strict recovery numeric tokens and signed timestamp round trips, failed
  manifest publication retaining the old file and removing staging, and the
  mux WAV extent (`parseMuxWav`): a clamped data size keeps the payload past the
  RIFF boundary with the frame count and duration covering it, while a
  well-formed extent and a real trailing chunk are left alone. It also covers
  the MP4 write itself: a real recording reopened for video subtype, size,
  audio track and duration; the `stts`/`ctts` timeline checked against the
  written packets including a decode-ordered B-frame stream; the recording cut
  keeping the future reference and the pre-cutoff B pictures while dropping the
  trailing post-cutoff packets; decoding of an
  OS-encoded H.264 stream when those MFTs exist (a printed skip otherwise);
  and a recording WAV-finalization failure failing the save while keeping
  `system.wav` and the manifest recoverable. Real NVENC/AMF streams, HEVC,
  hardware decode, and app-level publication and recovery of a killed session
  still need a hand check.

## Child DOX Index

None. This folder's children are files only.