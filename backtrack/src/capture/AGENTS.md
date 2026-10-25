# capture

## Purpose

Everything that turns a monitor or window into a D3D11 texture: the capture
source interface and its two implementations, the adapter and device, and the
GPU scaler that converts frames to whatever the active encoder wants.

## Ownership

- Owns: `ICaptureSource`, `WgcCaptureSource`, `DesktopDuplicationCapture`,
  `D3DDevice`, `D3D11FrameScaler`, `HdrStability.h`, `GpuTexturePool.h`, `GpuPoolBuilder.h`.
  `ICaptureSource` also carries the HDR diagnostics defaults (`hdrPinned`,
  `hdrBridgedFrames`); only Desktop Duplication overrides them.
- Does not own: encoder input format decisions (the encoder declares its
  preference through `IEncoder::preferredInputFormat`) or the frame queue, which
  belongs to `app`.

## Local Contracts

- `ICaptureSource` is the whole surface `RecorderController` uses. New methods
  need a default implementation; a new backend must be selected in
  `RecorderController::createCaptureSource` and report itself through
  `backend()`.
- Frame format follows the source, not this folder. `WgcCaptureSource` is always
  BGRA: its `Direct3D11CaptureFramePool` is created with
  `B8G8R8A8UIntNormalized`. `DesktopDuplicationCapture` adopts whatever format
  the duplication texture reports and republishes it in `GpuFrame::format`; on
  an advanced-color output that is `R10G10B10A2_UNORM` or
  `R16G16B16A16_FLOAT`.
- `GpuFrame::lease` (`std::shared_ptr<void>`) keeps the source-side surface
  alive to the end of the frame: a WGC zero-copy lease pins the
  `Direct3D11CaptureFrame`, every other path pins the pool `TextureSlot`.
  Dropping it early releases a surface the encoder is still reading.
- Desktop Duplication is per-output and must never guess output 0.
  `initialize` resolves `target.monitor` (or `settings.monitorIndex`) to an
  `IDXGIOutput` on the device's own adapter and fails when the monitor is not
  there. `app` pre-filters the same case against `D3DDevice::adapterIndex()`
  before choosing the backend, because the wrong adapter silently records the
  wrong monitor.
- A source never rebinds or recovers itself. Both implementations latch their
  loss flag - `WgcCaptureSource` on `item.Closed`, a WinRT `hresult_error`, or a
  failed pool recreate, `DesktopDuplicationCapture::markLost` on ACCESS_LOST,
  INVALID_CALL, DEVICE_REMOVED, DEVICE_RESET, `E_INVALIDARG`, or any other
  failed `AcquireNextFrame` - and `acquireNextFrame` then returns false
  immediately. `app` watches `isDeviceLost()` and owns every recovery.
- `GpuFrame::hdr` is set only by `DesktopDuplicationCapture`, and only in a
  pinned HDR session (`HdrSessionPin` in `HdrStability.h`) on the pinned pool
  format (`R10G10B10A2_UNORM` PQ or `R16G16B16A16_FLOAT` scRGB). The pin
  ratchets: the first surface pins HDR, an HDR surface upgrades an SDR session
  once, and nothing downgrades mid-session. The init-time OS poll never
  pre-pins: it stayed HDR for whole sessions whose every surface arrived BGRA,
  and pooling scRGB on the poll alone forces those SDR-range frames through
  the HDR tone-map. An SDR surface arriving in a pinned session is staged and normalized to scRGB-linear
  by the `kBridgeShaderHlsl` fullscreen pass (`srgbChannelToLinear`, the exact
  CPU-mirrored transfer tested in `HdrStabilityTests`), so bridged and native
  frames take the identical tone-map path and one GOP never mixes
  interpretations. The bridge renders into the desktop copy (which carries a
  render-target view for exactly this) before the slot copy and cursor
  composite, so cursor and downstream reads see current pixels either way.
  Every other frame, including every WGC frame, leaves `hdr` false; WGC assigns
  `hdr = false` explicitly on both emit paths so a stale DD `true` can never
  leak across a backend switch.
- `resolutionChangeCount()` advances when a source handles a change without a
  rebind: the WGC `FramePool.Recreate` path on a display-mode change, and
  `DesktopDuplicationCapture` on the one-time SDR->HDR upgrade
  (`applyHdrUpgrade`), a genuine HDR<->HDR (PQ<->scRGB) pool adopt, or a pool
  recreate for a size change. An SDR surface in a pinned session bridges with
  no recreate and no bump, so an oscillation storm costs one shader pass per
  frame, not a 500 MB pool rebuild plus a GOP break per flap. The capture loop
  then requests an encoder keyframe so the jump never lands mid-GOP.
- Cursor capture is per source, not shared. WGC delegates to the session
  (`IsCursorCaptureEnabled`), and `setCursorCaptureEnabled` re-applies it to the
  live session without a rebind. Desktop Duplication composites the pointer
  itself with the inline `kCursorShaderHlsl` quad - normal alpha plus the
  AND/XOR and alpha-XOR paths monochrome and masked-color shapes need - which is
  why its pool slots carry `D3D11_BIND_RENDER_TARGET`; its
  `setCursorCaptureEnabled` only flips a flag the next acquired frame reads.
  The cursor shader's `hdrBypass` keys off the session pin (`hdrPin_.pinnedHdr()`
  plus pool format), which never moves mid-session, so cursor and tone-map
  always agree; it stays 0 on 8-bit targets (legacy quantize +
  0.5-selector paths for monochrome shapes) and 1 on stable-HDR `R10G10B10A2_UNORM` /
  `R16G16B16A16_FLOAT` targets, where blending stays linear so PQ/scRGB
  highlights neither clip nor band.
- `D3DDevice` owns the adapter selection and reports the vendor the encoder
  factory keys off. Its `immediateContextMutex()` must be held by every caller
  issuing immediate-context work; teardown and recreation are serialized
  outside it by `RecorderController::pipelineGpuMutex_`.
- `D3D11FrameScaler` keeps video-processor state sticky (`VpState`) and
  reprograms it only when the configuration actually differs; reissuing the
  `Set*` calls every frame costs driver calls for nothing. Any new processor
  setting must go into `VpState` and `equals`, or the cache silently skips it.
- The scaler's NV12 fallback is sticky for the session and only `reset()`
  clears it: `nv12Unavailable_` drops NV12 output to BGRA after a conversion
  failure. An input-view failure (unknown capture surface) drops the frame
  without latching BGRA; only genuine init/`VideoProcessorBlt` failures under
  NV12 latch. `useCopiedInput_` is per processor generation, cleared in
  `releaseResources()`, so every rebuild retries the direct path before using
  the pooled input copy when `CreateVideoProcessorInputView` rejects the
  capture surface. That copy is created with `BindFlags = 0`: a
  SHADER_RESOURCE flag makes the call fail with `E_INVALIDARG` on strict
  drivers.
- The HDR latch is narrow and self-healing with backoff. Only `E_INVALIDARG` /
  `DXGI_ERROR_UNSUPPORTED` under an applied HDR input space sets
  `hdrInputRejected_` (`isHdrRejectionHr`); any other failure under that mapping
  is transient and retries tone-mapping on the next HDR frame. A latched frame
  retries in-call under the SDR mapping instead of dropping, keeping NV12
  instead of degrading to BGRA. The latch retries tone-mapping on the next HDR
  frame after `hdrRejectionCooldown(hdrRejectStrikes_)` (5 s, 15 s, then 60 s
  capped; a fixed window re-alternates tone-mapped and SDR frames on marginal
  drivers) or immediately when input format/dims change (which also resets the
  count, as does a recovery); latch logs one warning with the next retry in
  seconds, recovery logs info. The legacy path (no `VideoContext1`)
  programs no HDR input space, so `appliedHdrInputSpace_` stays false and
  `hdrMappingActive()` is false; HDR input there logs one warning and records
  without tone-mapping. `inputFormat_` is tracked in `ensureInitialized` /
  `ensureCopyPool`, and the copied input texture is rebuilt when the source
  format changes (PQ<->scRGB reuse the copy path at the same resolution).
  `hdrInputRejected()` / `hdrMappingActive()` expose the latch and the applied
  mapping; both are read by `app` only under `pipelineGpuMutex_`, which also
  guards every mutation.
- Desktop Duplication polls HDR state at most once per second through its cached
  `IDXGIOutput6` (`colorProbe_`, no factory walk; falls back to
  `monitorHasAdvancedColor` when the probe is gone) into a raw poll state that
  is diagnostics-only. Surfaces decide: the first one pins the session
  through `HdrSessionPin::begin`, a later HDR surface upgrades it once
  through `noteSurfaceFormat`. `hdrActive()` exposes the pin and
  `hdrChangeCount()` its upgrades; `hdrBridgedFrames()` counts bridged
  surfaces. The pool stays empty until the first acquired surface reveals its
  real format, so an HDR session never pays a BGRA recreate to get there; an
  SDR-first session pools BGRA and upgrades once if HDR arrives.
- The scaler prunes cached input views whose texture is otherwise unreferenced.
  Views pin their texture, so a missed prune holds GPU memory.
- Textures come from fixed `GpuTexturePool` pools sized by
  `texturePoolSizeForQueueLimit` (`frameQueueLimit + 4`, clamped 5..16; the
  sanitize 1..8 limit makes the live range 5..12), headroom over the queue
  limit because the capture loop pins one slot as `lastFrame`, and `app` feeds
  the same value to the scaler through `setPoolSize`. `GpuPoolBuilder` is the
  single clamp and texture factory for all four pool builds (owners keep only
  views and sidecars); its comment owns the bind-flag matrix. Pool exhaustion is a drop,
  counted as such, never a blocking wait on the capture thread; every pool logs
  through `acquireOrLog` (1st + every 300th). WGC zero-copy
  has a second cap, `wgcMaxZeroCopyLeases` (`framePoolSize - 1`, the frame pool
  itself is `wgcFramePoolSizeForQueueLimit`: `frameQueueLimit + 2`, 3..8);
  hitting it drops that frame onto the copy path instead of holding the
  `Direct3D11CaptureFrame`.
- Every pool teardown invalidates any GPU registration the consumer still holds
  for those textures, so `releaseResources()` bumps `poolGeneration()` and the
  caller must drop its registrations when that value changes across a call. The
  scaler reports it; `app` reacts. It never calls the encoder itself.

## Work Guidance

- Texture handling stays on the GPU. A `Map` on a capture or scaler path is a
  bug unless it is the deliberate readback of encoded bytes.
- When adding a capture mode, check the exclusive-fullscreen path: WGC
  `CreateForWindow` against an exclusive surface yields zero frames, which is
  what `createCaptureSource(target, exclusiveRecovery = true)` exists for. That
  flag keeps the target on the monitor and skips WGC entirely.
- Hot-path failures are logged through the logger's own rate limiting, and the
  existing sources deliberately carry local suppression on top of it: drop
  counters log on their first occurrence and every 300th, and
  `DesktopDuplicationCapture::logAcquireFailureOnce` adds a 2 s cooldown keyed
  on the HRESULT plus a count of the repeats it swallowed. Do not add another
  variant without saying why.

## Verification

`HdrStabilityTests` (CTest) covers the pure logic in `HdrStability.h`: the
session-pin ratchet, the sRGB bridge transfer, the rejection backoff, and the
format predicate. Everything GPU-bound (pool pinning, bridge pass, cursor
bypass) is verified by building Release and confirming the Diagnostics page
reports the expected selected and active capture backend, the HDR line
(active / rejected / SDR), capture size, dropped
frames, and cursor-only desktop frames on a machine with a hardware encoder.
Manual HDR check (needs an HDR monitor with the BGRA<->scRGB oscillation, no
suite can drive a duplication surface): record the desktop, then confirm the
HDR line reads `active (tone-mapping, pinned, ..., flips 0)` with a growing
`bridged` count while oscillating, no per-second `HDR state changed` or
`Capture surface changed` lines in the log, and no brightness flicker in the
saved clip. A uniformly deep-fried pinned clip (crushed mids, boosted
saturation for the whole duration) means SDR-range frames reached the HDR
tone-map: expect `pool=BGRA` SDR sessions in the log instead, and scRGB/R10
pools only after a genuine HDR surface (one `Capture surface changed` line
plus one GOP restart). Discriminator for a mixed-tenure session: record a
static gray ramp with OS HDR on (pin build) and with OS HDR off (SDR path),
then compare mid-gray Y; equal native/bridged tenures clear the bridge
transfer and indict the driver's G10 tone-map on SDR-range input.

## Child DOX Index

None. This folder's children are files only.
