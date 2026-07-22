# AI GENERATED DOC
## GPU Optimization Notes

Backtrack protects game frame time before minimizing an isolated GPU-engine percentage. NVIDIA Video Encode is fixed-function hardware, so its utilization is not directly comparable to 3D utilization. An optimization is accepted only when total relevant GPU work or game frame time improves without changing active-content quality, timing, or reliability.

## Balanced 1080p60 H.264 Profile

The Balanced path keeps its existing encoder behavior:

- NVENC P3 preset with low-latency tuning.
- H.264 at the configured resolution, frame rate, bitrate, GOP, and rate-control settings.
- B-frames, lookahead, adaptive quantization, and multipass disabled.
- Zero reorder delay and two reference frames.

The scheduling and input-format optimizations do not lower resolution, target timeline rate, bitrate, GOP length, or quality settings for changing content.

## Lossless Idle Coalescing

Windows Graphics Capture can produce no new image while the 60 Hz output timeline continues. When lossless idle coalescing is enabled and the encoder is operating with zero reorder delay, Backtrack extends the duration of the last encoded sample instead of submitting the same texture to NVENC again.

- Only scheduler-generated duplicates are coalesced; a genuinely new captured image still receives a normal NVENC submission.
- Scheduler stalls collapse up to four elapsed duplicate intervals into one equivalent held-frame interval instead of creating a burst of identical submissions.
- The MP4 muxer and replay buffer receive synchronized, absolute tail-duration updates. Replay snapshots remain immutable through copy-on-write replacement.
- Forced heartbeats cover recording startup and replay keyframe requests. A periodic IDR heartbeat at the configured GOP interval preserves keyframe availability during long static periods.
- Coalesced intervals are timeline intervals, not dropped frames. Diagnostics report source frames, cadence duplicates, catch-up duplicates, coalesced intervals, queue depth, NVENC submissions, encoded frames, and keyframes separately.
- Coalescing automatically remains inactive when zero reorder delay is unavailable because extending an already published tail is not safe with reordered output.

This produces variable-duration video samples while preserving the configured 60 Hz timeline and wall-clock A/V duration. It does not synthesize motion or discard distinct source images.

## BGRA-to-NV12 Input Path

The experimental D3D11 video-processor path can request pooled NV12 output before NVENC registration. It explicitly describes full-range BT.709 RGB input and studio-range BT.709 YCbCr output. NVENC registers textures using their validated D3D11 format rather than assuming BGRA.

The path is capability gated and has a sticky per-session BGRA fallback. If NV12 output is unsupported or video-processor initialization, input-view creation, or conversion fails, Backtrack retries the same frame as BGRA and avoids repeatedly attempting the failed NV12 path.

On the target GTX 1060 with NVIDIA driver 582.28, NV12 output allocation and processor initialization succeeded, but video-processor input-view creation returned `E_INVALIDARG` for both the direct WGC texture and a reusable copied BGRA texture. The fallback restored BGRA encoding, but this is not a valid NV12 benchmark data point. Shipping capture therefore prefers BGRA; NV12 remains experimental code rather than the default.

NV12 can move work from Video Encode or 3D/copy to Video Processing. A lower Video Encode percentage alone is therefore not proof of an improvement. It must not become preferred unless a supported target passes the A/B procedure in [profiling.md](profiling.md) with equal output and lower total cost or better game frame time.

## User-Facing Optimization Settings

- **Lossless idle coalescing:** avoids redundant idle and catch-up submissions while preserving sample duration and heartbeat keyframes. It defaults on under the new `gpu.losslessIdleCoalescing` persistence key; the obsolete forced-off setting is intentionally not imported.
- **Adaptive GPU:** Disabled, Conservative, or Aggressive. Unlike lossless coalescing, protection modes can deliberately drop work under saturation to protect gameplay and can affect smoothness.
- **WGC zero-copy:** allows the native WGC BGRA frame-pool texture to continue without an intermediate capture copy when no conversion is required. Scaling or experimental format conversion may use a separate pooled output texture.
- **Frame queue:** bounds queued textures before NVENC. Duration-only updates use a synchronized side channel and do not consume the single texture slot when queue depth is one.
- **Keyframe seconds:** controls normal GOP spacing and the maximum static heartbeat interval.

## Replay and Recording Behavior

Replay stores encoded video and audio packets, not raw D3D11 textures. Extending a coalesced video tail therefore changes packet metadata rather than retaining another GPU resource. Replay saves and recording muxing preserve variable sample durations, and recording stop waits for the video timeline end so a final held frame is not truncated.

Saving a replay or finalizing a recording uses the native Windows MP4 path and does not re-encode video. Existing recovery manifests continue to reference the encoded elementary stream and timeline metadata.

## Remaining Costs and Validation Status

- Desktop Duplication still requires a GPU copy so the acquired DXGI frame can be released promptly.
- Experimental NV12 output requires one D3D11 video-processor conversion pass; custom resolution also requires scaling.
- Dynamic 60 FPS content still requires approximately one NVENC submission per distinct output image.
- Periodic and forced heartbeat frames intentionally submit during static periods.
- Queue pressure, capture resets, monitor switching, device loss, recording/replay lifecycle, timestamps, A/V sync, and recovery behavior require runtime regression testing.
- Stop-time logs include the scheduling, submission, encoding, byte, and drop counters needed for non-interactive A/B runs.

The Release x64 implementation and scheduler tests pass. Target runtime testing rejected NV12 as the default because input-view creation failed and exercised the safe BGRA fallback. Valid UTF-8-configured BGRA coalescing on/off measurements are still required, so the observed 26% Video Encode utilization must not be described as resolved until the profiling and output-validation matrix is completed.
