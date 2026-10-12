# capture

## Purpose

Everything that turns a monitor or window into a D3D11 texture: the capture
source interface and its two implementations, the adapter and device, and the
GPU scaler that converts frames to whatever the active encoder wants.

## Ownership

- Owns: `ICaptureSource`, `WgcCaptureSource`, `DesktopDuplicationCapture`,
  `D3DDevice`, `D3D11FrameScaler`.
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
- `GpuFrame::hdr` is set only by `DesktopDuplicationCapture`, and only while
  `monitorHasAdvancedColor` is true and the surface is `R10G10B10A2_UNORM` (PQ)
  or `R16G16B16A16_FLOAT` (scRGB). The scaler declares that input color space so
  the video processor tone-maps to the SDR output. Every other frame, including
  every WGC frame, leaves it false.
- `resolutionChangeCount()` advances when a source handles a change without a
  rebind: the WGC `FramePool.Recreate` path on a display-mode change, and
  `DesktopDuplicationCapture` on an HDR flip (`applyHdrState`) or a pool
  recreate (size/format switch). The capture loop then requests an encoder
  keyframe so the jump never lands mid-GOP.
- Cursor capture is per source, not shared. WGC delegates to the session
  (`IsCursorCaptureEnabled`), and `setCursorCaptureEnabled` re-applies it to the
  live session without a rebind. Desktop Duplication composites the pointer
  itself with the inline `kCursorShaderHlsl` quad - normal alpha plus the
  AND/XOR and alpha-XOR paths monochrome and masked-color shapes need - which is
  why its pool slots carry `D3D11_BIND_RENDER_TARGET`; its
  `setCursorCaptureEnabled` only flips a flag the next acquired frame reads.
  The cursor shader's `hdrBypass` stays 0 on 8-bit targets (legacy quantize +
  0.5-selector paths for monochrome shapes) and 1 on `R10G10B10A2_UNORM` /
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
- The scaler's fallbacks are sticky for the session, not per frame, and only
  `reset()` clears them: `nv12Unavailable_` drops NV12 output to BGRA after a
  conversion failure, `hdrInputRejected_` pins a refused HDR input back to the
  SDR interpretation, and `useCopiedInput_` switches to a pooled input copy when
  `CreateVideoProcessorInputView` rejects the capture surface. That copy is
  created with `BindFlags = 0`: a SHADER_RESOURCE flag makes the call fail with
  `E_INVALIDARG` on strict drivers.
- The HDR latch is narrow and self-healing. Only `E_INVALIDARG` /
  `DXGI_ERROR_UNSUPPORTED` under an applied HDR input space sets
  `hdrInputRejected_` (`isHdrRejectionHr`); any other failure under that mapping
  is transient and retries tone-mapping on the next HDR frame. A latched frame
  retries in-call under the SDR mapping instead of dropping, keeping NV12
  instead of degrading to BGRA. `inputFormat_` is tracked in
  `ensureInitialized` / `ensureCopyPool`, and the copied input texture is
  rebuilt when the source format changes (PQ<->scRGB reuse the copy path at the
  same resolution). `hdrInputRejected()` / `hdrMappingActive()` expose the latch
  and the applied mapping; both are read by `app` only under
  `pipelineGpuMutex_`, which also guards every mutation.
- Desktop Duplication polls HDR state at most once per second through its cached
  `IDXGIOutput6` (`colorProbe_`, no factory walk; falls back to
  `monitorHasAdvancedColor` when the probe is gone). `hdrActive()` /
  `hdrChangeCount()` expose the state; the pool stays empty until the first
  acquired surface reveals its real format, so an HDR session never pays a BGRA
  recreate to get there.
- The scaler prunes cached input views whose texture is otherwise unreferenced.
  Views pin their texture, so a missed prune holds GPU memory.
- Textures come from fixed pools: both capture pools are
  `clamp(settings.gpu.frameQueueLimit + 4, 5, 16)`, headroom over the queue
  limit because the capture loop pins one slot as `lastFrame`, and `app` feeds
  the same value to the scaler through `setPoolSize`. Pool exhaustion is a drop,
  counted as such, never a blocking wait on the capture thread. WGC zero-copy
  has a second cap, `framePoolSize - 1` in-flight leases; hitting it drops that
  frame onto the copy path instead of holding the `Direct3D11CaptureFrame`.
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

No dedicated test suite. Build Release, then confirm the Diagnostics page
reports the expected selected and active capture backend, the HDR line
(active / rejected / SDR), capture size, dropped
frames, and cursor-only desktop frames on a machine with a hardware encoder.

## Child DOX Index

None. This folder's children are files only.
