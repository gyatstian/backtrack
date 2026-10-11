# audio

## Purpose

Audio in and out of the recorder: WASAPI system loopback and microphone capture,
conversion to one canonical ingest format, the mixed track handed to the muxer,
and the optional RNNoise microphone denoiser.

## Ownership

- Owns: `WasapiCapture`, `AudioConvert`, `AudioMix`, `WavReader`,
  `RnnoiseDenoiser`.
- Does not own: WAV or MP4 writing (`mux/WavWriter.h`, `mux/Mp4Muxer.h`), or
  the microphone AGC, which is implemented inside `app/RecorderController.cpp`.

## Local Contracts

- Canonical ingest format is stereo float32 at 48 kHz (`AudioConvert.h`
  constants). Every audio packet must pass `convertAudioPacketToCanonical`
  before it reaches a writer. A `false` return means the format could not be
  interpreted: drop the packet, never write its bytes into a stream that
  declares a different format.
- The reason for canonicalization is that WASAPI silently changes mix format
  across reconnects (headset swap, default device change). Without it the WAV
  writers keep appending raw bytes declared in the first packet's format, which
  produces pitch-shifted audio and timeline skew.
- PTS is never touched by conversion. It stays the authoritative timeline
  anchor.
- `kCanonical*` in `AudioConvert.h` and `kMux*` in `AudioMix.h` are checked
  against each other by `static_assert` in `AudioConvert.cpp` for sample rate
  and channel count, which must stay equal. Bit depth deliberately differs:
  ingest is 32-bit float, the mux target is 16-bit PCM.
- Session mutes are global system state. `WasapiCapture` owns the journal under
  local app data, restores orphaned mutes at startup, and exposes
  `emergencyRestoreSessionMutes()` for the fatal handlers in `main.cpp`. Sound
  separation is mute-based, so getting this wrong leaves other applications
  silent until the next clean run.
- `ProcessLoopbackMode` (`IncludeTargetProcessTree` / `ExcludeTargetProcessTree`)
  selects per-process capture with or without the target's child processes.
- Microphone processing order on the audio writer thread: denoise (if enabled),
  then AGC, then volume. The denoiser is microphone-only; system audio is never
  processed.
- `RnnoiseDenoiser` is opt-in, not thread-safe, and works in whole 480-sample
  frames. `process` may emit one buffered frame more than it consumed, so the
  output buffer needs `count + 480` headroom or the backlog grows without bound.
  It preserves the sample count exactly, which is what keeps packet timing and
  replay sync intact.
- Everything RNNoise-related is behind `BACKTRACK_HAS_NOISE_SUPPRESSION`,
  including the settings field and the UI checkbox.

## Work Guidance

- Dropping a packet is the correct response to an unusable format. Padding or
  guessing a rate is not.
- Device and audio-session enumeration (`enumerateDevices`,
  `enumerateAudioSessionApps`, `enumerateOpenApps`) is static and called from the
  UI thread on demand. Keep it side-effect free.
- Comments here explain timing and format hazards, since both are easy to break
  silently and produce audio that is subtly wrong rather than obviously broken.

## Verification

- `AudioConvertTests` (CTest) covers format parsing and canonicalization.
- `AudioDenoiseTests` (CTest, only with `BACKTRACK_NOISE_SUPPRESSION=ON`) covers
  the denoiser.
- Everything else is verified by building and by listening: system audio,
  microphone, and sound separation are the only real checks.

## Child DOX Index

None. This folder's children are files only.