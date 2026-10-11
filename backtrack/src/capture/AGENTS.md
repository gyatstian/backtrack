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
- Every implementation outputs BGRA `ID3D11Texture2D` frames with a
  `GpuFrame::lease` that keeps the source-side surface alive until the encoder
  is done with it. Dropping the lease early releases a texture the encoder is
  still reading.
- `GpuFrame::hdr` marks PQ (ST.2084) content. The scaler tone-maps those frames
  to SDR; 8-bit sources leave it false and take the fast path.
- `resolutionChangeCount()` advances when a source handles a resolution change
  without a rebind (the WGC `FramePool.Recreate` path). The capture loop then
  requests an encoder keyframe so a size jump never lands mid-GOP.
- `D3DDevice` owns the adapter selection and reports the vendor the encoder
  factory keys off. Its `immediateContextMutex()` must be held by every caller
  issuing immediate-context work; teardown and recreation are serialized
  outside it by `RecorderController::pipelineGpuMutex_`.
- `D3D11FrameScaler` keeps video-processor state sticky (`VpState`) and reprograms
  it only when the configuration actually differs; reissuing the `Set*` calls
  every frame costs driver calls for nothing. Any new processor setting must go
  into `VpState` and `equals`, or the cache silently skips it.
- The scaler prunes cached input views whose texture is otherwise unreferenced.
  Views pin their texture, so a missed prune holds GPU memory.
- Textures come from fixed pools sized from `settings.gpu.frameQueueLimit`.
  Pool exhaustion is a drop, counted as such, never a blocking wait on the
  capture thread.

## Work Guidance

- Texture handling stays on the GPU. A `Map` on a capture or scaler path is a
  bug unless it is the deliberate readback of encoded bytes.
- When adding a capture mode, check the exclusive-fullscreen path: WGC on a
  window can lose content that Desktop Duplication still sees, and
  `createCaptureSource(exclusiveRecovery)` exists for exactly that case.
- Rate-limit per-frame logging through the logger's own mechanism rather than
  adding new suppression state; if a new failure needs its own suppression, say
  why in a comment.

## Verification

No dedicated test suite. Build both configurations and confirm the Diagnostics
page reports the expected capture backend, dimensions, and drop counters on a
machine with a hardware encoder.

## Child DOX Index

None. This folder's children are files only.