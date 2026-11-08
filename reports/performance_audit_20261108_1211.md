# Performance Audit: backtrack (whole repository)

- Date: 2026-11-08
- Scope: whole repository (`main`, working tree), 261 tracked files; 58 files currently modified in the working tree
- Method: static audit (no load test), 4 parallel subagents over the source domains (~35k LOC of app code; vendored RNNoise and AMF/NVENC SDK headers under `src/third_party` and `src/Interface` excluded), then a manual refute pass verifying every top finding against the working-tree source
- Provenance: where the git diff let us tell, findings are marked "new in working tree" or "pre-existing"; the rest are "pre-existing or changed, unverified". Unless stated, treat a finding as pre-existing.

This is a real-time Windows screen recorder. The four failure modes of the
standard audit map here as: per-frame/per-callback work (N+1), redundant
copies and rescans (over-fetch), missing/inadequate data structures, and
missing caches. "Breaks at scale" means: larger replay windows, higher
resolution/FPS, more saved clips, longer recordings, slow disk.

---

## Top findings (ranked by impact-per-effort)

1. **Replay save pins the whole ring while it is read back — capture stalls for the entire read** (replay; High effort, High priority). The one finding that breaks the core "a save must not block capture" invariant.
2. **Auto-record polls the full desktop every 2 s on the UI thread** (ui; Low effort, Medium-High priority). Permanent UI-thread stall while enabled.
3. **Per-push overlapped-I/O machinery + full packet copy on the encode thread in disk mode** (replay; Medium effort, Medium priority). Paid every frame at 60–240 fps.
4. **Desktop Duplication copies the full frame twice even when no cursor is composited** (capture; Low-Medium effort, Medium priority).
5. **WASAPI capture heap-allocates + memcpys every audio packet on the real-time thread** (audio; Medium effort, Medium priority).
6. **Audio convert allocates per packet and copies the packet three times** (audio; Low effort, Medium priority).
7. **`videoSpan100ns` is O(n) per call and polled every 20 ms during save preflight** (replay; Low effort, Medium priority).
8. **Library scan walks the root twice and opens every clip's sidecars per scan, uncached** (clips; Medium effort, Medium priority).

---

## replay / mux

**ReplayBuffer save pin (Finding 1, the big one):**
  - Finding: At steady state the ring is full, so every push during a save finds `freeBytes() < size`, sees the save pin, and blocks on the condition variable until the save has read all buffered packets out of the ring. The stall equals the whole ring-read duration (window bytes ÷ disk speed), not the mux.
  - Evidence: `ReplayBuffer.cpp:730` (`++savePinsRefs_`), pin released only after the video dump + audio reassembly loops (`ReplayBuffer.cpp:1036`); producer blocks per frame in `RingFile.h:176-179` (`if (pinRefs) space.wait(lock)`) called from the encode thread under the replay mutex (`ReplayBuffer.cpp:1422-1426`).
  - Recommendation: replace binary pin/no-pin with an incremental read cursor — the save publishes bytes already consumed, and a producer evicts only ring bytes at or before it. Smaller alternative: bound the wait and drop on a keyframe boundary so a stalled capture never blocks. The correct fix touches the ring protocol.
  - Effort: High — Priority: High
  - Expected effect: capture latency during save goes from "whole ring read" (0.1 s at 60 s window on NVMe, ~70 s at 1 h @ 24 Mbps on a 7200 rpm HDD) to bounded/zero.
  - Note: `backtrack/src/replay/AGENTS.md` documents this as a designed tradeoff ("saves are rare and short"); the audit's point is that the bound is exactly what breaks as windows grow and disks get slow. Verified against the AGENTS.md contract and the code.

**RingFile per-push overlapped-I/O machinery (Finding 3):**
  - Finding: Every disk push, on the encode thread under `mutex_`, does one heap allocation for the operation, a second allocation for a packet-sized buffer plus a full `memcpy` of the packet, `DuplicateHandle`, `CreateEventW`, `CreateThreadpoolWait`, a synchronous `WaitForSingleObject`, then `CloseThreadpoolWait`. The overlapped/cancel machinery only serves the rare 8 s timeout path but is paid every frame.
  - Evidence: `RingFile.h:264-318` (`make_shared<Operation>` :266, `new uint8_t[size]` :271, `DuplicateHandle` :276, `CreateEventW` :281, `memcpy` :288, `CreateThreadpoolWait` :296, wait :312, close :317/256).
  - Recommendation: pool `Operation` objects (reuse the duplicated handle, event, threadpool wait); write the packet directly from the caller's buffer on the happy path, keep the owned-buffer copy only for the cancellation path.
  - Effort: Medium — Priority: Medium
  - Expected effect: lower per-frame syscall/allocation churn on the encode thread in disk mode.

**`videoSpan100ns` O(n) polled during save preflight (Finding 7):**
  - Finding: `videoSpan100ns` rebuilds a metadata vector of every buffered packet, finds the first keyframe, erases the prefix, and runs the window cut — ~4 O(n) passes plus an allocation — and the save preflight polls it every 20 ms for up to 3 s (`RecorderController.cpp:705-714`). At a 1 h window (~216 k packets) that is ~1.5 GB of transient allocation per save.
  - Evidence: `ReplayBuffer.cpp:1156-1191`; poll loop `RecorderController.cpp:705-714`.
  - Recommendation: cache incrementally — the window cut starts at the queue front (always a keyframe after trim), so `span = maxPresentationEnd − frontPts`; update `maxPresentationEnd` O(1) on push, recompute only when the max-end packet is evicted. One-line cheaper fix: sample every ~10th poll (200 ms).
  - Effort: Low (coarser poll) to Medium (incremental cache) — Priority: Medium
  - Expected effect: save preflight stops doing O(n) work 50×/s. Only the stale-buffer/refresh path hits this; the common path calls it once.

**Eviction loops re-scan from `begin()` for the next keyframe:**
  - Finding: The byte-cap eviction loop pops one packet then runs a full `find_if` from `video_.begin()` for the next keyframe, repeated per iteration — O(n × GOP) on a VBR overshoot burst (~13 M comparisons for 216 k packets at GOP 120). Same pattern in the disk eviction loop.
  - Evidence: `ReplayBuffer.cpp:1293-1300`, `:1465-1475`.
  - Recommendation: track a "next keyframe" iterator and start the scan from the current pop position.
  - Effort: Low — Priority: Low
  - Expected effect: overshoot eviction drops to ~O(pops). Steady-state trim is already amortized O(1).

**Disk-audio save reassembly copies bytes twice + per-packet format re-parse:**
  - Finding: Disk-audio reassembly reads each packet into a scratch then copies it again into a fresh `AudioPacket::bytes`; separately the audio format is re-parsed per packet on both save and recording paths although it is constant after ingest canonicalization.
  - Evidence: `ReplayBuffer.cpp:1012-1034` (scratch hop + `bytes.assign`), `ReplayBuffer.cpp:141-143` (`packetFormatMatches` → `parseAudioFormat`), `Mp4Muxer.cpp` `writeTimelineAudioPacket` (~:470).
  - Recommendation: read ring bytes directly into the resized `packet->bytes`; compare format by shared `WaveFormatBlob` pointer identity.
  - Effort: Low — Priority: Low
  - Expected effect: one full audio-window memcpy saved per save.

---

## app / clips / settings / core

**Encode loop allocates the packet vector per frame:**
  - Finding: `std::vector<EncodedPacket> ready;` is declared inside the per-frame encode loop, so its heap backing is freed and re-grown every frame.
  - Evidence: `RecorderController.cpp:3028-3036` (`std::vector<EncodedPacket> ready;` inside the `while`, `encoder_->encodeFrame(frame, ready)` :3032).
  - Recommendation: hoist `ready` above the loop and `ready.clear()` after publishing; `clear()` keeps capacity.
  - Effort: Low — Priority: Low-Medium
  - Expected effect: zero per-frame heap alloc/free on the encode thread.

**Drop-path logging acquires the global logger mutex before the enabled/rate-limit check:**
  - Finding: Every dropped frame calls `Logger::instance().warning(...)`; each call acquires the logger's global mutex and the `enabled_` check runs only after the lock (`Logger.cpp:258` `writeLocked`). Under GPU-protection drop storms (exactly when the machine is overloaded) the capture thread pays a global-mutex acquire per dropped frame.
  - Evidence: `RecorderController.cpp:1885` (scaler failed), `:1911` (GPU protection), `:1923` (encoder queue full); `Logger.cpp:244-246` (lock before `writeLocked`), `:258` (`if (!enabled_) return;`). The audio-queue drop path already gates on `dropped == 1 || dropped % 256 == 0` (`RecorderController.cpp:3120-3125`).
  - Recommendation: gate the per-frame drop logs on a local counter (same 1 / every-256 pattern), or add a lock-free `enabled()` fast path before building the call.
  - Effort: Low — Priority: Low-Medium
  - Expected effect: drop storms stop paying a global-mutex acquire per frame. **New in working tree** (the three warning lines are `+` in the diff).

**Library scan walks the root twice and re-reads every clip's sidecars per scan (Finding 8):**
  - Finding: `listClips` calls `clipRoots()` (walks the library root to find game folders) then `appendDirectoryClips` walks the same root again. Per clip it runs `exists(favoriteMarker)` (a stat) and `readTags(path)` (file open + parse) on every scan with no cache — only duration is cached.
  - Evidence: `ClipManager.cpp:231` (`clipRoots`) + `:238` (`appendDirectoryClips(root)`); `:219` (`info.favorite = std::filesystem::exists(...)`), `:221` (`info.tags = readTags(path)`); `ClipStorage.h:71-81` (`directory_iterator it(root,...)`); duration cache only: `ClipManager.cpp:244-276`.
  - Recommendation: merge game-folder discovery into the single directory pass; cache favorite/tags keyed by the size+mtime identity already used for `durationCache_`, invalidated by the in-place single-clip paths (`setFavorite`/`addTag`/`removeClip`/`renameClip`) and pruned in the existing stale-key pass.
  - Effort: Medium — Priority: Medium
  - Expected effect: refresh drops from one double directory walk + O(n) sidecar file opens to one walk + O(n) cache lookups. Refresh is user-triggered (page entry, F5, post-save), not a timer, and runs off-UI-thread, so this is scan latency at high clip counts.

**2 s sound-separation poll recomputes the muted-executable key list from scratch:**
  - Finding: Every 2 s poll rebuilds the muted-executable key vector — allocation, per-app `normalizedPathKey`, sort, dedupe — even when the set is unchanged.
  - Evidence: `RecorderController.cpp:2079-2081` (poll) → `:3211` `mutedSoundSeparationExecutableKeys` → `:174-196` (loop + `sort` + `unique`).
  - Recommendation: cache the computed vector, recompute only when the relevant settings change. The session-mute re-assert inside the poll is deliberate and stays.
  - Effort: Low — Priority: Low
  - Expected effect: the 2 s poll drops an alloc+sort when nothing changed.

**`updateSettings` materializes ~5 full `AppSettings` copies per call:**
  - Finding: Each `updateSettings` builds `currentWithoutVoice`, `currentNoReplay`, `incomingNoReplay`, `currentBase`, `incomingBase` — each duplicating both executable lists (up to 128 entries each) and paths — to run equality checks.
  - Evidence: `RecorderController.cpp:408-490`; `AppSettings` owns two `std::vector<...App>` (`Types.h:257-285`). Rate is bounded by the single controller worker (incl. volume-drag ticks, `MainWindow.cpp:588/595`), so minor.
  - Recommendation: compare only the sub-structs that differ, or copy the lists once into shared temporaries.
  - Effort: Low-Medium — Priority: Low
  - Expected effect: settings-action path stops duplicating up to 256 app entries 5× per call.

---

## capture / encoder / audio (real-time hot path)

**Desktop Duplication: two full-frame GPU copies per frame even when no cursor is composited (Finding 4):**
  - Finding: Every desktop-changed frame is copied twice on the GPU (`acquired → desktopCopy_ → slot`). The `desktopCopy_` hop exists only to feed the cursor compositor's pixel shader; when no cursor is drawn (`captureCursor_ == false`, cursor hidden, or no shape yet) the whole intermediate write is wasted bandwidth.
  - Evidence: `DesktopDuplicationCapture.cpp:444` (`CopyResource(desktopCopy_.Get(), acquiredTexture.Get())`), `:459` (`drawCursor` = `captureCursor_ && cursorVisible_ && cursorShapeValid_ && ensureCompositor()`), `:462-464` (slot copy always; composite conditional), `:577` (`desktopCopySrv_` bound only in `compositeCursor`, `:809`).
  - Recommendation: when `!drawCursor` (and no cursor composited since the last desktop change — track a dirty flag so a reappearing cursor composites over a valid copy), copy `acquiredTexture → slot` directly and skip the `desktopCopy_` write. Keep the two-copy flow when a cursor is drawn.
  - Effort: Low-Medium — Priority: Medium-High
  - Expected effect: one fewer full-frame `CopyResource` per frame (~8 MB each way at 4K BGRA) on the capture thread in the common cursor-off config.

**WASAPI capture heap-allocates + memcpys every audio packet on the real-time thread (Finding 5):**
  - Finding: The WASAPI capture thread (MMCSS "Pro Audio") heap-allocates a `std::vector<uint8_t>` and copies every buffer inside the `GetBuffer`/`ReleaseBuffer` loop, at device packet cadence (~100–200 packets/s per track; more at higher sample rates). Silent packets allocate a zeroed buffer too. This is the only real per-callback allocation in the audio cluster — the handoff itself is a move into an SPSC ring.
  - Evidence: `WasapiCapture.cpp:1202` (`GetBuffer`), `:1247-1249` (`packet.bytes.assign(byteCount, 0)` / `assign(data, data+byteCount)`), `:1260/:1264` (`callback(std::move(packet))`).
  - Recommendation: a small pool of preallocated fixed-capacity `AudioPacket` buffers recycled on the writer side, so the real-time thread does zero malloc/free per callback. Keep the move-based handoff.
  - Effort: Medium — Priority: Medium
  - Expected effect: zero heap allocation on the real-time audio thread; removes malloc/free glitch risk at sustained packet rates.

**Audio convert allocates per packet and copies the packet three times (Finding 6):**
  - Finding: `convertAudioPacketToCanonical` allocates a `converted` float vector and a `weights` vector per packet, then memcpys the result back into `packet.bytes` — read, write to a fresh vector, memcpy back: three touches and two heap allocations per packet even for the common stereo case.
  - Evidence: `AudioConvert.cpp:373` (`std::vector<ChannelWeight> weights; weights.reserve(source.channels)`), `:409` (`std::vector<float> converted(outputFrames * 2)`), `:434-435` (`packet.bytes.resize(...); std::memcpy(packet.bytes.data(), converted.data(), ...)`).
  - Recommendation: make `converted`/`weights` reusable scratch members of the audio-writer state (single writer thread, no sharing hazard); write directly into a pre-sized `packet.bytes`; fixed 2-entry weight array for stereo. The already-canonical fast path (`AudioConvert.cpp:395-405`) is good and stays.
  - Effort: Low — Priority: Medium
  - Expected effect: no per-packet heap churn and one fewer full packet copy on the audio writer thread; meaningful over long recordings.

**WGC per-frame heap allocation for the zero-copy lease:**
  - Finding: In zero-copy mode each accepted frame does `new DirectFrameLease{}` plus a `shared_ptr` control block on the capture thread, at capture FPS.
  - Evidence: `WgcCaptureSource.cpp:335-344`; the per-frame COM `GetInterface` for the D3D11 texture (`:305-307`) is inherent to WGC and not the target.
  - Recommendation: recycle a small fixed array of preallocated lease objects through an atomic freelist (bounded by the existing lease cap).
  - Effort: Low — Priority: Low
  - Expected effect: removes the capture-thread heap alloc at FPS rate; the WGC COM call per frame remains.

---

## ui / integrations / voice / hotkeys / platform

**Auto-record polls the full desktop every 2 s on the UI thread (Finding 2):**
  - Finding: With auto record on, every 2 s tick walks all visible top-level windows and opens each owning process to read its image path — on the UI thread, permanently, even during a long recording of an already-matched app and while minimized to tray. The `ScopedLatencyWarn` 50 ms budget at the top of the poll shows the authors expect it can be slow; it delays `WM_HOTKEY`/`WM_PAINT`/status dispatch during recording.
  - Evidence: `MainWindowController.cpp:160-175` (`pollAutoRecord` → `WasapiCapture::enumerateOpenApps()` at :172); timer armed at startup (`MainWindow.cpp:304`, `UiIds.h:133` `kAutoRecordPollMs = 2000`); `WasapiCapture.cpp:782-830` (`EnumWindows` + `GetWindowTextLengthW` + `OpenProcess` + `QueryFullProcessImageNameW` per visible window). The enumerate call is skipped when auto record is off (guard at :170), so this only bites while enabled.
  - Recommendation: in the already-matched, already-recording state, poll only the matched app's liveness (`OpenProcess` + `WaitForSingleObject(..., 0)` on the stored process id, or `IsWindow` on its HWND) instead of re-enumerating the desktop; fall back to full enumeration only when the match is lost. Optionally lengthen the cadence in the stable state.
  - Effort: Low — Priority: Medium-High
  - Expected effect: removes a periodic O(#visible windows) UI-thread stall that runs for the whole session.

**Voice: full SAPI re-init (incl. mic open) + `stop()` join on the UI thread on every settings save:**
  - Finding: Every controller action completion calls `voiceCommandService_.configure(...)` unconditionally, which tears down and rebuilds the recognizer (including `SetInput`, the documented slow step) and joins the worker — on the UI thread — even when the voice mode/replay-enabled state is unchanged. Recognition persists across commands (not per-command), so this is a redundant per-save re-init.
  - Evidence: `MainWindowController.cpp:72-92` (`updateGameIntegrations` → `configure` at :89, called from `handleControllerActionComplete` on the UI thread); `VoiceCommandService.cpp:42-65` (`configure` → `stop()` then new thread) and `:124-125` (`SetInput` = device open).
  - Recommendation: track the last-configured `(mode, replayEnabled, window)` and early-return from `configure` when unchanged; only re-init on a real change.
  - Effort: Low — Priority: Low-Medium
  - Expected effect: removes a per-save UI-thread stall (SAPI teardown + mic reopen, tens-to-hundreds of ms) and recognizer churn.

**Owner-draw rows re-format strings per paint:**
  - Finding: Every repaint of every visible row re-formats name/tags/duration/size (`wstringstream`-based `durationToText`/`bytesToText`/`tagsToText`) and re-derives the lowercase thumbnail cache key, though the underlying `ClipInfo` is unchanged between paints. Runs per tile (×3) in Gallery and per row in List on the UI thread; scroll/hover/selection/thumbnail-ready each redo it.
  - Evidence: `MainWindowMessages.cpp:937` (thumbnail key per tile), `:963` (name build), `:973-974` (meta), `:1004-1015` (list mode); formatters in `UiHelpers.cpp`.
  - Recommendation: memoize the formatted row text and the thumbnail cache key per clip, invalidated on scan/rename/favorite/tag change. `thumbnailCacheKey` is a pure function of `path`.
  - Effort: Low-Medium — Priority: Low-Medium
  - Expected effect: removes repeated per-paint allocations during heavy scrolling of a large library.

**League integration re-parses the whole event list every 1 s poll:**
  - Finding: Each poll re-extracts every event of the whole match from the `eventdata` body into a fresh `std::vector<LiveEvent>` (4 field scans + several string allocations per event), then filters by `lastEventId`. Cost grows linearly with match length though only post-`lastEventId` events can be new.
  - Evidence: `LeagueOfLegendsIntegration.cpp:29-30` (1 s poll), `:104-193` (`parseEvents`), `:335-374` (parse all, then filter). Runs on the dedicated worker thread — worker-side waste only, no UI stall.
  - Recommendation: the body is ordered by event; locate the first event object with `EventID > lastEventId` (or the byte offset of the last seen event) before per-event extraction instead of materializing every `LiveEvent`.
  - Effort: Medium — Priority: Low
  - Expected effect: bounds per-poll cost as a match lengthens (currently unbounded growth).

**Discord re-reads the foreground app's exe version resource from disk every 2 s:**
  - Finding: Every 2 s the worker calls `foregroundApplicationName()` → `friendlyNameForImagePath` → `GetFileVersionInfoSizeW`/`GetFileVersionInfoW` (file open + resource read) regardless of whether the foreground changed. Only the send is gated on `gameName != lastGameName`.
  - Evidence: `DiscordRichPresence.cpp:253-260` (re-read every `kActivityUpdateInterval` = 2 s at `:39`), `Win32Util.cpp:326-340` + `:205-217`. Worker thread, no UI impact.
  - Recommendation: cache the resolved name keyed by foreground HWND/processId; recompute only when the foreground HWND changes.
  - Effort: Low — Priority: Low
  - Expected effect: removes a periodic exe file open/read that never changes between window switches.

**Whole-list row rebuild on the UI thread after every scan:**
  - Finding: On every scan completion the Library page does `LB_RESETCONTENT` + one `LB_ADDSTRING` per clip on the UI thread; for thousands of clips that is a per-scan UI-thread burst of SendMessage calls + listbox reallocations.
  - Evidence: `MainWindowLibrary.cpp:677-710` (rebuild loop), called at `:662` (scan ready), `:535` (refresh), `:842/:890` (single-clip edits). One-shot per scan (not per tick), and rescans are coalesced via generation.
  - Recommendation: issue `LB_INITSTORAGE(count, 0)` before the add loop to preallocate the listbox string array.
  - Effort: Low — Priority: Low
  - Expected effect: shaves the per-scan UI-thread burst for very large clip libraries.

---

## Already efficient (verified, not just claimed)

- **Settings are loaded once, never re-read per access.** `load()` runs at startup (`Application.cpp:59-60`); `save()` only on user actions. No per-frame/per-second parse path. `settings()` is never called on per-second or per-frame paths — the per-second reads use the lock-free mirrors (`publishLiveMirrors`), matching the app/AGENTS.md contract exactly.
- **Replay in-memory ring is a `std::deque`**, so front pops are O(1) (the "vector with O(n) erase-from-front" concern is refuted); RAM payloads move via `shared_ptr`, no byte copies. Trim is amortized O(1) per frame in steady state. Save reads ring bytes without holding the replay mutex; the pin is released before muxing, so the slowest phase (MF sink writer) never blocks capture. RecoveryStore writes incrementally and flushes at GOP boundaries, no per-packet fsync.
- **Encoder input/output handling is zero-copy, no staging buffers**: NVENC registers/maps the D3D11 textures directly and reuses a fixed bitstream-buffer set; AMF wraps the D3D11 surface via `CreateSurfaceFromDX11Native`. No per-frame staging or lock/unlock copies of frame data.
- **Scaler state is sticky, not re-programmed per frame** (`D3D11FrameScaler.cpp:233` diffs `VpState`), input-view lookups cached (`:376-401`); all D3D resources are pooled, pool exhaustion drops rather than blocks; no spin-waiting in capture loops (WGC waits on `frameEvent`, DD uses `AcquireNextFrame(timeoutMs)`).
- **Audio callback is light**: WASAPI callback only pushes a moved `AudioPacket` into a fixed-capacity SPSC ring (`RecorderController.cpp:3110-3124`); no conversion/mixing on the real-time thread. `convertAudioPacketToCanonical` has a fast path that skips already-canonical packets.
- **Logger buffers without per-line flush** (a log line never holds the global mutex across a disk write) and rate-limits per distinct message text.
- **Library work is off the UI thread and coalesced**: scan worker with generation/directory guard, single-clip actions patch the vector and repaint instead of rescanning, thumbnails cached by path+size+mtime and bounded (cap 48, visible rows + 1 overscan).
- **No blocking HTTP on the UI thread** (League/Discord/update-check each run on their own threads and post to the UI only on discrete events); hotkey hook proc is allocation-free; layout is delta-based (`DeferWindowPos` batch, paint coalescing).
- **Per-second reads never copy `AppSettings`**; diagnostics counters are atomic/lock-free mirrors; `stats()` uses `try_to_lock` so the UI never queues behind a GPU recreate.

## What needs runtime profiling to confirm

- The actual ring-read duration under the save pin (Finding 1) on the user's disk — ms of capture stall per save at real window sizes, and whether upstream buffering masks it or drops frames. **Highest-value measurement.**
- Whether the per-frame overlapped-I/O syscall churn (Finding 3) shows up as measurable encode-thread time vs. the synchronous write wait itself.
- Real logger-mutex contention during drop storms (drop-path logging finding).
- The auto-record `EnumWindows` scan's ms/tick on a real desktop (window count, OpenProcess cost) vs. the 50 ms budget.
- Real VBR overshoot frequency to judge whether the O(n×GOP) eviction bursts ever fire.
- Whether DD's forced owned-scaler-output third copy (documented tradeoff for surface-validity) is measurable GPU bandwidth at 4K; whether the pool depth would permit registering DD slots directly.
- Actual library refresh latency at high clip counts — whether the O(n) sidecar opens dominate the directory walk.
- Voice `configure`/`stop()` join latency during settings saves.

## Method notes

- 4 parallel subagents, one per module cluster (app/clips/settings/core; replay/mux; capture/encoder/audio; ui/integrations/voice/hotkeys/platform), each returning cited finding records. No reroutes to a weaker model occurred.
- Refute pass done by the main agent: verified the top findings (save pin, encode-loop vector, DD double copy, logger lock ordering, auto-record poll, voice re-init, RingFile machinery, ClipManager scan) against the working-tree source before writing this report. All cited line numbers are current as of 2026-11-08.
- This is a static review; no load test was run. Nothing was edited — the audit is read-only.
