# app

## Purpose

Application lifetime and the recording pipeline. `Application` is the startup
sequence, `RecorderController` owns the four worker threads it starts and the
objects they share, and `VideoTimelineScheduler.h` is the header-only
frame-cadence logic.

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
     existing window and exit if one is running, then load settings and save
     them back. `SettingsStore::save` is atomic, so it cannot damage the file a
     live instance owns; the ordering is here because the losing launch must not
     run `load`/`save` at all. Two processes normalizing the same file
     concurrently can still race on the swap, and the settings save-back writes
     whatever `sanitizeSettings` produced from a possibly half-read file.
  2. initialize the logger, set the log level, log the session config
  3. startup registration, then `restoreOrphanedSessionMutes()` (must be after
     logger init and under the mutex)
  4. `OleInitialize`, `controller_.initialize(settings)`
  5. create and run `MainWindow`, then `controller_.shutdown()`
- The pipeline runs while replay is enabled; `recording_` only controls whether
  packets are muxed to a file. `startRecording` and `saveReplay` call
  `ensurePipeline()` when it is stopped, and `stopRecording` calls
  `stopPipeline()` only when replay is disabled.
- Threads and their locking:
  - `captureLoop`, `encodeLoop`, `audioLoop`: each has a thin wrapper that
    catches everything around an `*Impl` body. `beginStartupSweep` wraps its
    sweep lambda the same way. New threads follow the same split.
  - `pipelineGpuMutex_` serializes GPU resource teardown and recreation.
    Per-operation immediate-context access uses `D3DDevice::immediateContextMutex()`.
  - `stopPipeline` drains `frameQueue_` instead of discarding it, so
    `encodeLoopImpl` leaves its loop only when `stopRequested_` is set and the
    queue is empty and no duration update is pending. Keep that condition whole:
    tightening it silently drops queued frames on shutdown and on settings
    switches.
  - `stopPipeline` joins the capture worker before it stops system audio. The
    capture loop's 2 s sound-separation poll restarts WASAPI, and
    `WasapiCapture::start`/`stop` share one unsynchronized `std::thread`, so
    stopping audio first is a data race on that object.
  - `stopPipeline`'s thread joins are unbounded and must stay that way. A worker
    wedged inside a vendor call cannot be abandoned - a detach would let it
    touch a destroyed `RecorderController`, terminating would throw away the
    user's clip - and `std::thread::join` has no timed form. The `info` line that
    names the worker immediately before each join is the only diagnostic a real
    hang leaves. `kGpuPipelineStopTimeout` (30 s) covers waits that do have an
    abort path, such as `recreateGpuPipeline`'s encode-loop discard. Do not turn
    a join into a detach, a watchdog, or `std::terminate`.
  - `startupSweepThread_` runs `RecoveryStore::sweepOrphanedRecoveryData` and
    the replay ring-folder cleanup. `startRecording`, `saveReplay`,
    `recoverFailedRecording`, and `shutdown` wait for it first: the sweep
    deletes recovery data that those paths read.
- Both `scaler_.scale()` sites (the capture loop and `privatizeLastFrame`)
  sample `poolGeneration()` inside the `pipelineGpuMutex_` scope and call
  `IEncoder::resetInputResources()` when it changed. A rebuilt scaler pool
  invalidates the encoder's registered-input pointers and the driver reuses the
  freed addresses, so stale registrations must go before the new pool is
  consumed. Every pool teardown bumps that counter; there is no other signal.
  When `privatizeLastFrame`'s `scale()` fails it drops `lastFrame` outright
  rather than returning early. The raw WGC zero-copy frame carries the
  `Direct3D11CaptureFrame` lease that `switchCaptureTarget`'s source shutdown
  drains, and only a later successful capture could release it, so holding it
  across the swap stalls the capture thread. Losing one freeze frame beats a
  stalled rebind.
- HDR capture is automatic, never a setting. `createCaptureSource` prefers
  Desktop Duplication on an advanced-color monitor (WGC stays fixed-BGRA and
  leans on DWM pre-tonemap, which washes out); WGC remains the fallback when DD
  is unavailable or fails, and `exclusiveRecovery` keeps its monitor-only DD
  path untouched. The capture loop watches every acquired frame's `hdr` flag
  and format: a flip opens a new GOP (`requestKeyFrame` plus an `L"app"` log
  line) so whitened frames never stretch into a running GOP. A DD-side HDR flip
  also bumps `resolutionChangeCount`, which the existing resize hook turns into
  the same keyframe.
- `RecordingStats::hdrActive` / `hdrRejected` are lock-free mirrors for the
  Diagnostics page. `hdrMonitorActive_` caches the `monitorHasAdvancedColor`
  probe at source creation; `hdrActive_` follows the last acquired frame's
  `GpuFrame::hdr`. `stats()` ORs the scaler's sticky rejection - read inside
  the existing `try_to_lock` won-lock scope, race-free because the scaler is
  only touched under `pipelineGpuMutex_` - gated on live HDR input so a stale
  latch cannot paint SDR content as rejected, with the monitor-probe-no-frame
  case covering WGC/SDR interpretation.
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
- `beginStartupSweep` also removes a leftover ring folder from a killed session,
  because the ring files are pre-sized to the full capacity (video up to
  16 GiB, audio up to 4 GiB) and
  `RecoveryStore::sweepOrphanedRecoveryData` only walks the clip library's
  `.backtrack_tmp`. It is gated on disk-backed replay being off for both tracks:
  an enabled ring is truncated and reused by the next `configure`, so there is
  nothing to reclaim and no open handle to race. Only the currently configured folder is
  swept: a ring folder the user has since moved away from is stranded, because
  nothing records where it was and a scan for `*/.backtrack_ring` would have no
  way to tell an abandoned folder from one a running session has open.
- Frame cadence, PTS, and frame durations are decided by
  `VideoTimelineScheduler`. Keep that header free of Windows, D3D, and project
  includes so `VideoTimelineSchedulerTests.cpp` still compiles standalone with
  `cl`. `advanceVideoCadence` deliberately collapses an interval only past a
  half-frame overshoot; do not "simplify" that slack away, it is what keeps
  timer jitter from halving the effective frame rate.
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
- `MicrophoneAgc` is declared in `RecorderController.h` but implemented in
  `RecorderController.cpp`; the denoiser follows the same pattern behind
  `BACKTRACK_HAS_NOISE_SUPPRESSION`. Keep both on the audio writer thread.
- `RecorderController.cpp` includes `ui/UiHelpers.h` for
  `captureBackendDisplayName` alone. That is the one place `app` reaches into
  `ui`; a second one means the boundary has moved and belongs in this file's
  ownership notes rather than staying accidental.

## Verification

- `VideoTimelineSchedulerTests` (CTest `VideoTimelineSchedulerTests`, also
  compilable standalone) covers cadence collapse, coalescing, and duration
  math, including the `maxCoalescedDuration100ns` cap on held-frame durations.
  Extend it for any scheduler change.
- Everything else in this folder is verified by building and by exercising the
  pipeline in the app; the Diagnostics page reports the counters above.

## Child DOX Index

None. This folder's children are files only.