# replay

## Purpose

The rolling buffer a replay save cuts from. Video and sound each have two
interchangeable backends, a fixed-capacity ring file on disk by default and
process memory when the user opts out per track (`replay_video.bin` and
`replay_audio.bin` in the same folder).

## Ownership

- Owns: `ReplayBuffer`, its metadata model, the ring files, and the trimming and
  save logic. Owns the ring folder's layout, its startup cleanup
  (`removeOrphanedRing`), and the report of a disk-to-RAM degradation
  (`storageFallbackReason`, kept per track). Each ring file is pre-sized to
  the whole capacity (video up to 16 GiB, audio up to 4 GiB), which is why a
  killed session strands files that the clip library's recovery sweep never
  sees.
- Does not own: muxing (`mux/Mp4Muxer.h` is called from `saveTo`), capture, or
  the ring location and capacity: `RecorderController` resolves
  `AppSettings::replayRingDirectory` (falling back to `clipDirectory`) and
  derives the capacity from `replay.seconds` and `video.bitrateKbps`.

## Local Contracts

- `configure` uses the storage directory and video capacity only when
  `settings.diskBackedVideo` is set, the directory is non-empty, and the
  capacity is at least `kMinRingCapacityBytes` (8 MiB); the audio capacity
  likewise gates `settings.diskBackedAudio` at `kMinAudioRingCapacityBytes`
  (1 MiB). Anything else is a RAM configuration for that track, so a capacity
  below the floor is not a capacity change at all. `settings.enabled` off
  clears every buffer and closes both rings. Each track switches backends on
  its own: toggling sound to disk drops buffered audio only, never video.
- Only a backend or ring-directory switch may drop buffered video. A capacity
  change must not: the recorded extents stay valid at their offsets, and
  `settings.seconds` already reshapes the window on the next trim, so capacity
  is only a disk-usage ceiling. Re-laying out a ring that still holds packets
  would invalidate every live extent, so a capacity change is deferred while
  the ring holds packets or a save is pinned and takes effect on a later
  settings change once the buffer has drained. The
  caller derives capacity from `seconds`, so this is what makes editing the
  replay window length non-destructive. The audio ring follows the same rule
  for its own capacity; audio has no keyframe realignment, so its byte
  eviction is plain oldest-first across both tracks by front PTS.
- A save must never publish a partial video track. A ring read failure and a
  failed write to the temporary elementary stream are different faults and are
  reported separately: the stream stays healthy when a read fails, so checking
  the stream alone let a short sample table through and produced a clip whose
  video ended early against audio running to the full window. Failing the save
  is what keeps `lastSaveEndPts100ns_` from advancing past frames the user
  never received. The parameter-set probe counts as a ring read: a failed probe
  would otherwise look like "this frame carries no parameter sets" and prepend
  sets the dump loop is about to write anyway.
- Save-path logic must stay backend agnostic. `VideoPacketMeta` exists so the
  trimming, cut, and parameter-set logic reads the same for both backends; do
  not branch on the backend where metadata is enough.
- Cut points are the packets flagged `keyFrame`, which both encoders set from
  `BitstreamUtils::bitstreamContainsKeyFrame`; the buffer never scans
  bitstreams itself. `trimLocked` and `freeRingSpaceLocked` both realign to the
  oldest buffered keyframe, so a buffer that has lost its keyframes saves
  nothing rather than an undecodable clip.
- `lastSaveEndPts100ns_` makes consecutive saves overlap instead of starting
  cold. The next cut prefers the last keyframe at or before
  `lastSaveEndPts100ns_ - 3 s`, falls back to the first keyframe after that,
  and finally to the oldest buffered keyframe. It advances only when a save
  publishes, so a failed save re-cuts the same range instead of losing it.
- `codecHeaderBytes_` caches the parameter sets from the first keyframe that
  carried them and is prepended to a cut whose start packet lacks headers,
  because a driver can ignore header insertion.
- `extendLastVideoDuration` mutates the newest packet in place under the mutex
  instead of copying a full payload per emitted interval. `saveTo` snapshots
  metadata under the same mutex, so no reader sees it unsynchronized. Keep both
  halves of that invariant together.
- Ring protocol: a save takes a pin (`savePinsRefs_`), and while pinned, pushes
  that need space wait on `ringSpace_` instead of byte-trimming, because
  trimming could evict a packet an in-flight save has not read yet. The single
  pin covers both rings: it is taken when either backend is on disk and
  released once both the video and the audio bytes are consumed, before the
  audio is written and before muxing. Any new space policy must preserve the
  pin; the count is a counter rather than a bool so a second concurrent save
  cannot break the protocol silently. Disk-audio trimming and the
  empty-video audio clear both skip extent eviction while pinned, for the same
  reason the video path does.
- A ring read or write failure (typically a full disk, a vanished volume, or a
  media error) is not propagated as a save error. `failOverToRamLocked` discards
  the buffered video window and continues in memory for the rest of the session,
  and `failOverAudioToRamLocked` does the same for audio alone: a coherent clip
  matters more than the storage mode, and each backend degrades without
  touching the other. A read failure must take that path especially, because the
  bytes behind its extents are gone, so every later save would cut from the same
  offsets and fail identically. The user must still be told, so every call site
  passes a reason into the failover and a ring that cannot be opened at all
  publishes the same field; reasons are kept per track so one backend's report
  cannot hide or clear the other's. Keep the setting itself untouched: the
  checkbox stays ticked, so the report is the only evidence the backend changed.
- `clearBufferedVideoLocked` must not reset `ringWriteOffset_`, which keeps the
  write head at least a full buffer ahead of offset 0. It does not make `clear()`
  safe during a pin: zeroing `ringLiveBytes_` makes the whole ring look free, so
  a push could wrap onto pinned bytes. Clearing mid-pin is therefore only
  allowed when the ring is being abandoned, which is what makes
  `failOverToRamLocked` the one exception: it can run from the encode thread
  while `savePinsRefs_` is non-zero, and is safe because the same call flips the
  backend to RAM, so no push writes the ring again and the pinned extents keep
  their bytes until the deferred close. Every other caller runs on the
  controller worker, which serializes against `saveReplay`. Any new caller must
  preserve one of those two properties. Only `openRingLocked` and
  `closeRingNowLocked` reset the cursor.
- Ring I/O must never wait unboundedly. `transferRing` bounds every overlapped
  request with `kRingIoTimeoutMs` and, on timeout, cancels and then drains
  before returning: the `OVERLAPPED` and the caller's buffer both go out of
  scope at that return, so bailing out with the request still in flight would
  have the kernel write into freed memory. The drain's `INFINITE` wait is safe
  only because `CancelIoEx` guarantees the request completes. Both callers hold
  `mutex_`, and the ring folder is user-chosen, so an unbounded wait on a
  stalled USB stick or network drive would wedge the encode loop and
  `stopPipeline`'s thread joins with no way to close the app. A timeout is
  reported as an ordinary ring failure, so the session continues in RAM.
- `videoPacketCount` and `videoKeyFrameCount` read lock-free mirrors refreshed
  by `publishCountersLocked`, because `RecorderController::stats` calls them
  from the UI thread and must not queue behind the synchronous ring write that
  `pushVideo` performs under `mutex_`. Every mutation of the packet and
  keyframe counts has to republish, including byte-eviction in
  `freeRingSpaceLocked`, which does not go through `trimLocked`. The span and
  newest-PTS getters stay locked because only the controller worker calls them.
- Audio follows the same timeline rules as the recording path
  (`mux/AGENTS.md`): `writeTimelineAudio` drops a mismatched format, fills a PTS
  gap with at most 10 s of silence, and resyncs the pacing pointer to the
  incoming packet past that cap. The cap is load-bearing here, not cosmetic: a
  replay window runs up to an hour, so one unavailable loopback device would
  otherwise be written out as tens of minutes of silence, and the mux free-space
  preflight then demands hundreds of megabytes for a clip whose real audio is a
  few megabytes.
- A failed mux keeps its temporary directory only when the recovery manifest was
  published, and reports it through `RecoveryStore::preservedRecordingDetail`.
  The reverse case deletes the directory: `saveTo` has already verified every
  elementary-stream write by that point, so a manifest that will not publish is
  a real write failure, not a scanner race (`writeManifest` retries those
  itself). A directory holding streams but no manifest is skipped by
  `recoverLatestFailedRecording` and swept as garbage later, so keeping it would
  only promise a recovery that cannot happen.
- `newestVideoPts100ns` exists because PTS-age trimming cannot detect a stalled
  capture or encode pipeline; heartbeats keep advancing timestamps while the
  pipeline is idle. Callers use it to detect that case.

## Work Guidance

- The class is not copyable and owns a `HANDLE`. Keep it a member of
  `RecorderController`, never a value passed around.
- Payloads may wrap the end of the ring and are recorded as up to two extents.
  Reassembly bugs show up as clips that corrupt after roughly one buffer's worth
  of recording; test across a wrap when touching this code.
- Bytes are copied only on the save path. A push is metadata plus a ring append
  or a shared pointer move, but the ring append is a synchronous overlapped
  write that completes under `mutex_`; that is the encode thread's per-frame
  stall budget (bounded by `kRingIoTimeoutMs`) and the reason the UI counters
  are mirrored.
- Lock order is `mutex_` then `storageFallbackMutex_`, never the reverse.

## Verification

- `ReplayBufferTests` (CTest) covers the real buffer with `Mp4Muxer` entry
  points stubbed inside the test source. It covers trimming, wrap-around
  reassembly, byte-identical parity between the two backends,
  parameter-set prepending on both backends, decode-timestamp round-trip,
  consecutive-save overlap, audio on the shared timeline (RAM and disk), the
  disk-audio ring (round trip, wrap reassembly, capacity-only reconfigure,
  audio-only read-failure failover), the
  mux-failure recovery path, a capacity-only reconfigure (buffer kept, retained
  extents read back byte for byte, deferred resize applied once the buffer
  drains), backend and directory switches, a failed ring open, an oversized
  packet, an induced ring read failure, and the orphan-folder sweep.
- `breakRingForTesting` drops the video ring handle (and `breakAudioRingForTesting`
  the audio one) to induce the read failure, and
  `ReplayBufferTestAccess` in the test source is their only caller. Keep it that
  way: they exist because the save path's read-failure handling is silent data
  loss if it regresses, and no public path can provoke the fault.
- A ring write failure from a full disk is still a manual check. Injecting it
  needs the volume actually full, so put the ring folder on a small removable
  drive, fill it, and confirm the save keeps working in memory. The stall path
  is a manual check for the same reason: `kRingIoTimeoutMs` is long enough that
  no test can provoke it cheaply, so exercise it by pulling the drive mid-write
  and confirming the app still closes and degrades to memory.

## Child DOX Index

None. This folder's children are files only.