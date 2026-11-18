# voice

## Purpose

Optional spoken commands ("Backtrack clip that", "Record video") recognized
through Windows SAPI and posted back to the main window.

## Ownership

- Owns: `VoiceCommandService`, the SAPI grammar, the worker thread, and
  `VoiceCommandStats`.
- Does not own: what the command does. The service posts
  `kVoiceCommandMessage` from `VoiceCommandService.h`. Both windows respond
  by requesting a replay. `RecordVideo` changes the phrase, not the action.

## Local Contracts

- `configure(window, mode, replayEnabled)` is the only entry point. The service
  needs the window handle (to post results) and knows that saving a replay is
  only meaningful when the replay buffer is enabled.
- `configure` early-returns when `(window, mode, replayEnabled)` is unchanged
  from the last call, so save completion does not tear down and rebuild the
  SAPI engine (including the slow `SetInput` mic open); a real change always
  re-inits.
- Modes are `Disabled`, `ClipThat`, and `RecordVideo`, from
  `GameIntegrationSettings::VoiceCommandMode`. The grammar has one rule,
  `BacktrackCommand`, whose only word transition is the mode's phrase
  (`phraseFor`).
- Recognition runs on its own thread with a `stopEvent_`; `stop()` must set the
  event so the thread can exit without waiting on SAPI. Never block the UI thread
  on recognition.
- The worker re-checks `enabled_` immediately after `SetInput` opens the default
  capture device, which is the slow part: `stop()` joins on the UI thread, and
  everything after that check is skipped when it sees the stop. Every early
  return in `run()` pairs `CoInitializeEx` with `CoUninitialize`.
- An accepted recognition must match the rule id, must not come back
  `SP_LOW_CONFIDENCE`, and must be at least `kCooldownMs` after the last
  accepted one. Anything else counts as `rejected`.
- SAPI is optional at runtime. A machine with no speech recognizer installed
  must report that in `VoiceCommandStats::status` and bump
  `initializationFailures`, not crash or retry in a loop.
- Readiness and counters are published through `stats()` for the UI, so keep
  them updated when the recognizer actually becomes ready, not when it starts.

## Work Guidance

- Audio for recognition comes from the default capture device through SAPI, not
  from the recorder's microphone pipeline. Do not couple the two.
- A new command needs a `VoiceCommandMode` value in `core/Types.h`, a
  `phraseFor` entry, a matching combo entry in `ui_common`, and a handler that can tell
  the commands apart: `kVoiceCommandMessage` carries no mode.

## Verification

No automated test. Requires a machine with a working SAPI recognizer: confirm
each mode recognizes its own phrase and saves a replay, that `Disabled` and a
disabled replay buffer start no thread, and that stopping the service while it
is listening exits promptly.

## Child DOX Index

None. This folder's children are files only.