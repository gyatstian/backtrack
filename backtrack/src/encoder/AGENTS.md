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
  includes a backend header. Add virtuals with a default body rather than
  forcing every backend and call site to change.
- Codecs are H.264 and HEVC only (`core/Types.h` `VideoCodec`), both configured
  8-bit. NVENC maps the enum onto `NV_ENC_CODEC_H264_GUID` /
  `NV_ENC_CODEC_HEVC_GUID`, AMF onto the `AMFVideoEncoderVCE_AVC` /
  `AMFVideoEncoder_HEVC` components.
- `preferredInputFormat()` is what the capture scaler converts to, and the two
  backends deliberately differ: NVENC registers BGRA directly, AMF takes NV12 so
  the fixed-function video processor does the conversion instead of an AMF
  compute pass. AMF re-creates its component through `switchInputFormat` if the
  scaler falls back to BGRA mid-session.
- `resetInputResources()` drops every NVENC input registration, and NVENC
  requires a resource to stay registered until the encoder finished using it, so
  that path flushes the reorder pipeline with an EOS picture and harvests before
  unregistering. EOS is documented as repeatable within a session, so the next
  `encodeFrame` continues on the same session, and the flushed packets stay in
  `readyPackets` for it or for `drain()`. Never put a bare
  `unregisterAllResources()` back on that path: queued pictures still point at
  those textures. AMF wraps each input in a fresh `AMFSurface` and caches no
  registration, so its `resetInputResources()` stays empty. AMF's in-flight entry
  holds the `AMFSurface` and the capture `lease` only: the surface owns the
  texture reference, so a second one there is redundant, not a second lifetime.
- NVENC's registration map is bounded by the capture and scaler pools that own
  the input textures (a fixed pool size each), not by time; it is emptied on
  every pool-generation bump. `NvencEncoder` logs its size at debug so the real
  steady-state count stays observable from the encode side. Do not read a
  growth there as a leak without checking which pool grew first.
- Both backends load their runtime dynamically and must never link it:
  `nvEncodeAPI64.dll` (`LoadLibraryExW` with `LOAD_LIBRARY_SEARCH_SYSTEM32`, so
  System32 only) and `amfrt64.dll` (`LoadLibraryW` on `AMF_DLL_NAME`, default
  search order). A load or `GetProcAddress` failure is not an error path: report
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
  reported off there rather than echoed back as requested (AMF reports temporal
  AQ, multipass, and adaptive I/B-frames off, and B-frames off for HEVC).
  Two things deliberately fail `initialize` instead of degrading: an encode size
  outside the AMF input caps, and an AMF session whose bitrate / GOP / framerate
  do not read back after `Init`, because a silently half-configured session is
  worse than none.
- `IEncoder::encoderAvailable()` is polled by the capture loop and must be
  lock-free. Do not implement it in terms of `stats()`.
- Packets come out in decode order, with `pts100ns` in presentation order and
  `dts100ns` valid only with `hasDts`. Both backends set `hasDts` on every
  packet and take DTS from `DecodeOrderTimestamps`, which equals PTS when
  nothing reorders. Never treat a zero or negative timestamp as "absent".
- `DecodeOrderTimestamps.h` is the reordering helper both backends share. Reset
  it with the backend's full pipeline depth (`bFrames ? 2 : 0` plus
  `lookahead ? lookaheadDepth : 0`) on every initialize and component
  re-creation. A smaller value makes every steady-state output look like a
  swallowed frame and trips the desync watchdog, which then shifts every later
  DTS by whole frame durations.
- `requestKeyFrame()` arms the next accepted frame, not the current call. Both
  backends also re-arm it whenever a frame is dropped before it reaches the
  encoder, so the stream restarts from a clean reference chain; keep that, or a
  loss leaves a GOP that playback and replay cuts inherit.
- A wedged or TDR'd session escalates by publishing `encoderAvailable = false`
  and shutting the encoder down. That is the only signal
  `RecorderController`'s pipeline-recreate path uses, so never leave a broken
  session reporting healthy.
- `EncoderCommon` is the single source for what both backends do
  identically: `effectiveSettingsFor` (profile mapping plus the dependent
  lookahead/AQ/B-frame resets; capability gating stays per-backend),
  `finalizePacket` (driver-timestamp fallback, decode-order DTS, the
  IDR-or-bitstream keyframe rule, header patching, parameter-set caching, EMA
  stats), `ensureKeyFrameCarriesHeaders`, `effectiveDts` for consumers, and
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
  only framing `BitstreamUtils` scans.

## Work Guidance

- Backends are large by nature (vendor API surface). Keep new state inside the
  `Impl` pimpl and keep the public header to the `IEncoder` overrides.
- When a vendor parameter is unavailable on some hardware, degrade the setting
  rather than failing `initialize`, and say which one in `detail`. The two
  `initialize` failures above are the deliberate exceptions.
- Log which adapter, backend, and effective configuration were chosen at
  initialize time. That line is what makes a bad encode diagnosable later.

## Verification

`ReplayBufferTests` covers the `BitstreamUtils` classifiers against hand-built
Annex B NALs, including that `extractParameterSets` copies a NAL running to
the final byte of the buffer whole (its end scan needs a start code to stop at,
so a short copy here is a cached PPS stripped of its trailing bits). Build with
the SDK headers present and absent to exercise both sides of the compile guards,
then confirm the Diagnostics page reports the expected backend and effective
settings.

## Child DOX Index

None. This folder's children are files only.