# capture

## Purpose

Everything that turns a monitor or window into a D3D11 texture: the capture
source interface and its two implementations, the adapter and device, and the
GPU scaler that converts frames to whatever the active encoder wants.

## Ownership

- Owns: `ICaptureSource`, `WgcCaptureSource`, `DesktopDuplicationCapture`,
  `D3DDevice`, `D3D11FrameScaler`, `HdrStability.h`, `GpuTexturePool.h`,
  `GpuPoolBuilder.h`, `WgcCopyPool.h`.
- Does not own: encoder input format decisions (the encoder declares its
  preference through `IEncoder::preferredInputFormat`) or the frame queue, which
  belongs to `app`.

## Local Contracts

- `ICaptureSource` is the whole surface `RecorderController` uses. New methods
  need a default implementation; a new backend must be selected in
  `RecorderController::createCaptureSource` and report itself through
  `backend()`.
- `WgcCaptureSource` uses BGRA. desktop duplication requests
  `R16G16B16A16_FLOAT` and `B8G8R8A8_UNORM` through `DuplicateOutput1`;
  `E_NOINTERFACE`, `E_NOTIMPL`, `E_INVALIDARG`, and `DXGI_ERROR_UNSUPPORTED`
  fall back to legacy BGRA. transient errors retry the same API. other returned
  formats are rejected as source loss. R10 is not negotiated: its format alone
  does not establish PQ.
- `GpuFrame::lease` (`std::shared_ptr<void>`) keeps the source-side surface
  alive to the end of the frame: a WGC zero-copy lease pins the
  `Direct3D11CaptureFrame`, every other path pins the pool `TextureSlot`.
  Dropping it early releases a surface the encoder is still reading. `app`
  releases capture-held frames only after the encoder reset acknowledges every
  mapping; a failed reset keeps them alive. WGC shutdown waits 2 s for
  outstanding leases and then proceeds anyway, so a wedged encoder can still
  make session close block; that is a last-resort hang risk, not a safe
  retirement barrier.
- Desktop Duplication is per-output and must never guess output 0.
  `initialize` resolves `target.monitor` (or `settings.monitorIndex`) to an
  `IDXGIOutput` on the device's own adapter and fails when the monitor is not
  there. `app` pre-filters the same case against `D3DDevice::adapterIndex()`
  before choosing the backend, because the wrong adapter silently records the
  wrong monitor.
- Desktop Duplication returns the image unrotated and reports the display
  rotation through `DXGI_OUTDUPL_DESC::Rotation`; `ModeDesc` carries the
  rotated (physical) mode dimensions. `DesktopDuplicationCapture` queries it
  at `initialize`, stores `rotation_`, and normalizes `width_`/`height_` to
  the unrotated texture dimensions (swap for 90/270). The cursor position
  DXGI reports is in physical (rotated) coordinates, so `compositeCursor`
  maps it to the texture space through the header-inline
  `unrotatedDimensions`/`transformCursorPosition` helpers (the cursor shape
  itself stays upright; the saved output is in the unrotated orientation).
  `rotation_` resets to `UNSPECIFIED` on loss and shutdown.
- A source never rebinds or recovers itself. Both implementations latch their
  loss flag - `WgcCaptureSource` on `item.Closed`, a WinRT `hresult_error`, or a
  failed pool recreate, `DesktopDuplicationCapture::markLost` on ACCESS_LOST,
  INVALID_CALL, DEVICE_REMOVED, DEVICE_RESET, `E_INVALIDARG`, or any other
  failed `AcquireNextFrame` - and `acquireNextFrame` then returns false
  immediately. `app` watches `isDeviceLost()` and owns every recovery.
- desktop duplication sets `GpuFrame::hdr` for FP16 scRGB surfaces only;
  BGRA and WGC frames remain SDR. the flag selects the scaler's linear-input
  mapping, not proof that an FP16 surface contains HDR highlights. the output
  color poll does not label pixels. a BGRA/scRGB transition adopts the arriving
  format instead of bridging: the old bridge assumed an unverified SDR white
  level and could not reconstruct clipped highlights.
- `resolutionChangeCount()` advances on an in-place size or format change.
  desktop duplication recreates its pool on each BGRA/scRGB transition, so
  repeated transitions can cause visible brightness changes and pool churn.
  the capture loop requests a keyframe on each reported change. WGC reports
  the change before attempting `FramePool.Recreate`, allowing the controller to
  drain old frame leases and encoder registrations first. `WgcCopyPool` rebuilds
  lazily when the fallback texture size differs from the new capture size.
- Cursor capture is per source, not shared. WGC delegates to the session
  (`IsCursorCaptureEnabled`), and `setCursorCaptureEnabled` re-applies it to the
  live session without a rebind. Desktop Duplication composites the pointer
  itself with the inline `kCursorShaderHlsl` quad - normal alpha plus the
  AND/XOR and alpha-XOR paths monochrome and masked-color shapes need - which is
  why its pool slots carry `D3D11_BIND_RENDER_TARGET`; its
  `setCursorCaptureEnabled` only flips a flag the next acquired frame reads.
  `hdrBypass` avoids 8-bit quantization on scRGB targets. HDR cursor color,
  white level, and XOR approximations still need hardware validation.
  retain the cursor constant-buffer fields until layout and rotated/HDR
  hardware evidence supports a change. `UpdateSubresource` reads all 32 bytes;
  removing `desktopSize` alone leaves an unsafe 24-byte C++ source object.
  `desktopCopy_` is written only when a cursor is actually composited; otherwise
  the acquired surface is copied straight into the slot and `desktopCopyDirty_`
  marks the intermediate stale, so a cursor that reappears refreshes it before
  compositing. Both copies must run while the frame is held (before
  `releaseHeldFrame`), because DXGI invalidates the acquired surface on release.
  DXGI reports the pointer shape only when it changes, so the CPU-side
  shape/position/visibility is carried across a rebuild on the same output
  (`DesktopDuplicationCursorCarry`) instead of blanking the cursor until the
  next pointer update; a different monitor or a loss (`markLost`) clears the
  carry so a stale cursor is never composited.
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
  programs no HDR input space, so `appliedHdrInputSpace_` stays false;
  HDR input there logs one warning and records without tone-mapping.
  `inputFormat_` is tracked in `ensureInitialized` /
  `ensureCopyPool`, and the copied input texture is rebuilt when the source
  format changes (PQ<->scRGB reuse the copy path at the same resolution).
  `hdrInputRejected()` exposes the latch; `app` reads it only under
  `pipelineGpuMutex_`, which also guards every mutation.
- desktop duplication polls output color state through cached `IDXGIOutput6`
  at most once per second, with `monitorHasAdvancedColor` as fallback. it is
  not used to infer the surface format. the pool stays empty until the first
  acquired surface; later format changes use the same pool recreation path as
  size changes.
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
  `Direct3D11CaptureFrame`. Lease objects are recycled from a pool of exactly
  `wgcMaxZeroCopyLeases` blocks, so the capture thread does not allocate per
  frame; the deleter clears the pinned frame before the block returns to the
  freelist, so a lease handed to the encoder is never reused in flight.
- The scaler's output pool is the encoder-feeding pool and is sized separately:
  `encoderFeedingPoolSizeFor(queueLimit, encoderRetention)` (`queueLimit +
  retention + 1`, clamped 5..64) from the encoder's `inputRetentionBudget`,
  so a deep NVENC lookahead session that retains accepted inputs before
  producing output cannot exhaust it and drop frames. Only this feeding pool
  grows; the queue-derived capture and source pools are intermediate and stay
  capped at 16. `app` re-negotiates it after encoder initialize and on every
  handoff.
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

`HdrStabilityTests` (CTest) exercises production duplication negotiation with
fake API calls, format acceptance, retries, HDR rejection backoff, and the
header-only advanced-color predicate shared with `platform`.
`DesktopDuplicationRotationTests` (CTest) exercises the header-inline rotation
coordinate helpers (dimension normalization and cursor transform for 90/180/270)
and the cursor carry across a same-output rebuild, without a D3D device. `WgcCopyPoolTests` (CTest) exercises fallback resize with WARP.
`D3D11FrameScalerTests` (CTest) uses WARP and a fake video device to check the
copied-input fallback, NV12 failure handling, and same-size alias/copy paths
including sticky BGRA fallback.

on HDR hardware, compare recorded gray and highlight ramps with the displayed
image and an SDR reference, including multiple SDR white levels, fullscreen
content, cursor shapes, and BGRA/scRGB transitions. verify SDR capture and
recovery after display changes as well. the fake does not validate driver color
conversion, tone mapping, cursor appearance, or actual HDR luminance.

on rotated hardware, verify the saved output is in the unrotated orientation
with correct dimensions and that the cursor tracks content in all four
orientations; the fake does not validate the OS-provided rotation value, the
actual texture orientation, or cursor appearance.

## Child DOX Index

None. This folder's children are files only.
