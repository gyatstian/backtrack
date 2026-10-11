# app

## Purpose

Application lifetime and the recording pipeline. `Application` is the startup
sequence, `RecorderController` owns every worker thread and the objects they
share, and `VideoTimelineScheduler.h` is the header-only frame-cadence logic.

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
  1. load settings, single-instance mutex (`Local\Backtrack.SingleInstance`),
     activate an existing window and exit if one is running
  2. initialize the logger, set the log level, log the session config
  3. startup registration, then `restoreOrphanedSessionMutes()` (must be after
     logger init and under the mutex)
  4. `OleInitialize`, `controller_.initialize(settings)`
  5. create and run `MainWindow`, then `controller_.shutdown()`
- The pipeline runs continuously while replay is enabled; `recording_` only
  controls whether packets are muxed to a file. Starting recording does not
  start capture, and stopping recording does not stop it.
- Threads and their locking:
  - `captureLoop`, `encodeLoop`, `audioLoop`: each has a thin wrapper that
    catches everything around an `*Impl` body. New threads follow the same
    split.
  - `pipelineGpuMutex_` serializes GPU resource teardown and recreation.
    Per-operation immediate-context access uses `D3DDevice::immediateContextMutex()`.
  - `startupSweepThread_` runs `RecoveryStore::sweepOrphanedRecoveryData`.
    `startRecording`, `saveReplay`, `recoverFailedRecording`, and `shutdown`
    wait for it first: the sweep deletes recovery data that those paths read.
- `stats()` is called from the UI thread and must stay non-blocking. Read
  atomics only. Do not take `pipelineGpuMutex_` or the encoder mutex there; use
  the mirrored `lastEncoderCapabilities_` / `lastEncoderStats_` snapshots or a
  lock-free mirror such as `encoderZeroReorderDelay_`.
- Frame cadence, PTS, and frame durations are decided by
  `VideoTimelineScheduler`. Keep that header free of Windows, D3D, and project
  includes so `VideoTimelineSchedulerTests.cpp` still compiles standalone with
  `cl`. `advanceVideoCadence` deliberately collapses an interval only past a
  half-frame overshoot; do not "simplify" that slack away, it is what keeps
  timer jitter from halving the effective frame rate.
- Counters in `RecordingStats` are diagnostics and must stay truthful: increment
  the specific counter for every frame or packet actually dropped, coalesced, or
  duplicated.

## Work Guidance

- Prefer fixing a decision at its source (the scheduler, the encoder
  capabilities, the capture source) over branching on it in the loops.
- When adding a pipeline stage, define where it sits in the queue chain and
  which thread owns it before writing the code; the SPSC queues assume exactly
  one producer and one consumer each.
- `MicrophoneAgc` is declared in `RecorderController.h` but implemented in
  `RecorderController.cpp`; the denoiser follows the same pattern behind
  `BACKTRACK_HAS_NOISE_SUPPRESSION`. Keep both on the audio writer thread.

## Verification

- `VideoTimelineSchedulerTests` (CTest `VideoTimelineSchedulerTests`, also
  compilable standalone) covers cadence collapse, coalescing, and duration
  math. Extend it for any scheduler change.
- Everything else in this folder is verified by building and by exercising the
  pipeline in the app; the Diagnostics page reports the counters above.

## Child DOX Index

None. This folder's children are files only.