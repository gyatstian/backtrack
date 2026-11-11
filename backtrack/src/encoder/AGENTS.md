# encoder

## Purpose

Hardware video encoding: the backend-neutral interface, the NVIDIA NVENC and
AMD AMF implementations, the vendor factory, and the bitstream helpers they
share.

## Ownership

- Owns: `IEncoder`, `NvencEncoder`, `AmfEncoder`, `EncoderFactory`,
  `EncoderCommon`, `BitstreamUtils`, `DecodeOrderTimestamps.h`.
- Does not own: frame timing (that is `app/VideoTimelineScheduler.h`) or muxing
  (`mux`).

## Local Contracts

- `IEncoder` is the only encoder surface `RecorderController` touches; it never
  includes a backend header. Its header forward-declares `D3DDevice` (the
  backends include `capture/D3DDevice.h` in their own `.cpp`). Add virtuals with a default body rather than
  forcing every backend and call site to change; `resetInputResources()` is the
  exception (pure virtual: no backend may inherit a silent `return true`).
  `encodeFrame` has a
  drain-to-vector overload that appends every ready packet (default pops at
  most one through the single-packet form); the single-packet form returns
  false when no output is ready.
- Codecs are H.264 and HEVC only (`core/Types.h` `VideoCodec`), both configured
  8-bit. NVENC maps the enum onto `NV_ENC_CODEC_H264_GUID` /
  `NV_ENC_CODEC_HEVC_GUID`, AMF onto the `AMFVideoEncoderVCE_AVC` /
  `AMFVideoEncoder_HEVC` components.
- `preferredInputFormat()` is what the capture scaler converts to, and the two
  backends deliberately differ: NVENC registers BGRA directly, AMF takes NV12 so
  the fixed-function video processor does the conversion instead of an AMF
  compute pass. AMF re-creates its component through `switchInputFormat` if the
  scaler falls back to BGRA mid-session. If that would introduce B-frames into
  a previously zero-reorder session, it faults and lets the pipeline recreate:
  the capture loop caches the zero-reorder flag until then.
- NVENC caches registrations, not mappings. Each accepted submission gets its
  own `nvEncMapInputResource` binding, holds the frame lease, and stores an
  internal input token in `NV_ENC_PIC_PARAMS::inputTimeStamp`. The binding is
  unmapped only after `nvEncLockBitstream` succeeds for the output whose
  `outputTimeStamp` matches that token; B-frame reorder means this is not always
  the output submitted with that input. Re-submitting a texture while its
  binding is still live is dropped and re-arms the keyframe instead of sharing
  the mapping. Registration holds its own COM reference so pool destruction
  cannot invalidate the driver registration.
- `resetInputResources()` returns false when EOS, lock, unmap, or unregister
  fails. It never frees a pending mapping or lease on failure; the caller must
  keep the capture frames and producer storage alive. A failed retirement leaves
  the NVENC session faulted and intact. `shutdown()` and a failed reinitialize
  abandon the whole `Impl` for the process lifetime rather than destroying a
  session that may still have driver work. AMF owns copied inputs and returns
  true.
- NVENC's registration map is bounded by the capture and scaler pools that own
  the input textures (a fixed pool size each), not by time. It is emptied by a
  successful reset. `NvencEncoder` logs its size at debug so the real
  steady-state count stays observable from the encode side. Do not read a
  growth there as a leak without checking which pool grew first.
- `IEncoder::inputRetentionBudget()` reports how many pool-backed input
  surfaces the encoder can hold before producing output; NVENC overrides it
  with its bitstream-buffer count (lookahead + B-frame delay + 4), AMF keeps
  the 0 default because it copies inputs and pins nothing. `app` sizes the
  encoder-feeding pool from this so deep lookahead cannot exhaust it.
- Both backends load their runtime dynamically and must never link it:
  `nvEncodeAPI64.dll` and `amfrt64.dll`, both with `LoadLibraryExW` and
  `LOAD_LIBRARY_SEARCH_SYSTEM32`. A load or `GetProcAddress` failure is not an error path: report
  `EncoderCapabilities::detail` with an actionable message and `available = false`.
  The backend also logs that detail itself, because `RecorderController`'s own
  failure line names no reason and the Diagnostics page is not where a bug report
  looks.
- The two backends populate `EncoderCapabilities::h264` / `::hevc` differently.
  NVENC queries them per codec GUID and refuses a session whose requested codec is
  unsupported. AMF asserts both true: `updateCapabilities` only runs after
  `CreateComponent` has already succeeded for the *requested* codec, so the other
  codec is never probed. It is display-only for AMF - the real gate is that
  `CreateComponent` fails with a reason - so validating it means creating and
  discarding a second VCN component on every `updateCapabilities` call, including
  the `switchInputFormat` one. Not worth it; probing is a real capability query.
- `BACKTRACK_HAS_NVENC` and `BACKTRACK_HAS_AMF` guard the SDK includes. Both
  `.cpp` files must compile with the flag off, producing a stub that reports the
  backend unavailable with a reason.
- `EncoderFactory::createEncoderForDevice` keys off `D3DDevice::vendor()`, which
  is derived from the DXGI vendor id: AMD `0x1002` uses AMF, NVIDIA `0x10DE`
  uses NVENC, Intel and unknown default to NVENC.
  `platform/Win32Util.h`'s `dxgiAdapterSupportsHardwareEncode` mirrors that
  vendor list for adapter selection; keep the two in agreement when adding a
  vendor. The Intel/unknown default is a deliberate last resort, not a supported
  path: on those machines NVENC initialization fails and its logged detail is the
  only evidence of why. Overriding the backend per machine is a settings feature
  (`AppSettings` + a UI control), not a factory detail - do not grow the factory
  to carry it.
- Requested settings may exceed hardware limits. Each backend clamps what it
  cannot honor and publishes the result in `capabilities().effective`; the UI
  renders that, not the request. A setting the backend never configures must be
  reported off there rather than echoed back as requested (AMF reports lookahead,
  temporal AQ, multipass, and adaptive I/B-frames off, and B-frames off for HEVC).
  AMF disables pre-analysis before `Init`; pre-encode-assisted rate control is
  not lookahead. Configuration fails if pre-analysis remains active or a
  rejected disable cannot be verified and the hardware capability is not known
  to be absent. AVC B-frame timing follows the read-back pattern; if readback is
  unavailable, it disables B-frames instead.
  AMF also fails `initialize` for an unsupported size or bitrate / GOP /
  framerate that do not read back after `Init`.
- `IEncoder::encoderAvailable()` is polled by the capture loop and must be
  lock-free. Do not implement it in terms of `stats()`. `stats()` and
  `capabilities()` serve a snapshot when the state mutex is held across a
  driver call instead of waiting, so diagnostics never block the encode.
- AMF output VUI is pinned BT.709 (transfer, primaries, matrix, studio range
  in `applyColorProperties`); input conversion stays driver AUTO. Explicit
  input matrix/range pins deep-fried SDR on Adrenalin while the defaults
  encode it exactly, so the driver owns conversion and we only describe the
  stream. Color `setProp` failures stay warning-only: a driver that drops them still
  encodes, and the log names the property.
- Packets come out in decode order, with `pts100ns` in presentation order and
  `dts100ns` valid only with `hasDts`. Both backends set `hasDts` on every
  packet and take DTS from `DecodeOrderTimestamps`, which equals PTS when
  nothing reorders. Never treat a zero or negative timestamp as "absent".
- `DecodeOrderTimestamps.h` is the reordering helper both backends share. Reset
  it on every initialize and component re-creation with the established pipeline
  depth: NVENC uses its B-frame count plus configured lookahead; AMF uses the
  established AVC B-frame pattern and no lookahead. It assigns decode slots from
  submitted timestamps in FIFO order. In-flight backlog and batched harvesting
  may leave more slots queued than the configured depth; occupancy alone is not
  evidence of a lost output. A full drain retains up to the configured depth of
  trailing slots for a continuing session.
- `requestKeyFrame()` arms the next accepted frame, not the current call. Both
  backends also re-arm it whenever a frame is dropped before it reaches the
  encoder, so the stream restarts from a clean reference chain; keep that, or a
  loss leaves a GOP that playback and replay cuts inherit.
- A non-blocking bitstream lock (`doNotWait`) that cannot return the picture
  yet is "not ready", not a fault. Some drivers answer it with
  `NV_ENC_ERR_LOCK_BUSY`, at least one with `NV_ENC_ERR_OUT_OF_MEMORY`, so
  `lockNotReady` classifies both while the lock is non-blocking and only the
  per-output `kBitstreamLockTimeout` deadline escalates. Submission skips the
  poll until the oldest pending output could plausibly be complete
  (`outputShouldBeComplete`); outputs are still matched by timestamp token.
- `failEncoder` is the terminal session fault: publish unavailable and stop
  submitting. `failInputMapping` adds the unknown-binding latch and is reserved
  for a failed unmap/unregister/EOS. A lock-status fault must not take the
  mapping-fault path - the session can still retire its inputs, which is what
  lets `recreateGpuPipeline` release them instead of aborting.
- A wedged or TDR'd session escalates by publishing `encoderAvailable = false`
  and shutting the encoder down. That is the only signal
  `RecorderController`'s pipeline-recreate path uses, so never leave a broken
  session reporting healthy. A terminal AMF output-query failure tears down the
  component and context immediately; callers must stop using both.
- `EncoderCommon` is the single source for what both backends do
  identically: `effectiveSettingsFor` (profile mapping plus the dependent
  lookahead/AQ/B-frame resets; capability gating stays per-backend),
  `finalizePacket` (explicit driver-timestamp validity plus fallback,
  decode-order DTS, the
  IDR-or-bitstream keyframe rule, header patching, parameter-set caching, EMA
  stats), `ensureKeyFrameCarriesHeaders`, and
  the shared escalation budgets (`kEncoderMaxConsecutiveFailures`,
  `kEncoderOutputTimeout`, `kEncoderInFlightMaxAge`). Only the keyframe
  picture-type read stays per-backend (NVENC `pictureType`, AMF
  `OUTPUT_DATA_TYPE`), passed in as `metadataIdr`; so do the drain
  mechanisms (NVENC EOS poll, AMF helper-thread `Drain()`).
- `BitstreamUtils` decides replay cut safety, so it is shared and strict:
  `bitstreamContainsKeyFrame` matches IDR (or HEVC BLA/IDR) only. A plain I
  frame, or an open-GOP CRA frame that a driver reports as an I frame, must not
  be marked as a cut point. `extractParameterSets` exists because a driver can
  ignore header insertion and leave a cut undecodable: both backends cache the
  parameter sets from the first packet that carries them and prepend them to any
  keyframe that lacks them. Both encoders emit Annex B start codes, which is the
  only framing `BitstreamUtils` scans. parameter-set completeness requires
  SPS/PPS for H.264 and VPS/SPS/PPS for HEVC; duplicates and other NAL types do
  not satisfy a missing set. NAL-type masks must cover all 64 HEVC types.
  classification reads the first available NAL header byte, not the payload;
  extraction also returns partial parameter-set groups and preserves their bytes.

## Work Guidance

- Backends are large by nature (vendor API surface). Keep new state inside the
  `Impl` pimpl and keep the public header to the `IEncoder` overrides.
- When a vendor parameter is unavailable on some hardware, degrade the setting
  rather than failing `initialize`, and say which one in `detail`. The AMF
  configuration failures above are the deliberate exceptions.
- Log which adapter, backend, and effective configuration were chosen at
  initialize time. That line is what makes a bad encode diagnosable later.

## Verification

`DecodeOrderTimestampsTests` covers reordered backlog, batched output, drain,
and same-session continuation.
`NvencLifecycleTests` (CTest, needs the SDK header) drives the real encoder
through a fake NVENC API: per-use mapping, B-frame token matching and
out-of-order retirement, repeated input, rejection, reset, lock/unmap failure,
shutdown retention, and the early non-blocking lock statuses (an incomplete
`LOCK_BUSY`/`OUT_OF_MEMORY` lock keeps the encoder available and still
publishes once the picture completes, while a permanently stuck lock escalates
after the deadline without latching the mapping fault). drop-path regressions
cover counters, keyframe rearm, ready tails, unchanged diagnostics, and mapping
retirement; retention-budget cases cover pipeline delay and the buffer-count cap.
`NvencStubTests` (CTest) covers the `BACKTRACK_HAS_NVENC=0` stub and zero retention.
`AmfEncoderTests` (CTest, needs the AMF headers) exercises AMF configuration,
lookahead feature reporting, pre-analysis readback, B-frame timing, and
format-switch failures with fake components and no GPU. requested/effective
separation, complete warning details, component selection, null-component failure,
and caller-owned error logging have focused regressions.
`ReplayBufferTests` covers the `BitstreamUtils` classifiers against hand-built
Annex B NALs: the HEVC closed-GOP range and CRA rejection, every complete/missing
parameter-set combination for each codec, duplicate/irrelevant NALs, short/long
and mixed start codes, truncated/trailing input, and byte-exact extraction through
the final byte of the buffer. Build with
the SDK headers present and absent to exercise both sides of the compile guards,
then confirm the Diagnostics page reports the expected backend and effective
settings.

## Child DOX Index

None. This folder's children are files only.