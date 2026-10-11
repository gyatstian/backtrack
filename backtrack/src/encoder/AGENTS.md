# encoder

## Purpose

Hardware video encoding: the backend-neutral interface, the NVIDIA NVENC and
AMD AMF implementations, the vendor factory, and the bitstream helpers they
share.

## Ownership

- Owns: `IEncoder`, `NvencEncoder`, `AmfEncoder`, `EncoderFactory`,
  `BitstreamUtils`, `DecodeOrderTimestamps.h`.
- Does not own: frame timing (that is `app/VideoTimelineScheduler.h`) or muxing
  (`mux`).

## Local Contracts

- `IEncoder` is the only surface `RecorderController` touches. Add virtuals with
  a default body rather than forcing every backend and call site to change.
- Both backends load their runtime dynamically and must never link it:
  `nvEncodeAPI64.dll` (System32 search only) and `amfrt64.dll`. A load or
  `GetProcAddress` failure is not an error path: report
  `EncoderCapabilities::detail` with an actionable message and
  `available = false`.
- `BACKTRACK_HAS_NVENC` and `BACKTRACK_HAS_AMF` guard the SDK includes. Both
  `.cpp` files must compile with the flag off, producing a stub that reports the
  backend unavailable.
- `EncoderFactory::createEncoderForDevice` keys off the DXGI vendor id: AMD
  `0x1002` uses AMF, NVIDIA uses NVENC, anything else defaults to NVENC.
  `platform/Win32Util.h` mirrors that vendor list for adapter selection; keep
  the two in agreement when adding a vendor.
- Requested settings may exceed hardware limits. Each backend clamps what it
  cannot honor and publishes the result in `capabilities().effective`; the UI
  renders that, not the request.
- `IEncoder::encoderAvailable()` is polled by the capture loop and must be
  lock-free. Do not implement it in terms of `stats()`.
- Packets are produced in decode order: `pts100ns` in presentation order,
  `dts100ns` valid only with `hasDts`. Never treat a zero or negative timestamp
  as "absent".
- `BitstreamUtils` decides replay cut safety, so it is shared and strict:
  `bitstreamContainsKeyFrame` matches IDR (or HEVC BLA/IDR) only. A plain I
  frame, or an open-GOP CRA frame that NVENC reports as an I frame, must not be
  marked as a cut point. `extractParameterSets` exists because a driver can
  ignore header insertion and leave a cut undecodable.
- `DecodeOrderTimestamps.h` holds the reordering helper both backends share.

## Work Guidance

- Backends are large by nature (vendor API surface). Keep new state inside the
  `Impl` pimpl and keep the public header to the `IEncoder` overrides.
- When a vendor parameter is unavailable on some hardware, degrade the setting
  rather than failing `initialize`, and say which one in `detail`.
- Log which adapter, backend, and effective configuration were chosen at
  initialize time. That line is what makes a bad encode diagnosable later.

## Verification

No dedicated suite. Build with the SDK headers present and absent to exercise
both sides of the compile guards, then confirm the Diagnostics page reports the
expected backend and effective settings.

## Child DOX Index

None. This folder's children are files only.