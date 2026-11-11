# replay

## Purpose

The rolling buffer a replay save cuts from. Video and sound each have two
interchangeable backends, a fixed-capacity ring file on disk by default and
process memory when the user opts out per track (normally `replay_video.bin`
and `replay_audio.bin` in the same folder, with alternate names on collision).

## Ownership

- Owns: `ReplayBuffer`, `RingFile` (the byte store both ring files share),
  its metadata model, the ring files, and the trimming and
  save logic. owns ring-file creation and the report of a disk-to-RAM
  degradation (`storageFallbackReason`, kept per track). each ring file is
  pre-sized to the whole capacity (video up to 16 GiB, audio up to 4 GiB).
- Does not own: muxing (`mux/Mp4Muxer.h` is called from `saveTo`), capture, or
  the ring location and capacity: `RecorderController` resolves
  `AppSettings::replayRingDirectory` (falling back to `clipDirectory`) and
  derives the capacity from `replay.seconds` and `video.bitrateKbps`.

## Local Contracts

- ring files use exclusive creation, alternate names on collision, and
  delete-on-close handles. legacy ring files and folders lack reliable ownership
  evidence and are retained during startup maintenance.
- `configure` uses the storage directory and video capacity only when
  `settings.diskBackedVideo` is set, the directory is non-empty, and the
  capacity is at least `kMinRingCapacityBytes` (8 MiB); the audio capacity
  likewise gates `settings.diskBackedAudio` at `kMinAudioRingCapacityBytes`
  (1 MiB). Anything else is a RAM configuration for that track, so a capacity
  below the floor is not a capacity change at all. Either capacity is still
  stored as that track's RAM byte cap (zero falls back to the controller's
  derivation), so a long window can never grow memory without bound and the
  cap survives a disk-to-RAM failover. `settings.enabled` off
  clears every buffer and closes both rings. Each track switches backends on
  its own: toggling sound to disk drops buffered audio only, never video.
- RAM trimming evicts to both bounds: the time window and the stored byte cap.
  Video realigns to the oldest buffered keyframe on either bound (same as
  `freeRingSpaceLocked`); audio evicts oldest-first across both tracks by front
  PTS. RAM snapshots hold shared pointers, so byte eviction needs no save-pin
  wait.
- the video and audio rings share `RingFile`. its free-byte accounting requires
  every release to remove a physical prefix. video realigns to the next keyframe;
  disk audio keeps one insertion-ordered queue across tracks. age trimming stops
  at the first unexpired packet, even when later packets have expired. saves
  split the read audio snapshot by track after reading. the save pin, the
  per-ring consumed cursor, deferred close, and failover reasons stay in
  `ReplayBuffer`.
- apart from normal time and byte trimming, only a backend or ring-directory
  switch drops buffered video. reconfiguring RAM video preserves compatible
  packets and immediately applies both bounds. on disk capacity is only a
  usage ceiling: the recorded extents stay valid at their offsets and
  `settings.seconds` reshapes the window on trim. re-laying out a ring that
  still holds packets would invalidate its extents, so a capacity change is
  deferred while the ring holds packets or a save is pinned. it takes effect
  on a later settings change once the buffer has drained. the audio ring
  follows the same rule for its own capacity.
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
  not branch on the backend where metadata is enough. `saveTo` snapshots once
  into a RAM-or-pinned-extents view and the cut, header probe/prepend, sample
  table, and audio reassembly all read through that one reader; `videoSpan100ns`
  collects the metadata once and runs the same window cut. Duration coalescing
  is canonical in `mux/MuxedInputs.h` (`coalesceVideoDuration100ns`, 60 s
  ceiling): both `extendLastVideoDuration` bodies and `pushVideo` use it, same
  as the recording path.
- Cut points are the packets flagged `keyFrame`, which both encoders set from
  `BitstreamUtils::bitstreamContainsKeyFrame`; the buffer never scans
  bitstreams itself. `trimLocked` and `freeRingSpaceLocked` both realign to the
  oldest buffered keyframe, so a buffer that has lost its keyframes saves
  nothing rather than an undecodable clip.
- `lastSaveEndPts100ns_` makes consecutive saves overlap instead of starting
  cold. The next cut prefers the last keyframe at or before
  `lastSaveEndPts100ns_ - 3 s`, falls back to the first keyframe after that,
  and finally to the oldest buffered keyframe. it updates only when a save
  publishes, so a failed save re-cuts the same range instead of losing it.
- video packets are in decode order. the replay end, audio end bound and
  consecutive-save accounting use the maximum retained presentation end,
  recalculated after cuts. `videoSpan100ns` uses the same window cut before
  consecutive-save overlap. when no later keyframe permits a cut, frames
  starting past the window budget remain and the reported span can exceed it.
  PTS-age trimming and the stall probe still use the last decode-order PTS.
- `codecHeaderBytes_` caches the parameter sets from the first keyframe that
  carried them and is prepended to a cut whose start packet lacks headers,
  because a driver can ignore header insertion. Sample 0's table size counts
  only headers actually written, so the offsets behind it cannot shift.
- `saveTo` preflights free space (`RecoveryStore::ensureFreeDiskSpace`) from
  snapshot byte totals before writing streams; the mux re-checks with real
  file sizes. `MuxPublishResult` is the unified RAM/disk save outcome.
- `extendLastVideoDuration` mutates the newest packet in place under the mutex
  instead of copying a full payload per emitted interval. `saveTo` snapshots
  metadata under the same mutex, so no reader sees it unsynchronized. Keep both
  halves of that invariant together.
- Ring protocol: a save takes a pin (`savePinsRefs_`) and reads through an
  incremental cursor - each packet it consumes is published via
  `RingFile::markSaveConsumed`, and ring eviction may reclaim only that
  consumed prefix (`releaseConsumedPrefix`), so a save never loses unread bytes
  and a push blocks on `ringSpace_` only when the unconsumed prefix alone cannot
  free enough space (a save reads far faster than encode rate, so in practice
  never). The single pin covers both rings: it is taken when either backend is
  on disk and released once both the video and the audio bytes are consumed,
  before the audio is written and before muxing. The count is a counter rather
  than a bool so a second concurrent save cannot break the protocol silently;
  concurrent saves only over-count the cursor, and every mark still corresponds
  to a real read, so the front is never under-protected. Disk-audio trimming
  and the empty-video audio clear both skip extent eviction while pinned, for
  the same reason the video path does.
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
- `clear()` and disabled `configure` share `clearBuffersLocked`, which requires
  an already-held `mutex_` and serialization against saves. clearing keeps the
  rings open; disabling closes them after clearing.
- `clearBufferedVideoLocked` must not reset the `RingFile` write cursor, which
  keeps the write head at least a full buffer ahead of offset 0. It does not make
  `clear()` safe during a pin: clearing live-byte accounting makes the whole ring look free, so
  a push could wrap onto pinned bytes. Clearing mid-pin is therefore only
  allowed when the ring is being abandoned, which is what makes
  `failOverToRamLocked` the one exception: it can run from the encode thread
  while `savePinsRefs_` is non-zero, and is safe because the same call flips the
  backend to RAM, so no push writes the ring again and the pinned extents keep
  their bytes until the deferred close. Every other caller runs on the
  controller worker, which serializes against `saveReplay`. Any new caller must
  preserve one of those two properties. only `RingFile::open` and
  `RingFile::closeNow` reset the cursor.
- pending ring transfers wait at most `kRingIoTimeoutMs` per request. on timeout
  or wait failure they request cancellation and return a ring fault; cancellation
  does not guarantee completion. a thread-pool wait retains the operation's
  duplicate file handle, event, `OVERLAPPED` and private buffer until final
  completion, including after ring close. a failed write prevents further writes
  to that ring; a reopened ring is independently created. a pending deletion
  may prevent reopening its old name. a request that never completes retains
  those resources until process exit. ring I/O has no stop signal from the
  controller, and synchronous Win32 submission, cancellation, opening or
  pre-sizing are not covered by the completion-wait deadline.
- `videoPacketCount` and `videoKeyFrameCount` read lock-free mirrors refreshed
  by `publishCountersLocked`, because `RecorderController::stats` calls them
  from the UI thread and must not queue behind the synchronous ring write that
  `pushVideo` performs under `mutex_`. Every mutation of the packet and
  keyframe counts has to republish, including byte-eviction in
  `freeRingSpaceLocked`, which does not go through `trimLocked`. The span and
  newest-PTS getters stay locked because only the controller worker calls them.
- Audio follows the same timeline rules as the recording path
  (`mux/AGENTS.md`): `writeTimelineAudio` opens the WAV and drops
  format-mismatched packets, then paces every packet through the shared
  `paceAudioPacket` core (`mux/AudioTimeline.h`) with the replay window
  `[start, end]` as its bound, so a packet past the window end is not written
  and the clip's audio stops with the video.
- each `saveTo` owns a freshly created session directory. a failed stream or
  mux write retains its files; `RecoveryStore` creates a manifest for a usable
  video prefix where possible and reports when manual recovery may be needed.
  a same-name retry never truncates an earlier session.
- `newestVideoPts100ns` exists because PTS-age trimming cannot detect a stalled
  capture or encode pipeline; heartbeats keep advancing timestamps while the
  pipeline is idle. Callers use it to detect that case.
- `trimLocked` accumulates evicted packet counts into `trimVideoEvicted_` /
  `trimAudioEvicted_` (RAM and disk, video and audio) and logs one aggregate
  info when either crosses the report threshold, instead of a line per pushed
  packet. `saveTo` logs one save-start line naming the backends and the buffered
  packet/byte counts.

## Work Guidance

- The class is not copyable and owns a `HANDLE`. Keep it a member of
  `RecorderController`, never a value passed around.
- Payloads may wrap the end of the ring and are recorded as up to two extents.
  Reassembly bugs show up as clips that corrupt after roughly one buffer's worth
  of recording; test across a wrap when touching this code.
- a disk push copies each I/O chunk into operation-owned storage before waiting
  under `mutex_`; a completed disk read copies back to the save buffer. the
  completion-wait budget is `kRingIoTimeoutMs` per request, and UI counters are
  mirrored to avoid waiting behind a disk push. completed transfers recycle
  their operation (duplicated handle, event, owned buffer) from a per-ring
  pool; only the timeout/cancellation path detaches an operation for the drain
  callback.
- Lock order is `mutex_` then `storageFallbackMutex_`, never the reverse.

## Verification

- `StorageLayoutTests` (CTest) covers game roots, tags, nested recovery, and
  ring collisions, clear/disable ring lifetime and resumed saves, with recovery
  preserving the original session content.
- `ReplayBufferTests` (CTest) covers the real buffer with `Mp4Muxer` entry
  points stubbed inside the test source. It covers trimming, wrap-around
  reassembly, byte-identical parity between the two backends,
  parameter-set prepending on both backends, decode-timestamp round-trip,
  RAM reconfiguration with payload-exact retained video, reordered presentation
  ends across both backends and cuts, consecutive-save overlap, audio on the
  shared timeline (RAM and disk), the
  disk-audio ring (byte-exact wrap reassembly with reversed track timestamps,
  skew, age trimming and saves between track drains, capacity-only reconfigure,
  audio-only read-failure failover), the
  mux-failure recovery path, same-name retry, aged startup sessions and failed
  manifest rewrites, a capacity-only reconfigure (buffer kept, retained
  extents read back byte for byte, deferred resize applied once the buffer
  drains), backend and directory switches, a failed ring open, an oversized
  packet, an induced ring read failure, a RAM byte cap (large window plus many
  pushes stays under the passed caps and retains newest, including after a
  read-failure failover), and legacy ring-folder preservation. `RingFileTests`
  covers the ring file itself.
- `breakRingForTesting` drops the video ring handle (and `breakAudioRingForTesting`
  the audio one) to induce the read failure, and
  `ReplayBufferTestAccess` in the test source is their only caller. Keep it that
  way: they exist because the save path's read-failure handling is silent data
  loss if it regresses, and no public path can provoke the fault.
- `RingFileTests` (CTest) exercises wrap and partial reads, immediate and
  completed errors, zero/oversized completion counts in both directions,
  a real pending pipe read, and injected delayed completion
  after close. a full disk or stalled removable drive remains a manual
  end-to-end check of disk-to-RAM failover.

## Child DOX Index

None. This folder's children are files only.