# voice

## Purpose

Optional spoken commands ("clip that", "record") recognized through Windows
SAPI and posted back to the main window.

## Ownership

- Owns: `VoiceCommandService`, the SAPI grammar, the worker thread, and
  `VoiceCommandStats`.
- Does not own: what the command does. The service posts
  `kVoiceCommandMessage`; `MainWindowController.cpp` performs the save or the
  recording toggle.

## Local Contracts

- `configure(window, mode, replayEnabled)` is the only entry point. The service
  needs the window handle (to post results) and knows that saving a replay is
  only meaningful when the replay buffer is enabled.
- Modes are `Disabled`, `ClipThat`, and `RecordVideo`, from
  `GameIntegrationSettings::VoiceCommandMode`.
- Recognition runs on its own thread with a `stopEvent_`; `stop()` must set the
  event so the thread can exit without waiting on SAPI. Never block the UI thread
  on recognition.
- SAPI is optional at runtime. A machine with no speech recognizer installed
  must report that in `VoiceCommandStats::status` and bump
  `initializationFailures`, not crash or retry in a loop.
- Readiness and counters are published through `stats()` for the UI, so keep
  them updated when the recognizer actually becomes ready, not when it starts.

## Work Guidance

- Audio for recognition comes from the default capture device through SAPI, not
  from the recorder's microphone pipeline. Do not couple the two.
- A new command needs a grammar entry here and a branch in the message handler.

## Verification

No automated test. Requires a machine with a working SAPI recognizer: confirm
each mode fires the expected action, that `Disabled` starts no thread, and that
stopping the service while it is listening exits promptly.

## Child DOX Index

None. This folder's children are files only.