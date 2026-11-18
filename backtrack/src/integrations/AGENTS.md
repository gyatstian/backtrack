# integrations

## Purpose

Optional hooks into other applications: the Discord Rich Presence client and the
League of Legends kill reminder.

## Ownership

- Owns: `DiscordRichPresence`, `LeagueOfLegendsIntegration`, their worker
  threads, the shared `WinHttpJson.h` GET + JSON helpers, and the protocol
  details of talking to those applications.
- Does not own: the actions they trigger. Both report through a callback or a
  posted message; `StudioSession.cpp` reacts.

## Local Contracts

- `LeagueOfLegendsIntegration` detects the game by reaching its Live Client Data
  endpoint on `127.0.0.1:2999` with WinHTTP. There is no process-name match, no
  window match, and no read of the game's memory; a client is "running" exactly
  while that endpoint answers.
- It polls `/liveclientdata/activeplayer` as the liveness probe and
  `/liveclientdata/eventdata` for the feed, firing `KillCallback` when a
  `ChampionKill` names the active player (`riotId`, `summonerName`, or
  `riotIdGameName`, compared tag-insensitively). The window turns that into
  `kLeagueKillDetectedMessage`. Kill reminder only: it never triggers a save.
- The first feed snapshot after connection and one whose maximum id decreases or
  maximum time drops by more than 0.001 seconds establish a baseline without
  replaying kills. Empty snapshots retain the previous baseline.
- Live Client Data responses are size-capped (`kMaxHttpResponseBytes`, 4 MiB,
  in `WinHttpJson.h`). Port
  2999 is loopback but not ours, so an oversized body is refused, never
  buffered, and the session is opened `WINHTTP_ACCESS_TYPE_NO_PROXY` so a system
  proxy cannot redirect the poll off the machine. That cap is on the raw
  `std::string` body; `trimText` only ever sees the extracted player names
  inside it, so its erase-from-front cost is bounded by a name, not by the body.
- `WinHttpJson.h` owns the status-checked, size-capped GET and `\u`-safe JSON
  string reader for League and the update check in `StudioSession`. both use async
  sessions: stop or the total request deadline closes the request, and the
  callback retains the read buffer and parent handles until handle closing.
  the synchronous overload has only per-stage timeouts. `ignoreCert` is for
  the self-signed local endpoint only; a failed status query logs status=0.
- Liveness and failures are published through `stats()` (`gameAvailable`,
  `lastError`) for the Diagnostics page; the worker sets them on
  detect/loss/fault transitions, not per poll.
- `DiscordRichPresence` speaks the local Discord IPC named-pipe protocol: open
  `\\?\pipe\discord-ipc-0` through `-9`, handshake (opcode 0), send framed
  commands (opcode 1), drain the inbox, and treat opcode 2 as a close. It is a
  hand-written client, so keep the framing and opcode handling in this folder
  rather than reaching for a library. Inbound frames are capped at
  `kMaxInboundFrameBytes`; an oversized length drops the connection instead of
  sizing an allocation. `kDiscordApplicationId` must be a real Discord
  application id or Discord silently ignores every frame.
- Presence modes are `Always`, `FullscreenOnly`, and `Off`. `start()` with
  `Off` stops the worker and returns, mirroring
  `VoiceCommandService::configure`, so the caller passes the mode through
  unconditionally. `FullscreenOnly`
  relies on `platform/Win32Util.h:foregroundWindowIsFullscreen`; leaving
  fullscreen sends a null-activity frame, so the presence disappears rather than
  freezing on the last game. The elapsed-timer policy (`resetTimerOnSwitch_`,
  from `GameIntegrationSettings::discordRichPresenceResetTimerOnSwitch`) picks
  whether the activity start timestamp restarts on every foreground-app switch
  or stays fixed for the worker's lifetime; it applies to both non-`Off` modes.
- The activity text names the foreground application, using the same source as
  replay naming (`foregroundApplicationName`). Keep those two consistent; a
  mismatch is visible to users as "it names the wrong game".
- pipe reads and writes are overlapped, stop-aware and deadline-limited. a
  completion wait owns each operation's duplicate handle, `OVERLAPPED` and
  buffer until the kernel signals completion, including after cancellation.
  exit attempts a null-activity frame with a 100 ms I/O deadline, then closes
  the pipe; an unresponsive peer may retain stale presence. do not add
  `FlushFileBuffers`.
- Both workers run on their own named thread, watch `stopRequested_`, and wrap
  their body so no exception escapes the thread procedure. Neither integration
  may fail app startup: a missing game client or Discord only logs and retries.
  `stop()` joins the worker; cancellation completion can outlive that join.

## Work Guidance

- These are third-party protocols, so they break without notice. Keep the
  failure paths tolerant and log the reason at `debug` or `warning`, never
  `error`, for an expected absence.
- No game-specific behavior beyond League of Legends. The README states this
  plainly to users; do not imply broader support.

## Verification

`LeagueOfLegendsIntegrationTests`, `DiscordRichPresenceTests`, and
`WinHttpJsonTests` are registered with CTest; see `backtrack/tests` for their
field-scanning, feed, pipe, and HTTP checks. manual check: with League running,
confirm the kill callback fires; with Discord
running, confirm the presence appears, updates when the foreground application
changes (the foreground is only re-read every `kActivityUpdateInterval`), and
disappears on exit.

## Child DOX Index

None. This folder's children are files only.