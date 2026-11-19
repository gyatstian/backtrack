# studio

## Purpose

the native Win32 window: chrome, pages, session worker, gallery, diagnostics.

## Ownership

- `StudioWindow.cpp` owns chrome, layout, input and contextual footer.
  `StudioMotion` owns the navigation marker and capture confirmation motion.
- `StudioSettings.cpp`, `StudioGallery.cpp`, and `StudioDiagnostics.cpp` own
  page content. `StudioSession.cpp` owns the session worker, hotkeys, the tray,
  and integration lifetimes.
- use application services and `ui_common`; no other window exists.

## Local Contracts

- keep the overlapped window styles for native drag, snap, and double-click
  maximize/restore. `WM_NCCALCSIZE` replaces the native frame with the client
  title bar and keeps maximized content inside the work area. `WM_NCHITTEST`
  enables resize grips only when restored. layout keeps child windows out of
  those strips so the parent receives edge mouse input.
  the compact bar shows the app icon without title text. named minimize/close
  buttons blend into the bar at rest and are not tab stops. center their
  antialiased 2-dip strokes on the button bounds. the close mark is slightly
  wider than it is tall, with red hover and press fills.
- the main navigation bar touches the title bar and stays visible on every
  page. the settings page shows a second row of category tabs below it. the
  bars and page share one surface; selected navigation and the contextual
  footer share a fill. sections use inset labels in divider lines.
- use one 15-dip Bahnschrift SemiLight font (GDI weight 350). leave gaps
  between buttons in both navigation rows. animate the existing full-width
  neutral underline in each row's bottom band, not a second indicator. use
  an inset outline for keyboard focus when cues are visible: owner-drawn
  controls honour `ODS_NOFOCUSRECT` and the checkbox honours `UISF_HIDEFOCUS`.
  caption buttons draw a cached glyph alpha mask per DPI, not per-pixel
  supersampling on every paint.
- the window remembers its rect and maximized state through
  `AppSettings::window*`; restore keeps the stored rect only when it still
  intersects a monitor work area. write the placement on shutdown directly into
  the saved baseline, never through the settings draft, so closing cannot
  publish unsaved pages.
- the footer carries one persistent failure line for the last recorder or
  hotkey failure. it outlives `statusTimer` and clears only on a later
  successful action; ordinary status text still expires.
- controls with an ambiguous effect carry hover help: consequences, defaults,
  units, and what a blank value means. never restate the label or the checkbox
  text, and add no help to a control whose name already says what it does.
- layout uses dpi-scaled units. the footer occupies a stable reserved area;
  showing help or status must not reflow or overlay the page. keep it readable
  with keyboard focus as well as hover.
- make pointer motion visible through travel or a shape change, not just a
  color fade. the tab marker travels after navigation, the record glyph morphs
  on confirmed state changes, and the footer check draws on replay success.
  use windows' animation preference; keyboard and hotkey actions settle
  immediately. no frame work for idle, hidden, or minimized windows, and no
  per-frame layout or gallery repaint.
- controls have native roles and keyboard behavior. keep labels concise,
  spacing compact, and controls square unless rounding has a purpose.
  size page actions for their labels, not the full row.
- keep studio styling and navigation while following legacy settings placement.
  the capture page pairs the recording action with its shortcut; hotkey fields
  carry no label. the replay block is two lines, its enable checkbox with the
  replay shortcut above the save action and the buffer length; while replay is
  off the shortcut is hidden and the second line is hidden. `enabledWhen` hides
  a whole row and reflows, `visibleWhen` hides one control in place. replay sound
  belongs in audio settings and monitor options in capture settings. the save
  action sits at the left of the reserved footer; the window-wide update action
  sits at the right.
- the library toolbar is one row above the gallery: Open, Rename, Favorite from
  the left, Refresh and Delete in from the right edge. diagnostics keeps
  runtime, encoder, and recent log in separate columns with log actions above
  the log. size fields for their values. indent dependent settings with a
  vertical guide on the left; keep activation rules with their rows. a
  dependent row whose rule is false is hidden and the page reflows without it,
  so it draws no guide and takes no keyboard focus.
- apply live resize and form wheel bursts at paint time; finish the last
  resize on drag exit. height changes only reposition form rows if the usable
  width or scroll position changes. clamped scrolls do nothing. a hidden row
  takes no scroll height, so a page whose optional rows are hidden may not
  scroll at all.
- buffer scrolling pages with their native children. finish positioning before
  queueing repaint; don't blit old child pixels or erase owner-drawn button
  backgrounds separately.
- page controls share one settings draft. entering audio settings requests
  endpoint discovery off the ui thread; results refresh the existing selectors
  without rebuilding the page or losing draft edits.
- invalid visible numeric input blocks publication; invalid hidden fields use
  their saved values. only a successful save of changed normalized settings
  advances the baseline and queues a recorder apply. an unchanged normalized
  draft needs no disk write or recorder action. the diagnostics logging toggle
  commits only its own field and is blocked while the recorder is busy.
- the session worker runs pre-capture preparation, initializes and shuts down
  the controller, and serializes recorder actions and integration changes.
  hotkeys are registered from `registeredHotkeys`, which unbinds the replay
  chord while replay is disabled, so its key stays usable elsewhere.
  navigation stays available during startup; recorder actions and settings
  writes are blocked while busy. ignore control commands during page rebuild
  or close, and activation requests during close. accept voice replay only
  from an enabled, ready listener outside startup, settings apply, and close.
- `WM_QUERYENDSESSION` allows shutdown; `WM_ENDSESSION(TRUE)` finalizes an
  active recording and joins every worker synchronously through `shutdown`,
  because nothing runs after that message. closing to the tray only hides the
  window, so it neither prompts nor saves the draft; an exit from the tray
  restores the window before the unsaved-changes prompt.
- normal exit stops accepting new work and keeps dispatching messages until
  all workers finish, including the gallery worker. accepted recorder actions
  (including startup) and queued gallery edits drain before destruction.
  direct destruction through `DestroyWindow` still joins workers synchronously
  in `WM_DESTROY`. workers post notifications without taking ownership of the
  window handle.
- publish recorder action completion before probing recovery. show recovery
  only when an eligible session is known and the recorder is idle.
- the library is gallery-only and leaves the legacy view setting untouched. it
  browses one disk folder at a time through `ClipManager::listContents`: the
  gallery shows the subfolders of the open folder and the clips directly inside
  it. the gallery draws thin folder rows above the clip grid, one video tile wide
  and scrolled with it: a drawn folder glyph, the folder name, and the number of
  clips below it at any depth, one row per subfolder, alphabetical. rows are
  recycled owner-drawn buttons in their own pool, drawn like the clip tiles and
  clicked like them: the folder name is the button text, the count is painted,
  and the row opens through the ordinary button click. never move a folder row
  back to view-level hit testing; that path did not receive clicks.
  the open folder is the top of `Gallery::stack`; clicking a row pushes it,
  **Back** (left of Refresh, hidden at the root with its slot reserved) pops
  it, `VK_BACK` does the same, and clicking the Library tab while on the library
  returns to the root.
  building the library page also drops the stack, so the tab never resumes a
  folder. each level change clears the selection, the scroll and the thumbnail
  window, then rescans through the worker. the viewport slides in over 200ms from
  the side the navigation came from, through the `librarySlide` motion slot, so
  it honours the windows animation preference. page-wide pointer input (the wheel
  and the back side mouse button) goes through `libraryInput` and is called from
  the viewport, every control, the body and the window: the body's own handlers
  ran scroll math on every page and swallowed those messages, so only input over
  a tile reached the gallery. add any new page-wide message there, not per child.
  the toolbar carries six actions, which is what the minimum window width leaves
  room for.
  its worker runs scans, edits, and thumbnail decoding. the viewport recycles
  named native tile buttons and limits preview work to visible rows plus one
  overscan row, capped at 48. cancel superseded scans and queued previews,
  discard stale results, and avoid requeuing ready or in-flight previews.
  renaming replaces the selected tile's name line with one edit field rather
  than adding a row: the field is a child of the gallery viewport so it scrolls
  and clips with its tile, and it is raised above the recycled tiles on every
  placement. Enter in the field commits, Escape cancels, and a selection change
  cancels with it. the field owns its keys, so Delete and Ctrl+R are text there.
  the clips rename rules check the name on the ui thread before the edit is
  queued; a rejected or colliding name keeps the field open with its text and a
  specific status. a successful rename patches the tile in place, no rescan.
  delete moves the clip to the Recycle Bin, then selects and focuses the nearest
  remaining tile. Refresh stays enabled without a selection.
  thumbnails are cached by path and revalidated against size and modified time,
  so a rescan that shifts indexes keeps them. they decode at the tile width
  rounded up to 160px steps and re-decode only when that step grows.

## Verification

see [`StudioWindowTests`](../../tests/StudioWindowTests.cpp) for automated
coverage. manually check live edge resizing, rapid scrolling, title-bar drag
and snap, keyboard traversal, mixed-dpi layouts, real thumbnails and drag/drop,
window placement across two monitors and after unplugging one, closing during a
recording, a Windows shutdown during a recording, and capture with real devices. check rapid tab reversals, hide or minimize
mid-transition, and toggle windows animation effects while open.

## Child DOX Index

none.
