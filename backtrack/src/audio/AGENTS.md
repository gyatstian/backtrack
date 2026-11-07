# audio

## Purpose

Audio in and out of the recorder: WASAPI system and microphone capture,
per-process loopback for sound separation, conversion to one canonical ingest
format, the mixed track handed to the muxer, and the optional RNNoise
microphone denoiser.

## Ownership

- Owns: `WasapiCapture`, `SessionMuteStore`, `AudioConvert`, `AudioMix`, `WavReader`,
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
- accepted inputs are PCM at 8, 16, 24 or 32 bits per sample and 32-bit float,
  at any nonzero sample rate. the frame stride must be nonzero and fit every
  sample; padded frames are repacked rather than passed through as canonical.
  mono duplicates to both outputs, stereo stays stereo, and three or more
  channels fold down per ITU-R BS.775: front left/right at unity, center and
  surrounds at -3 dB into their own side, LFE omitted. The
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
- an empty endpoint ID follows the `eConsole` default for the track's render or
  capture flow. the worker subscribes before resolving and rebuilds on a
  matching default change, even if the previous client is healthy. registration
  failures retry; explicit IDs stay pinned and retry if unavailable. process
  loopback does not follow endpoint notifications.
- endpoint notifications are registered and unregistered on the capture COM
  thread. callbacks only mark a generation and signal a duplicated event; they
  never access the capture object. a failed unregister retains the disabled
  callback and event for process lifetime to avoid a dangling system registration.
- session mutes are global system state. `SessionMuteStore` journals them in
  `audio-session-mutes.txt` under `%LOCALAPPDATA%\Backtrack` and attempts a
  startup sweep. fatal handlers attempt `emergencyRestoreSessionMutes()` before
  logging; the emergency path only try_locks. keys use
  `platform/Win32Util.h:normalizedPathKey`.
- live mutes are tracked per session instance - endpoint id plus
  `IAudioSessionControl2::GetSessionInstanceIdentifier` - not per PID, because
  one process can own several render sessions (one per endpoint, plus a
  replacement after it recreates its stream). a record whose exact instance is
  gone is retired: Windows keeps a mute per session, so the mute died with it.
  a wanted session that is no longer muted is re-asserted on every apply.
- the journal keeps the durable identity only: PID and path key. session
  instance identifiers do not survive an application restart, so a startup
  sweep pairs one record with one still-muted live session and restores that
  record's own previous state. do not add an instance id to the journal and
  expect it to match after a restart.
- each mute is journaled before the session is touched, and a journal failure is
  reported to the caller instead of ignored. `apply` returns
  `SessionMuteApplyResult`: `exclusionEstablished()` is false on a mute failure,
  an unknown previous state, a journal write failure, or a failed enumeration.
  `SessionMuteCandidate::muteStateKnown` is false when `GetMute` fails, and such
  a session is never muted - an unverifiable previous state is not a safe change
  to the user's audio.
- a record is retired only after its restore is verified (`SetMute` succeeded, or
  the session is already unmuted) or when its session/process is gone. failed
  restores and unreachable journal obligations stay journaled and are retried; a
  later apply merges them with its own mutes instead of overwriting them. a
  fully restored journal is deleted, including after a complete startup sweep.
- COM ownership: the store holds no COM interface. `WasapiCapture` initializes
  the apartment for the whole store call, and the store invokes every callback
  and releases every candidate before returning, so enumeration, callback
  invocation, and interface release all happen on one initialized apartment. no
  interface escapes to another thread or a later call. the emergency path
  re-enumerates on its own apartment scope and leaves an unreadable journal in
  place.
- `ProcessLoopbackMode` (`IncludeTargetProcessTree` / `ExcludeTargetProcessTree`)
  selects per-process capture with or without the target's child processes.
- Microphone processing order on the audio writer thread: canonicalization,
  denoise (if enabled), AGC, then volume. The denoiser is microphone-only and
  requires the canonical format; system audio is never processed.
- Gain after canonicalization is `applyMuteGain` over the float span alone
  (muted zeroes, unity is a no-op, otherwise scale and clamp). The percent
  range belongs to `sanitizeSettings`; the call site never re-clamps.
- `RnnoiseDenoiser` is opt-in, not thread-safe, and works in whole 480-sample
  frames. `process` may emit one buffered frame more than it consumed, so the
  output buffer needs `count + 480` headroom or the backlog grows without bound.
  Stream length is preserved cumulatively, not per call: at most a sub-frame
  remainder stays buffered, and the caller resizes the packet to what was
  produced. The model is trained on int16-cast-to-float samples, so `process`
  converts normalized floats to PCM16 scale (x32768, inside the conditioning
  gain) before the model and back afterwards; the units matter because the
  model's features are nonlinear log band energies, and the reciprocal gain
  cannot compensate for feeding it the wrong scale. `process` takes the
  packet's PTS and stamps the output with the OLDEST pending sample's
  position, so fragmented packets stay exactly contiguous on the WAV pacer's
  integer timeline; a packet whose PTS is not contiguous with the previous one
  (dropped packet, device switch, capture resync) resets the denoiser and
  restarts the output timeline at that packet's PTS. At PCM16 scale the
  vendored silence gate (the local 1e-9 patch or the upstream 0.04) fires only
  on true digital silence, so the patch's quiet-mic motivation is moot; it
  stays in the vendored file, which is out of scope here.
- `RnnoiseDenoiser.cpp` carries no `#ifdef` of its own; the build files drop it
  from the source list unless `BACKTRACK_HAS_NOISE_SUPPRESSION` is defined. The
  settings field, its `audio.inputNoiseSuppression` key, and the UI checkbox are
  each wrapped in that same `#ifdef`.
- `buildMixedPcmAudio` resamples each recorded WAV to the mux rate, sums the
  tracks instead of averaging (each track's gain is already baked into its WAV),
  clamps, and writes 16-bit stereo at `kMuxAudioSampleRate` to `mixed_audio.wav`
  beside the system WAV. A track is absent only when its path is empty or the
  file genuinely does not exist; an expected track that cannot be read back
  (locked, permission denied, corrupt header) fails the save so recovery
  preserves the recording instead of publishing a silent clip. A valid empty
  WAV is skipped. A failed mixed create or write returns false rather than
  handing the muxer a missing or truncated audio track.
- `parseWav` reads the recorded WAVs back for mixing and validates
  `mixed_audio.wav` before `Mp4Muxer` publishes. It trusts the declared RIFF
  extent only while it is plausible: a process killed before
  `WavWriter::patchHeader()` leaves zeroed or stale sizes, and the parser falls
  back to the real file size so audio already on disk stays recoverable.
  `buildMixedPcmAudio` additionally extends a data extent that parseWav had to
  cap at the 4 GiB RIFF boundary (WavWriter's large-file form, data size
  clamped to the uint32 maximum) to the real file size, so long recordings mix
  in full.
  `ui/UiHelpers.cpp` keeps its own minimal loader for notification sounds.

## Work Guidance

- Dropping a packet is the correct response to an unusable format. Padding or
  guessing a rate is not.
- Device and audio-session enumeration (`enumerateDevices`,
  `enumerateAudioSessionApps`, `enumerateOpenApps`) is static and called from the
  UI thread on demand. Keep it side-effect free.

## Verification

- `AudioConvertTests` (CTest) covers format parsing, supported and unsupported
  encodings and widths, invalid strides, padded-frame repacking, resampling with
  mono duplication, a mid-stream format change, and the 5.1 fold-down.
- `SessionMuteStoreTests` (CTest) covers the journal codec round trip and
  malformed-line skipping, journaling the intent before the session is touched,
  per-instance tracking of two sessions that share a PID (own previous states,
  replacement of one, re-assertion of a mute the app cleared), a failed mute and
  a failed journal write surfaced to the caller and retried without muting,
  unknown `GetMute` state left untouched, a failed restore retried until it
  succeeds, a partial startup sweep followed by an unrelated apply/clear (the
  orphan is preserved, then retired once its process is gone), a complete sweep
  retiring the journal (including the empty-write-cache case), and callbacks
  never invoked after the enumeration that produced them returned. Needs no COM
  or WASAPI: sessions enter as `SessionMuteCandidate` values with recording
  lambdas.
- `ReplayBufferTests` (CTest) links `WavReader.cpp` and covers a well-formed WAV
  and one whose RIFF and data sizes were never patched.
- `AudioDenoiseTests` (CTest, only with `BACKTRACK_NOISE_SUPPRESSION=ON`) covers
  chunk-boundary buffering and stream-length conservation, `reset`,
  level-independent suppression, fragmented-packet PTS contiguity through the
  pacing path, and discontinuity reset.
- `AudioPersistenceTests` covers the mix source policy and the large-file
  read-back in `buildMixedPcmAudio`: an expected WAV that reads back corrupt or
  locked fails the save, an absent or valid-empty track stays video-only, and a
  valid WAV mixes.
- `WasapiDefaultDeviceTests` (CTest) drives the capture worker with fake
  endpoints: default-device switches, a pinned explicit device, switch during
  resolution and inside a packet callback, default loss and recovery, and
  registration/stop races. real endpoint switching still needs a listening
  check.
- The mixed track and sound separation are verified by building and listening.

## Child DOX Index

None. This folder's children are files only.