# audio

## Purpose

Audio in and out of the recorder: WASAPI system and microphone capture,
per-process loopback for sound separation, conversion to one canonical ingest
format, the mixed track handed to the muxer, and the optional RNNoise
microphone denoiser.

## Ownership

- Owns: `WasapiCapture`, `AudioConvert`, `AudioMix`, `WavReader`,
  `RnnoiseDenoiser`.
- Does not own: WAV or MP4 writing (`mux/WavWriter.h`, `mux/Mp4Muxer.h`), or
  the microphone AGC, which is implemented inside `app/RecorderController.cpp`.

## Local Contracts

- Canonical ingest format is stereo float32 at 48 kHz (`kCanonical*` in
  `AudioConvert.h`). Every audio packet must pass
  `convertAudioPacketToCanonical` before it reaches a writer. A `false` return
  means the format could not be interpreted: drop the packet, never write its
  bytes into a stream that declares a different format.
- The reason for canonicalization is that WASAPI silently changes mix format
  across reconnects (headset swap, default device change). Without it the WAV
  writers keep appending raw bytes declared in the first packet's format, which
  produces pitch-shifted audio and timeline skew.
- Accepted inputs are PCM at 8, 16, 24 or 32 bits per sample and 32-bit float,
  at any sample rate. Mono duplicates to both outputs, stereo passes through,
  and three or more channels fold down per ITU-R BS.775: front left/right at
  unity, center and surrounds at -3 dB into their own side, LFE omitted. The
  mask comes from `dwChannelMask` when its popcount matches the channel count
  and from the standard Windows speaker order otherwise. Gains are normalized so
  full-scale input on several channels cannot clip, and a mask that folds onto
  one side only falls back to plain front-left/front-right selection.
- Resampling interpolates linearly over an exact rational source position, and
  the last output frame clamps to the last source frame instead of
  extrapolating. `kCanonical*` and `kMux*` in `AudioMix.h` are checked against
  each other by `static_assert` in `AudioConvert.cpp` for sample rate and
  channel count, which must stay equal. Bit depth deliberately differs: ingest
  is 32-bit float, the mux target is 16-bit PCM.
- PTS is never touched by conversion; it stays the authoritative timeline
  anchor. The capture thread produces it as a locally accumulated
  `nextPacketPts100ns`, resynced to the device clock (QPC plus a steady-clock
  offset, or steady now minus the packet duration when the buffer carries
  `AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR`) on the first packet, on
  `AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY`, or once drift passes
  `kAudioTimestampResyncThreshold100ns` (50 ms).
- The capture thread never gives up on a lost client. Activation, `GetMixFormat`,
  `Initialize`, `GetBuffer` and `ReleaseBuffer` failures - including
  `AUDCLNT_E_DEVICE_INVALIDATED` on an unplugged or swapped endpoint - clear
  `running_` and retry with 250 ms exponential backoff capped at 2 s until
  `stop()`. `start()` reports only the first attempt, so a start that returns
  false can still be followed by a successful reconnect.
- Session mutes are global system state. `WasapiCapture` mirrors them into
  `audio-session-mutes.txt` under `%LOCALAPPDATA%\Backtrack`, sweeps orphaned
  mutes at startup, and exposes `emergencyRestoreSessionMutes()` for the fatal
  handlers in `main.cpp`. Sound separation is mute-based, so getting this wrong
  leaves other applications silent until the next clean run. The sweep restores
  only sessions still muted, so a user who unmuted one by hand keeps that, and
  it deletes the journal only after a sweep that actually ran, rewriting the
  entries it could not reach so a failed enumeration leaves them for the next
  launch. The journal is written after every applied mute rather than once per
  sweep: `g_sessionMuteMutex` is held across the whole enumeration and the crash
  path only try_locks it, so a kill inside that walk has no second chance to be
  recorded. Any thread that calls into a session volume needs its own COM
  apartment, including the fatal handlers in `main.cpp` (OS-created or faulting
  threads); `emergencyRestoreSessionMutes` therefore initializes one and ignores
  a changed-apartment result, because such a thread is initialized all the same.
  Every mute key comes from `platform/Win32Util.h:normalizedPathKey`, which
  `RecorderController` and the UI also key by: see that header's contract.
- `ProcessLoopbackMode` (`IncludeTargetProcessTree` / `ExcludeTargetProcessTree`)
  selects per-process capture with or without the target's child processes.
- Microphone processing order on the audio writer thread: canonicalization,
  denoise (if enabled), AGC, then volume. The denoiser is microphone-only and
  requires the canonical format; system audio is never processed.
- `RnnoiseDenoiser` is opt-in, not thread-safe, and works in whole 480-sample
  frames. `process` may emit one buffered frame more than it consumed, so the
  output buffer needs `count + 480` headroom or the backlog grows without bound.
  Stream length is preserved cumulatively, not per call: at most a sub-frame
  remainder stays buffered, and the caller resizes the packet to what was
  produced. That is what keeps packet timing and replay sync intact.
- `RnnoiseDenoiser.cpp` carries no `#ifdef` of its own; the build files drop it
  from the source list unless `BACKTRACK_HAS_NOISE_SUPPRESSION` is defined. The
  settings field, its `audio.inputNoiseSuppression` key, and the UI checkbox are
  each wrapped in that same `#ifdef`.
- `buildMixedPcmAudio` resamples each recorded WAV to the mux rate, sums the
  tracks instead of averaging (each track's gain is already baked into its WAV),
  clamps, and writes 16-bit stereo at `kMuxAudioSampleRate` to `mixed_audio.wav`
  beside the system WAV. An unreadable source is skipped with a warning; a
  failed mixed create or write returns false rather than handing the muxer a
  missing or truncated audio track.
- `parseWav` reads the recorded WAVs back for mixing and validates
  `mixed_audio.wav` before `Mp4Muxer` publishes. It trusts the declared RIFF
  extent only while it is plausible: a process killed before
  `WavWriter::patchHeader()` leaves zeroed or stale sizes, and the parser falls
  back to the real file size so audio already on disk stays recoverable.
  `ui/UiHelpers.cpp` keeps its own minimal loader for notification sounds.

## Work Guidance

- Dropping a packet is the correct response to an unusable format. Padding or
  guessing a rate is not.
- Device and audio-session enumeration (`enumerateDevices`,
  `enumerateAudioSessionApps`, `enumerateOpenApps`) is static and called from the
  UI thread on demand. Keep it side-effect free.

## Verification

- `AudioConvertTests` (CTest) covers format parsing, canonicalization, resampling
  with mono duplication, a mid-stream format change, an unsupported format being
  dropped, and the 5.1 fold-down.
- `ReplayBufferTests` (CTest) links `WavReader.cpp` and covers a well-formed WAV
  and one whose RIFF and data sizes were never patched.
- `AudioDenoiseTests` (CTest, only with `BACKTRACK_NOISE_SUPPRESSION=ON`) covers
  chunk-boundary buffering and stream-length conservation, `reset`, and
  level-independent suppression.
- Everything else is verified by building and by listening: WASAPI capture and
  its reconnect, the mixed track, and sound separation are the only real checks.

## Child DOX Index

None. This folder's children are files only.