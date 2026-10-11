# integrations

## Purpose

Optional hooks into other applications: the Discord Rich Presence client and the
League of Legends kill reminder.

## Ownership

- Owns: `DiscordRichPresence`, `LeagueOfLegendsIntegration`, their worker
  threads, and the protocol details of talking to those applications.
- Does not own: the actions they trigger. Both report through a callback or a
  posted message; `MainWindowController.cpp` reacts.

## Local Contracts

- `LeagueOfLegendsIntegration` polls the League Live Client Data endpoint while a
  client is running and invokes its `KillCallback`, which the window turns into
  `kLeagueKillDetectedMessage`. Kill reminder only; the integration does not read
  gameplay state or trigger a save itself.
- `DiscordRichPresence` speaks the local Discord IPC named-pipe protocol: connect,
  handshake, send framed opcodes, drain the inbox. It is a hand-written client,
  so keep the framing and opcode handling in this folder rather than reaching for
  a library.
- Presence modes are `Always`, `FullscreenOnly`, and `Off`. `FullscreenOnly`
  relies on `platform/Win32Util.h:foregroundWindowIsFullscreen`.
- The activity text names the foreground application, using the same source as
  replay naming (`foregroundApplicationName`). Keep those two consistent; a
  mismatch is visible to users as "it names the wrong game".
- Disconnect cleanly on `stop()` and in the destructor. A crashed session that
  leaves a socket open shows a ghost presence in the user's Discord until it
  expires, which is the visible symptom of a bug here.
- Both workers run on their own thread, watch `stopRequested_`, and must not let
  an exception escape the thread procedure. Neither integration may block the
  UI thread or fail app startup: a missing game client or Discord must be
  invisible.

## Work Guidance

- These are third-party protocols, so they break without notice. Keep the
  failure paths tolerant and log the reason at `debug` or `warning`, never
  `error`, for an expected absence.
- No game-specific behavior beyond League of Legends. The README states this
  plainly to users; do not imply broader support.

## Verification

No automated test; both depend on external applications. Manual check: with
League running, confirm the kill callback fires; with Discord running, confirm
the presence appears, updates on a foreground change, and disappears on exit.

## Child DOX Index

None. This folder's children are files only.