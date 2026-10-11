# replay

## Purpose

The rolling buffer of recent encoded video and audio that a replay save cuts
from. Two interchangeable backends: process memory by default, a fixed-capacity
ring file on disk when the user opts in.

## Ownership

- Owns: `ReplayBuffer`, its metadata model, the ring file, and the trimming and
  save logic.
- Does not own: muxing (`mux/Mp4Muxer.h` is called from `saveTo`), capture, or
  the disk location policy, which comes from `AppSettings::replayRingDirectory`.

## Local Contracts

- `configure` takes the storage directory and capacity only when
  `settings.diskBackedVideo` is set. With it off both are ignored and buffering
  stays fully in RAM.
- Save-path logic must stay backend agnostic. `VideoPacketMeta` exists so the
  trimming, cut, and parameter-set logic reads the same for both backends; do
  not branch on the backend where metadata is enough.
- Only packets that `BitstreamUtils::bitstreamContainsKeyFrame` accepts are cut
  points. `trimLocked` drops back to the oldest buffered keyframe, so a buffer
  that has lost its keyframes saves nothing rather than an undecodable clip.
- `codecHeaderBytes_` caches the parameter sets from the first buffered packet
  that carried them and is prepended to a cut whose start packet lacks headers,
  because a driver can ignore header insertion.
- `extendLastVideoDuration` mutates the newest packet in place under the mutex
  instead of copying a full payload per emitted interval. `saveTo` snapshots
  metadata under the same mutex, so no reader sees it unsynchronized. Keep both
  halves of that invariant together.
- Ring protocol: a save takes a pin (`savePinsRefs_`), and while pinned, pushes
  that need space wait on `ringSpace_` instead of byte-trimming, because
  trimming could evict a packet an in-flight save has not read yet. Any new space
  policy must preserve the pin; the count is a counter rather than a bool so a
  second concurrent save cannot break the protocol silently.
- A ring write failure (typically a full disk) is not propagated as an error.
  `failOverToRamLocked` discards the buffered video window and continues in
  memory for the rest of the session: a coherent clip matters more than the
  storage mode.
- `newestVideoPts100ns` exists because PTS-age trimming cannot detect a stalled
  capture or encode pipeline; heartbeats keep advancing timestamps while the
  pipeline is idle. Callers use it to detect that case.

## Work Guidance

- The class is not copyable and owns a `HANDLE`. Keep it a member of
  `RecorderController`, never a value passed around.
- Payloads may wrap the end of the ring and are recorded as up to two extents.
  Reassembly bugs show up as clips that corrupt after roughly one buffer's worth
  of recording; test across a wrap when touching this code.
- Bytes are copied only on the save path. Pushes are metadata plus a ring append
  or a shared pointer move.

## Verification

- `ReplayBufferTests` (CTest) covers the real buffer with `Mp4Muxer` entry
  points stubbed inside the test source. Extend it for trimming, wrap-around
  reassembly, parameter-set prepending, and the disk backend.
- No test covers the actual ring file against a full disk; that is a manual
  check.

## Child DOX Index

None. This folder's children are files only.