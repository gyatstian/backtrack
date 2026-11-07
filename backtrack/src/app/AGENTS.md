# app

## Purpose

Application lifetime and the recording pipeline. `Application` is the startup
sequence, `RecorderController` owns the four worker threads it starts and the
objects they share, `VideoTimelineScheduler.h` owns frame cadence,
`CaptureFrameHandoff.h` orders release of capture frames, `AudioDrain.h`
provides the audio-writer drain loop, and `RecordingCutoffTracker.h` tracks the
recording-stop drain handshake.

## Ownership

- Owns: startup ordering, the capture / encode / audio / startup-sweep threads,
  pipeline (re)creation, recording state, and frame-timing decisions.
- Does not own: the implementation of capture, encoding, audio, or muxing. It
  calls their interfaces and never reaches around them.

## Local Contracts

- `main.cpp` installs the SEH filter, `std::terminate` handler, and console
  control handler. Each one calls
  `WasapiCapture::emergencyRestoreSessionMutes()` before returning or aborting,
  because session mutes are global state that outlives a crash. Any new fatal
  exit path needs the same call.
- Startup order in `Application::run` is a contract, not a preference:
  1. single-instance mutex (`Local\Backtrack.SingleInstance`), activate an
     existing window and exit if one is running, then load settings. normalize
     only a complete read; missing or failed reads are not saved back. skip
     startup-registration updates after a failed read. the mutex keeps a losing
     launch from racing the live instance's settings I/O.
  2. initialize the logger, set the log level, log the session config
  3. startup registration, then `restoreOrphanedSessionMutes()` (must be after
     logger init and under the mutex)
  4. `OleInitialize`, `controller_.initialize(settings)`
  5. create and run `MainWindow` with the startup settings snapshot and load
     status, then `controller_.shutdown()`
- The pipeline runs while replay is enabled; `recording_` only controls whether
  packets are muxed to a file. `startRecording` and `saveReplay` call
  `ensurePipeline()` when it is stopped, and `stopRecording` calls
  `stopPipeline()` only when replay is disabled.
- A recording stop drains by encode-thread acknowledgement, not by PTS:
  `stopRecording` starts a `RecordingCutoffTracker` stop at the cutoff PTS and
  waits (5 s, on `recordingCutoffComplete_`) for the encode thread to
  acknowledge every pre-stop frame was published. The encode thread feeds
  submissions (`noteFrameSubmitted`, which latches the post-stop boundary) and
  publications (`notePacketPublished`) into the tracker, and the muxer accepts
  only packets at or below the cutoff while a stop is pending
  (`publishVideoPacket`, shared with the recreate drain). With replay disabled
  `stopPipeline` drains the frame queue and encoder synchronously on the
  stopping thread, so the completed drain is the acknowledgement and no CV
  wait runs. PTS alone cannot cut the recording (finding 08): B-frame
  reordering lets a post-stop P frame complete before a pre-stop B frame, so a
  highest-PTS comparison would strand the pre-stop tail.
- The pipeline-recreate predicate treats a missing encoder as a fault
  (`pipelineEncoderFaulted`): after a full GPU recreate where D3D init
  succeeded but capture init failed, `encoder_` is null, and the health check
  must still recreate (finding 06).
- Encoder-fault recreates back off (`EncoderFaultBackoff`, 500 ms doubling to
  8 s) when a fault repeats before the rebuilt pipeline published a packet; a
  fault after a published packet keeps the 500 ms retry. The PTS baseline is
  sampled after `recreateGpuPipeline` returns, so the broken pipeline's own
  drain tail cannot reset the streak. A device-removal recreate stays at
  500 ms, and the sleep happens before the recreate so the warning names the
  fault time.
- Threads and their locking:
  - `captureLoop`, `encodeLoop`, `audioLoop`: each has a thin wrapper that
    catches everything around an `*Impl` body. `beginStartupSweep` wraps its
    sweep lambda the same way. New threads follow the same split.
  - `pipelineGpuMutex_` serializes GPU resource teardown and recreation.
    Per-operation immediate-context access uses `D3DDevice::immediateContextMutex()`.
    The encode worker starts before the capture worker can request a discard.
  - `stopPipeline` drains `frameQueue_` instead of discarding it, so
    `encodeLoopImpl` leaves its loop only when `stopRequested_` is set and the
    queue is empty and no duration update is pending. Keep that condition whole:
    tightening it silently drops queued frames on shutdown and on settings
    switches.
  - `stopPipeline` separates the capture/video stop flag (`stopRequested_`)
    from the audio-producers-finished signal (`audioProducersFinished_`). The
    writer loop (`drainAudioQueues` in `AudioDrain.h`) exits only on
    `audioProducersFinished_`, not on `stopRequested_`: a producer holding a
    packet when the stop lands must enqueue it before the writer exits.
    `stopPipeline` joins the capture and encode workers, then joins both
    WASAPI producers (`systemAudio_.stop()`, `microphoneAudio_.stop()`), and
    only then sets `audioProducersFinished_` and signals the writer to drain
    and exit. The flag is reset in `ensurePipeline`.
  - `stopPipeline` joins the capture worker before it stops system audio. The
    capture loop's 2 s sound-separation poll restarts WASAPI, and
    `WasapiCapture::start`/`stop` share one unsynchronized `std::thread`, so
    stopping audio first is a data race on that object.
  - `refreshSystemAudioCapture` re-applies the sound-separation mutes on every
    2 s capture-loop poll, not only when the key list changes, so a replaced or
    restarted session and a mute the app cleared are caught before their audio
    reaches a recording. When `applySessionMutesForExecutables` cannot establish
    the exclusion (mute failure, unknown previous state, journal failure, failed
    enumeration) the poll stops system audio and keeps it stopped: recording
    audio the user asked to exclude is worse than a video-only clip. It logs one
    error per failure episode and resumes capture when the exclusion holds again.
    `systemAudioExclusionFailed_` and `systemAudioMutedExecutableKeys_` are
    guarded by `systemAudioCaptureMutex_`; `stopPipeline` resets both.
  - `stopPipeline`'s thread joins are unbounded and must stay that way. A worker
    wedged inside a vendor call cannot be abandoned - a detach would let it
    touch a destroyed `RecorderController`, terminating would throw away the
    user's clip - and `std::thread::join` has no timed form. The `info` line that
    names the worker immediately before each join is the only diagnostic a real
    hang leaves. `kGpuPipelineStopTimeout` (30 s) covers waits that do have an
    abort path, such as `recreateGpuPipeline`'s encode-loop discard. Do not turn
    a join into a detach, a watchdog, or `std::terminate`.
  - `startupSweepThread_` runs `RecoveryStore::sweepOrphanedRecoveryData` and
    inspects the legacy ring folder. `startRecording`, `saveReplay`,
    `recoverFailedRecording`, and `shutdown` wait for it first so recovery and
    new saves cannot overlap.
- The capture loop checks `scaler_.poolGeneration()` around `scale()` under
  `pipelineGpuMutex_` and resets encoder input registrations on change. A failed
  reset faults the encoder; the loop recreates the pipeline instead of trusting
  stale registrations.
- The scaler's output pool is the encoder-feeding pool and is sized from
  `encoderFeedingPoolSizeFor(activeFrameQueueLimit, encoder_->inputRetentionBudget())`
  (queued + retained + held-frame slack, clamped 5..64), negotiated after
  encoder initialize and re-applied on every handoff/recreate. Deep NVENC
  lookahead retains accepted inputs until their output is harvested, so a
  queue-only pool would exhaust and starve the producer; the capture-side
  pools are intermediate and keep their queue-derived size.
- WGC resize and source handoff wait for the encode worker to discard the old
  queue, then reset encoder inputs before releasing capture-held and discarded
  frames. `IEncoder::resetInputResources()` is fallible: false means a driver
  mapping, lease, or registration is still live, so the handoff/recreate path
  aborts and keeps the source, held frames, and discarded frames intact. A full
  device recreate follows the same rule and never copies from the old device.
  Source handoff drops the freeze frame rather than copying a potentially mapped
  input. Encoders requesting a non-BGRA format always receive an owned scaler
  output, including on scaler fallback. NVENC unmaps each input after the output
  carrying its timestamp completes, so BGRA passthrough no longer leaves a
  cached mapping across producer access.
- `setUiPreferences` publishes committed log level and library view without a
  pipeline restart. queued `updateSettings` actions preserve these preferences,
  including commits made while an action is in flight.
- `updateSettings` restarts the pipeline only when it must. Voice-command,
  replay-buffer, and live audio/cursor changes (volumes, AGC enable, denoiser
  enable, cursor capture) apply in place through the lock-free mirrors; a
  volume drag must never split the replay timeline or the active recording.
  Anything else takes the full stop/start path.
- Per-second reads never copy `AppSettings` (paths and vectors allocate).
  `stats()`, the stall watchdog, the capture-loop cursor poll, and the audio
  volumes read the mirrors published by `publishLiveMirrors`, which runs
  wherever `settings_` is assigned. Keep new hot reads off `settings()`.
- Exclusive-fullscreen recovery state lives in the `CaptureSession`
  supervisor in `RecorderController.h` (explicit `toX()` transitions); the
  loop keeps frame data and clocks only. Partial resets write `session`
  fields directly. Microphone gain runs on canonical floats through
  `audio/applyMuteGain`; `sanitizeSettings` owns the 0-200 % range, so the
  call site never re-clamps.
- HDR capture is automatic, never a setting. `createCaptureSource` prefers
  Desktop Duplication on an advanced-color monitor (WGC stays fixed-BGRA and
  leans on DWM pre-tonemap, which washes out); WGC remains the fallback when DD
  is unavailable or fails, and `exclusiveRecovery` keeps its monitor-only DD
  path untouched. Once DD is live on an HDR monitor the session is sticky: a
  single ACCESS_LOST/INVALID_CALL retries DD once before the WGC fallback, so
  one transient loss never flaps the backend. A DD session also pins one HDR
  interpretation for its whole lifetime (capture owns the ratchet): SDR
  surfaces bridge into the HDR mapping with no pool recreate and no GOP break,
  so the capture loop's flip watch only ever fires on the one-time SDR->HDR
  upgrade, never on the surface oscillation. The watch itself stays - a flip
  still opens a new GOP (`requestKeyFrame` plus an `L"app"` log line) so a
  genuine interpretation change never stretches into a running GOP - with flips
  coalesced to one GOP restart per 2 s window and a >5-flips/min storm logging
  one `L"app"` warning per minute window.
- `RecordingStats::hdrActive` / `hdrRejected` / `hdrFlipCount` / `hdrFormat` /
  `hdrPinned` / `hdrBridgedFrames`
  are lock-free mirrors for the
  Diagnostics page. `hdrMonitorActive_` caches the `monitorHasAdvancedColor`
  probe at source creation; `hdrActive_` follows the last acquired frame's
  `GpuFrame::hdr`, `hdrFormat` its format, `hdrPinned_` the source pin and
  `hdrBridgedFrames_` the source bridge count (both read through the
  `ICaptureSource` defaults on WGC, so they stay false/0 there); `hdrFlipCount`
  counts flips per pipeline run (reset at pipeline start, never on handoff, so
  history survives, as does the bridged count). `stats()` ORs the scaler's sticky rejection - read inside
  the existing `try_to_lock` won-lock scope, race-free because the scaler is
  only touched under `pipelineGpuMutex_` - gated on live HDR input so a stale
  latch cannot paint SDR content as rejected, with the monitor-probe-no-frame
  case covering WGC/SDR interpretation. `hdrActive_`/`hdrFormat`/`hdrPinned_`
  reset on every video
  handoff (`resetVideoHandoff`, `switchCaptureTarget`); the outgoing frame is
  dropped before rebinding, so an HDR frame cannot leak into a WGC session.
  The monitor probe is creation-time
  only; an OS HDR toggle mid-session on WGC stays invisible until the next
  rebind (diagnostics-only known limit), while a DD session absorbs the toggle
  by bridging (the one-time SDR->HDR upgrade still arrives via the per-frame
  flag plus the resolution hook). The Diagnostics page splits `hdrRejected` for display -
  scaler refusal on live HDR input reads as rejected, monitor-probe-no-frame
  reads as SDR-on-HDR-monitor (WGC/DWM) - so the washout witness never looks
  like a driver refusal and `stats()` fields stay stable. The HDR line appends
  backend + format + flip count (`hdrFlipCount`/`hdrFormat`), plus `pinned` and
  the bridged count on a pinned session, so stability is
  self-serve; no new setting, HDR stays automatic.
- `stats()` and `encoderCapabilities()` run on two threads at once - the UI
  thread (diagnostics timer, page refresh) and the controller worker thread
  `MainWindow` uses for blocking actions - and must never block on
  `pipelineGpuMutex_`, which a GPU recreate holds for seconds.
  The sanctioned pattern is
  `std::unique_lock(pipelineGpuMutex_, std::try_to_lock)`: publish
  `lastEncoderStats_` / `lastEncoderCapabilities_` when the lock is won and
  report the previous snapshot when it is not. Both members are
  `std::atomic<std::shared_ptr<const T>>`, never plain values: under contention a
  plain member copy tears `EncoderCapabilities`' `std::wstring`s, and taking the
  lock to read them back would queue the UI behind a GPU recreate. Prefer a
  lock-free mirror such as `encoderZeroReorderDelay_` when one is enough.
  `ReplayBuffer::videoPacketCount` / `videoKeyFrameCount` are lock-free mirrors
  for exactly this reason, and `replayStorageFallback()` reads
  `ReplayBuffer::storageFallbackReason()`, which deliberately uses its own lock
  rather than the buffer mutex. Keep anything the Diagnostics page reads off the
  `ReplayBuffer::mutex_`; taking it here would queue the UI behind the encode
  thread's synchronous ring write.
- `replay_.configure`'s capacity argument is derived from `replay.seconds`, so it
  changes on every replay-window edit. `ReplayBuffer` treats it as a disk-usage
  ceiling, not as a backend switch: only a backend or ring-directory change
  clears the buffer, and a capacity change is deferred until the ring is empty.
  Do not reintroduce a capacity comparison as a switch condition; it silently
  wiped the whole buffer on a window-length edit.
- `frameQueue_`'s ring storage is fixed for the queue's lifetime at
  `frameQueueCapacityFor`'s ceiling (16), so `resetCapacity` only lowers a
  logical limit and never reallocates; change the constructor and that clamp
  together. `size()`/`capacity()` are safe for the UI diagnostics timer to
  read while a pipeline restart resets the queue.
- `beginStartupSweep` preserves ambiguous legacy ring folders. new ring files
  are exclusively created and deleted on handle close. per-game output folders
  escape dot-prefixed names before creation and fall back to the library root
  for reparse-point game folders. failed per-game saves remain recoverable from
  the configured clip library. new clip outputs are returned only after
  exclusive name reservation succeeds.
- Frame cadence, PTS, and frame durations are decided by
  `VideoTimelineScheduler`. Keep that header free of Windows, D3D, and project
  includes so `VideoTimelineSchedulerTests.cpp` still compiles standalone with
  `cl`. `advanceVideoCadence` deliberately collapses an interval only past a
  half-frame overshoot; do not "simplify" that slack away, it is what keeps
  timer jitter from halving the effective frame rate.
- Unmatched video-duration extensions between encode submissions and the
  encoder outputs that consume them are tracked in `app/PendingDurationTracker.h`
  (`PendingDurationTracker`, cap 512). Bookkeeping stays bounded even when an
  encoder input is dropped: eviction drops the smallest PTS (the oldest
  submitted frame) when the cap is reached, never the front of an insertion
  order that an orphaned key could pin open (finding 22). Entries leave by the
  matching packet (`consume`, coalescing the stored extension into the packet
  duration) or by `erase` when the muxer or replay buffer already extended.
- Counters in `RecordingStats` are diagnostics and must stay truthful: increment
  the specific counter for every frame or packet actually dropped, coalesced, or
  duplicated. The capture loop's two video drop branches stay separate:
  `shouldDropForGpuProtection` is the proactive queue-depth heuristic and owns
  `gpuProtectionDrops`, while a `frameQueue_.tryPush` failure is encode-thread
  backpressure and reaches `droppedFrames` alone. Merging them with `||` bills
  the heuristic for every encoder stall.

## Work Guidance

- Prefer fixing a decision at its source (the scheduler, the encoder
  capabilities, the capture source) over branching on it in the loops.
- When adding a pipeline stage, define where it sits in the queue chain and
  which thread owns it before writing the code; the SPSC queues assume exactly
  one producer and one consumer each.
- `MicrophoneAgc` and the audio-writer packet helpers (canonical gain, block
  levels, noise suppression) live in `app/AudioWriter.h/.cpp`; the denoiser
  follows the same pattern behind `BACKTRACK_HAS_NOISE_SUPPRESSION`. Keep both
  on the audio writer thread: only `RecorderController::writeAudioPacket` calls
  them. Member state (mirrors, scratch buffers) stays in `RecorderController`.
  The denoiser rewrites the packet's PTS to the oldest pending sample position
  so fragmented packets stay contiguous on the pacer's timeline; see the
  `RnnoiseDenoiser` bullet in audio/AGENTS.md.
- `captureBackendDisplayName` lives in `platform/Win32Util.h`, shared by `app`
  status lines and the UI so both name a backend identically.
  `RecorderController.cpp` must not reach into `ui`; a second cross-boundary
  need belongs here as an ownership note rather than staying accidental.

## Verification

- `VideoTimelineSchedulerTests` (CTest `VideoTimelineSchedulerTests`, also
  compilable standalone) covers cadence collapse, coalescing, and duration
  math, including the `maxCoalescedDuration100ns` cap on held-frame durations.
  Extend it for any scheduler change.
- `RecorderControllerCutoffTests.cpp` covers the recording-stop cutoff
  handshake (finding 08: the delayed pre-cutoff tail stays in the recording,
  the ack waits on the post-stop boundary and the publication quiesce, and a
  dropped frame cannot stall the stop), the missing-encoder fault predicate
  (finding 06), and the encoder-fault recreate backoff schedule.
- `RecorderControllerLeaseTests.cpp` exercises the frame-handoff ordering with a
  lease-checking source.
- `PendingDurationTrackerTests.cpp` covers insert/coalesce/consume, smallest-PTS
  eviction, and the finding-22 regression (one unmatched PTS plus 100,000
  consumed updates keeps bookkeeping bounded without a restart).
- `RecorderControllerShutdownOrderingTests.cpp` exercises the audio-writer drain
  loop (`drainAudioQueues` in `AudioDrain.h`): a late packet enqueued after the
  stop request but before the producers-finished flag is consumed before the
  writer exits, and packets queued before the flag drain on signal.
- `MainWindowSettingsTests.cpp` exercises settings handoff and ui preferences
  without a capture pipeline.
- Everything else in this folder is verified by building and by exercising the
  pipeline in the app; the Diagnostics page reports the counters above.

## Child DOX Index

None. This folder's children are files only.