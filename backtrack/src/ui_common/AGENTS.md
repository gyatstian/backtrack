# ui_common

## Purpose

shared services and Win32 helpers for the studio window.

## Ownership

- `RecorderActions` executes a recorder action, checks releases, and
  reconciles auto-record ownership. the caller owns serialization and worker
  lifetime.
- `SettingsPublication` publishes a candidate before advancing the baseline
  or applying startup registration and UI preferences.
- `DiagnosticText` formats runtime and encoder reports; `DiagnosticsLog`
  exports a bounded snapshot without replacing or aliasing the live log.
- `WindowTools` owns control, picker, media-thumbnail, size and date text, and
  notification helpers. window-specific navigation and layout stay out.

## Local Contracts

- `WindowTools` layout moves don't repaint or copy pixels. the caller queues
  a repaint of the affected parent and children after positioning.
- release checks use cancellable async WinHTTP with a total deadline. request
  buffers and handles must remain alive through pending callbacks.
- notification sounds keep async playback buffers alive. load and scale custom
  WAVs outside the playback mutex; skip optional feedback on contention. scale
  only the `data` chunk extent in PCM8/16. use a scaled built-in tone when a
  custom sound cannot be attenuated.
- log export flushes the logger, stages in an exclusively created sibling,
  replaces by handle, and removes only its own staging file on failure.

## Verification

`LoggerDiagnosticsTests` and the window suites exercise the shared code.

## Child DOX Index

none.
