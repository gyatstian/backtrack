# AI GENERATED DOC
## Profiling Capture and GPU Usage

## Build for Measurement

Use the same Release x64 binary for comparable runs:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' '.\backtrack\backtrack.vcxproj' /t:Build /p:Configuration=Release /p:Platform=x64 /m /v:minimal
```

Confirm the diagnostics page reports NVENC available. Record the NVIDIA driver version, Windows build, monitor refresh rate, capture target, WGC zero-copy state, queue depth, resolution, frame rate, codec, profile, bitrate, and GOP. Do not compare runs if any of these changed unintentionally.

## GTX 1060 1080p60 A/B Matrix

Measure at least 60 seconds per run after a warm-up period. Repeat each run three times and report the median. Use H.264, 1920x1080 at 60 FPS, Balanced, Windows Graphics Capture, and queue depth one.

Run the shipping BGRA combinations first to isolate lossless idle coalescing:

| Content | Idle coalescing | Encoder input | Purpose |
| --- | --- | --- | --- |
| Static desktop | Off | BGRA | Original fixed-submission baseline |
| Static desktop | On | BGRA | Scheduling/coalescing effect |
| Active gameplay | Off | BGRA | Active-content baseline |
| Active gameplay | On | BGRA | Confirms no regression when images change |

Shipping capture prefers BGRA. The experimental capability-gated NV12 path has no user-facing format switch. On the target GTX 1060 with NVIDIA driver 582.28, video-processor input-view creation returned `E_INVALIDARG` for direct and copied BGRA inputs, so the run fell back to BGRA and cannot be reported as NV12. Only add NV12 rows on hardware and drivers where the log proves that NV12 initialization, conversion, and NVENC submission remained active for the complete run.

Benchmark settings files must be UTF-8 without a BOM. `SettingsStore` parses UTF-8; a UTF-16 temporary file is invalid and may silently replace the intended A/B configuration with defaults.

For every run record:

- Average and peak GPU **Video Encode**, **Video Processing**, **3D**, and copy-engine utilization.
- Game average FPS, 1% low, representative GPU frame time, and visible stutter.
- Diagnostics: timeline intervals, source frames, cadence duplicates, catch-up duplicates, coalesced idle intervals, frame-queue depth, NVENC submissions, encoded frames, keyframes, all drop counters, and encoder failures.
- Output bytes and average bitrate over the same wall-clock duration.
- CPU usage and capture/encode thread behavior.

Task Manager is useful for a quick indication but is not sufficient for the final decision. Its engine percentages are sampled and cannot be added as if they represented interchangeable hardware. Use PresentMon or an equivalent repeatable frame-time capture for gameplay, and use Windows Performance Recorder/WPA or GPUView to inspect GPU-engine scheduling.

## Expected Counter Relationships

During a stable static period with coalescing enabled:

- Timeline intervals continue near 60 per second.
- Source frames may be much lower when WGC reports no changed image.
- Coalesced idle intervals rise instead of NVENC submissions.
- NVENC submissions still occur for forced and GOP heartbeat frames.
- Encoded frames track successful submissions, not timeline intervals.
- Drop counters remain unchanged; coalescing is not a drop.

During changing gameplay, source frames and NVENC submissions should remain close to the configured cadence. Catch-up intervals may rise after a scheduler stall, but a burst of identical catch-up submissions should not occur. Queue depth one must remain available to textures because duration updates use a separate side channel.

## Output and Timing Validation

Decode representative BGRA and NV12 clips and validate all of the following before accepting NV12:

- BT.709 color matrix and limited-range YCbCr signaling/levels are correct.
- Black and white levels, neutral grays, and saturated colors show no visible shift.
- Matching changing-content frames pass an objective comparison within the expected RGB-to-YUV conversion tolerance; use PSNR/SSIM or a lossless frame-difference workflow.
- Video PTS values are monotonic, each duration is positive, and sample end times cover the requested wall-clock clip duration.
- Audio and video start/end alignment remains stable through idle spans and scheduler stalls.
- The first frame is present, the final held frame is not truncated, and heartbeat keyframes remain available for replay trimming.
- Average bitrate and active-content encoder settings remain unchanged within normal run-to-run variance.

Also test recording start/stop, static startup, replay saves spanning idle periods, back-to-back replay saves, monitor switching, WGC zero-copy on and off, queue saturation, device loss/recovery, and recovery-manifest finalization.

## Acceptance Rule

Accept lossless idle coalescing when static NVENC submissions fall, timeline duration and A/V sync remain correct, no distinct source image is lost, and lifecycle regression tests pass.

BGRA remains the preferred shipping input. Consider changing that decision only if NV12 works without fallback and improves total relevant GPU cost or game frame time without color, quality, timing, drop, or reliability regressions. A lower Video Encode percentage accompanied by equivalent or greater Video Processing/3D work, worse game frame time, or changed output is not a win.

Do not publish a claimed reduction from the observed 26% GTX 1060 Video Encode utilization until the BGRA coalescing on/off rows contain valid measured results. The failed NV12 input-view attempt is a compatibility result, not performance evidence.

## Windows Performance Recorder

Record a short trace:

```powershell
wpr -start CPU -start GPU -filemode
Start-Sleep -Seconds 60
wpr -stop "$env:USERPROFILE\Desktop\backtrack.etl"
```

Open the ETL in Windows Performance Analyzer and inspect CPU Usage by Process and Thread, GPU Utilization by engine, Disk Usage, Context Switches, and DPC/ISR activity. Capture and encode threads should show short wakeups around frame boundaries rather than sustained CPU work.

## GPUView and Failure Indicators

Use GPUView to verify D3D11 video-processor/copy work and NVENC work without CPU staging-texture readbacks.

- High encode-thread CPU can indicate NVENC initialization or mapping failure.
- Video Processing activity is expected for BGRA-to-NV12 conversion and must be included in the comparison.
- Some CPU during stop/save is expected while Media Foundation writes MP4/AAC output.
- Frame drops with high queue depth indicate capture/encoder backpressure; they are distinct from coalesced intervals.
- GPU protection drops mean Adaptive GPU deliberately sacrificed work to protect gameplay.
- Repeated capture recreation commonly indicates monitor changes, protected content, or driver/device reset and must be investigated before using the run as benchmark evidence.
