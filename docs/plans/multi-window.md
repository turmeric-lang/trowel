# Multiple windows — plan

> **Status:** Complete. All seven milestones landed — window registry,
> New/Close Window, open routing, drag-and-drop, per-window persistence,
> Window menu, and documentation. See the inline ✅ markers on each
> milestone below for deviations and bugs fixed along the way.
> **Related:** [`repl-working-directory.md`](repl-working-directory.md)
> (complete, composes with per-window REPL).

Trowel is currently a single-`MainWindow` app: one window owns the tab
set (`buffers_`), the REPL, the splitter, and is the fixed target of the
control socket. This plan adds real multi-window support, modeled
loosely on VSCode.

Four behaviors drive the design:

- **(a)** If no window is open and a file is opened → open it in a new
  window.
- **(b)** If a window is open and a file is opened → open it in a new
  tab in that window.
- **(c)** The user can open a new blank window.
- **(d)** A file dropped onto a window replaces the current tab (or
  creates one if the window somehow has none).

## What already works

Worth stating plainly, because it shrinks the job considerably:

- **Opening already fans into tabs.** `MainWindow::openPath()` reuses an
  empty, unmodified Untitled buffer and otherwise appends a tab. So (b)
  already holds *within* a window — the work is routing, not tabbing.
- **(d) is most of the way there.** `replaceBufferWithFile(index, path)`
  swaps a tab's content in place and is already used by `DirectoryView`
  activation. Drop handling can call it directly. **But it does not
  prompt to save** — dropping onto a modified tab would silently discard
  edits. That guard is net-new and is the one real bug risk in (d).
- **The REPL is already per-window by construction.** Each `MainWindow`
  ctor builds its own `ReplSession`. N windows means N independent
  `tur repl` processes — which is the behavior we want (see
  "REPL working directory" below), just at N shells of cost.

## What blocks it today

- **One window pointer, everywhere.** `TrowelApplication` (macOS
  `QEvent::FileOpen`), the single-instance forwarder (Linux), and
  `ControlServer` each hold a single `MainWindow*`. `Dispatch()` takes
  one window.
- **Restore happens in the constructor.** `MainWindow`'s ctor calls
  `restoreState()`, which reopens persisted buffers. A *new* window must
  come up blank, so construction and restore have to be separated.
- **Persistence is singular.** The `QSettings` keys `geometry`,
  `splitterState`, `openBuffers`, `activeBuffer`, and `replVisible` all
  describe exactly one window.
- **`quitOnLastWindowClosed` is default-true**, so closing the window
  quits the app. Requirement (a) can never trigger until that changes on
  macOS.

## Decisions

Settled up front; the rest of the plan assumes these.

- **Lifecycle.** `setQuitOnLastWindowClosed(false)` on macOS only, so
  the app survives with zero windows and a reopen makes a fresh one
  (this is what enables (a)). Linux/Windows keep quitting on last close
  — cleaner interaction with the single-instance socket, which assumes
  "process alive ⇒ window available".
- **Session restore.** Reopen *every* window from the last session, with
  its tabs and geometry. Requires the per-window settings array below.
- **Duplicate files.** Re-opening a file already open in the target
  window focuses that tab instead of adding a second one. Cross-window
  duplicates stay allowed — no global search, no window-raising
  surprises.
- **Last tab close.** Unchanged from today: `Ctrl/Cmd+W` on the last tab
  leaves a fresh Untitled buffer and the window stays. Windows close via
  `Ctrl/Cmd+Shift+W` or the window controls.

## Architecture: a window registry

Introduce a small registry — either on `TrowelApplication` or a
`WindowManager` it owns — holding `QList<MainWindow*>` plus a
most-recently-activated pointer, and exposing:

```cpp
MainWindow* activeWindow();       // last-activated, or nullptr
MainWindow* newWindow();          // blank window, one Untitled buffer
MainWindow* activeOrNewWindow();  // the (a)/(b) routing primitive
```

Track "last activated" ourselves rather than calling
`QApplication::activeWindow()`, which returns null whenever the app is
backgrounded — precisely the case when a forwarded CLI open or a
LaunchServices `FileOpen` arrives. Windows register on construction and
deregister on close.

With that in place the four behaviors are one-liners at the routing
points:

| Req | Behavior              | Implementation                                     |
|-----|-----------------------|----------------------------------------------------|
| (a) | no window → new window| `activeOrNewWindow()->openPath(f)`                 |
| (b) | window → new tab      | same call; `openPath` already appends a tab        |
| (c) | blank new window      | `newWindow()` from File ▸ New Window               |
| (d) | drop replaces tab     | `maybeSaveBuffer` → `replaceBufferWithFile`        |

Note that (a) and (b) collapse into the *same call*. The branch lives
entirely inside `activeOrNewWindow()`, which is the point of the
abstraction.

## Open routing

Three entry points must funnel through `activeOrNewWindow()`:

1. **macOS** — `TrowelApplication::openFile()`, driven by
   `QEvent::FileOpen` from LaunchServices, Finder, or the `trowel` CLI
   shim's `open -b`.
2. **Linux** — the single-instance forwarder, which currently sends
   `editor.open` per file plus a `window.activate`.
3. **Control socket** — `editor.open`.

Because the forwarder and the control socket share `editor.open`, fixing
the handler's target resolution covers both. `Dispatch()` keeps taking a
concrete `MainWindow*`; only the resolution of *which* window changes.

The **focus-existing-tab** rule lands inside `openPath()`: compare
absolute paths against open editor tabs in that window and activate a
match instead of appending.

## Drag and drop

No drop handling exists today (`grep` finds no `setAcceptDrops` or
`dropEvent` anywhere in `src/`), so this is entirely net-new:

- `setAcceptDrops(true)` on `MainWindow`.
- `dragEnterEvent` accepts `text/uri-list` payloads containing local
  file URLs.
- `dropEvent`:
  1. `maybeSaveBuffer(activeIndex_)` — the missing guard. Bail if the
     user cancels.
  2. Replace the active tab with the first dropped path via
     `replaceBufferWithFile()`; a dropped directory swaps in a
     `DirectoryView` the way `openDirectory()` does.
  3. Any remaining dropped files open as new tabs.
  4. `raise()` + `activateWindow()`.

**Divergence from VSCode, on purpose:** VSCode opens a drop as a *new*
tab. The spec here is replace-current, which is why step 1 matters —
without the save prompt, replace is destructive.

## Persistence

Replace the singular keys with a `windows` array, each entry carrying
what one window needs:

```
windows/<n>/geometry
windows/<n>/splitterState
windows/<n>/openBuffers      # same dir:// encoding as today
windows/<n>/activeBuffer
windows/<n>/replVisible
focusedWindow                # index into the array
```

Written with `QSettings::beginWriteArray`, which lands in the INI as a
`[windows]` section keyed `<1-based index>\<key>`. The array is cleared
before each write — `beginWriteArray` only truncates, so a shrinking
window set would otherwise leave a stale trailing entry.

`recentFiles` and `editorFont` stay top-level: they are app-wide, not
per-window.

Migration: when `windows` is absent, synthesize a single entry from the
existing top-level keys (which themselves already migrate the older
`lastFile`), then drop them.

Persist the whole set at app quit rather than per-window, so two windows
can't clobber each other's keys. A window closing mid-session removes
its entry and rewrites, so a crash still leaves the most recent good
state.

## REPL working directory

No change, and that is a deliberate choice worth recording.

`replWorkingDir()` returns the active file's directory, or `$HOME` when
there is no file, and is consulted only at REPL start and restart. Carry
that unchanged into multi-window and the two things users actually want
fall out of how the window was born:

- **Window opened onto a file** → REPL roots in that file's directory —
  "follows the project".
- **Blank New Window** → no file → `$HOME` → a scratch REPL.

So the determining question is "did this window open onto a file?",
which is already in the user's head. No mode, no toggle, no new concept.

The cwd deliberately does **not** chase tab switches: a REPL that
silently `cd`s when you click another tab would break relative paths and
in-flight work. **Restart REPL** is the explicit, already-existing
escape hatch, and it re-roots to the current active file's directory.

Further control over the REPL's cwd is its own plan — see
[`repl-working-directory.md`](repl-working-directory.md). It has no
dependency on this work and can land before or after.

## Milestones

0. **Settings isolation (prerequisite).** ✅ Done. The smoke suite could
   not sandbox `QSettings` on macOS: it sets `$HOME`, but QSettings
   resolves through `cfprefsd`, which keys off the real uid. Tests were
   therefore reading *and rewriting* the developer's real preferences —
   four tests failed on a clean tree because the restored session leaked
   in, and every run clobbered the saved buffer list. `TROWEL_SETTINGS_DIR`
   (honored in `main.cpp`, set by `conftest.py`) pins settings to a
   per-test INI file. Needed before milestone 1, whose whole claim is
   "restore behaves identically", and before milestone 5, which changes
   the settings schema.
1. **Registry + refactor.** ✅ Done. Window registry, construction/restore
   split so a bare `MainWindow` comes up blank, `ControlServer` repointed
   at the registry. No user-visible change — the load-bearing refactor
   everything else sits on, provable by the existing smoke suite passing
   untouched (46 passed, 2 skipped, unchanged).

   One deliberate behavior *fix* rode along: the REPL is now started
   after restore and after command-line files are opened, so it roots at
   what the window actually shows. Previously it started mid-construction,
   so `trowel ~/proj/foo.tur` rooted the REPL at the *restored session's*
   directory (or `$HOME`), never at `foo.tur`'s — contradicting the rule
   documented above. Verified against both binaries.
2. **New Window.** ✅ Done. File ▸ New Window (`Ctrl/Cmd+Shift+N`) and
   File ▸ Close Window (`Ctrl/Cmd+Shift+W`). Quit now goes through
   `closeAllWindows()` rather than closing one window, so it means the
   app and still honors a per-window unsaved-changes cancel.

   The `window.new` / `window.list` control commands were pulled forward
   from milestone 6, since without them the milestone could not be
   tested; `tests/smoke/test_multi_window.py` covers it. The Window menu
   landed in milestone 6.

   ~~**Interim wart:** every window's `closeEvent` writes the same
   singular settings keys, so with two windows open the last one closed
   wins and the next launch restores only that one.~~ Fixed in
   milestone 5.
3. **Open routing.** ✅ Done. All three entry points were already routed
   in milestone 1, so this milestone was the rest of the rule set:
   focus-existing-tab in `openPath()`, and the macOS lifecycle needed to
   make (a) reachable at all — `setQuitOnLastWindowClosed(false)` plus a
   `QEvent::ApplicationActivate` handler so a dock click on a windowless
   app gets a window back. Both pulled forward from milestone 5.

   Two consequences that needed handling:

   - **Quit stopped quitting.** With the last-window rule disabled,
     `closeAllWindows()` no longer ends the process, so Quit routes
     through `MainWindow::quitApp()`, which closes every window and then
     asks explicitly — but only if all of them actually agreed to go, so
     an unsaved-changes cancel still aborts the quit.
   - **`Dispatch()` now takes the registry, not a window,** and picks a
     target per command. Previously every request resolved through
     `activeOrNewWindow()`, which meant asking "how many windows are
     open?" *created* one, and a query like `editor.get_text` would
     silently conjure a blank window to answer. Now opening a document
     creates a window (that *is* rule (a)); everything else reports
     `no_window`. Registry-level commands (`ping`, `window.new`,
     `window.list`) resolve no window at all.

   `window.list` also reports `tabs` / `tab_count`, without which the
   focus-existing-tab rule is not observable — activating a duplicate
   tab and focusing the existing one look identical from `editor.get_text`.
4. **Drag and drop.** ✅ Done. `setAcceptDrops(true)` plus
   `dragEnterEvent`/`dropEvent` on `MainWindow`; the logic lives in
   `openDropped()` so it is testable independently. The save guard is in
   place — replacing a modified tab prompts first, and a cancel aborts
   the drop.

   Two rules compose here in a way the plan did not spell out: a drop of
   a file **already open in that window** focuses its tab rather than
   replacing the current one, because replacing would leave two tabs on
   one file. Replace still applies for anything not already open.

   `replaceBufferWithDirectory()` was factored out so a dropped directory
   swaps the tab to a `DirectoryView`; `openDirectory()`'s duplicated
   in-place swap now calls it.

   The `window.drop` control command synthesizes a real drag/drop against
   the window, so the smoke tests exercise the actual event handlers
   rather than just `openDropped()`.

   **Crash fixed along the way.** Reaching a directory tab and then
   running any `editor.*` command killed the process:
   `MainWindow::editorView()` is null for a non-editor tab and the
   control handlers dereferenced it unguarded. Pre-existing (Open
   Directory reaches the same state), but drag-and-drop makes it far
   easier to hit. Added `RequireEditor()`, which replies `no_editor`;
   all 15 affected commands now error cleanly.
5. **Persistence + lifecycle.** ✅ Done. The lifecycle half landed early
   in milestone 3, so this was the per-window settings array:
   `MainWindow::sessionState()` / `applySessionState()` for one window's
   slice, `WindowManager::persistAll()` / `restoreAll()` for the array,
   and migration from the legacy singular keys (including `lastFile`),
   which are removed once the array is authoritative.

   **Close and quit need opposite persistence.** Closing a window drops
   it from the saved session; quitting must keep every window that was
   open. But quit *is* a series of closes, so the naive version erases
   the session window by window until nothing is left to restore.
   `quitApp()` therefore snapshots the full set up front and sets a
   `quitting` flag that suppresses the per-close rewrite; if a window
   refuses to close, the flag clears and the set is resynced to what
   actually survived.

   **Recent files had to become a merge.** Each window carries its own
   `recentFiles_`, so the plain last-writer-wins write let whichever
   window closed last discard everything the other windows had opened —
   observed directly in the written INI. `persistGlobals()` now merges
   against what is already stored.
6. **Tests + Window menu.** ✅ Done. The control commands and most smoke
   coverage were pulled forward into milestones 2–5, so this milestone
   was the Window menu deferred from milestone 2, plus the
   single-instance test update.

   The **Window menu** lists every open window, checkmarks the current
   one, and switches on click. It relists on `WindowManager::
   windowsChanged`, which fires when a window opens or closes **and when
   a title changes** — the latter turned out to be load-bearing: a window
   is titled "Trowel" at construction and only becomes "foo.tur —
   Trowel" once a file loads, so without the title notification every
   menu kept the stale placeholder. `aboutToShow` alone was not enough,
   since it only fires for a human opening the menu, not for
   `menu.invoke`.

   The **single-instance tests** now assert the route half of the rule:
   forwarded files land as tabs in the window that is already open
   (`window.list` stays at 1), and forwarding an already-open file
   focuses its tab instead of adding one. These are Linux-only and skip
   on macOS, so they are **unverified on the development machine** — the
   routing they exercise (`editor.open` → `activeOrNewWindow()`) is
   covered on macOS by `test_multi_window.py`, but the forwarding path
   itself needs a Linux run or CI.
7. **Documentation.** ✅ Done.
   [`docs/guides/windows-tabs-and-repl.md`](../guides/windows-tabs-and-repl.md)
   states the behavior as rules rather than implementation: where a file
   opens (including the deliberate drag-and-drop divergence from VS
   Code), the tab/window key map, focus-existing-tab, session restore,
   the macOS lifecycle, and the REPL cwd rule — including that Restart
   REPL re-roots and clears state.

   Linked from the README under a new "Using Trowel" section (there was
   no home for user-facing guides before), and `keyboard-shortcuts.md`
   picked up the new bindings, which it was missing. CHANGELOG gained an
   `Unreleased` section covering milestones 1–6, including the three
   bugs fixed along the way.

   The guide's claims were checked against a running build rather than
   written from the code: Untitled buffers are not restored, launching
   with a file skips restore, File ▸ New adds rather than replaces. The
   one claim not driven end-to-end is per-window REPL-pane visibility —
   the toggle is a toolbar action with no menu path, so `menu.invoke`
   cannot reach it; the key is confirmed present per window in the
   written INI.

## Corner cases

- **Zero windows on macOS.** `activeOrNewWindow()` must mint a window,
  or control commands like `editor.open` hit a null `editorView()`.
- **Modified tab + drop** → must prompt; see above.
- **Multiple files dropped** → first replaces, rest become tabs.
- **Quit with unsaved work across several windows** → each window's
  `closeEvent` already runs `maybeSaveAll()`; cancelling in any window
  must abort the whole quit, not leave a half-closed set.

## Non-goals

- Moving or dragging tabs between windows.
- Split editors within a window.
- A shared REPL across windows.
- Cross-window "focus the tab that already has this file open".
- Per-window themes or fonts (font stays a global `QSettings` value).
- Windows-platform lifecycle tuning beyond "quit on last close".

## Future

- Drag a tab out to spawn a window (natural once drop handling exists).
- Window menu with numbered shortcuts.
- Per-window session restore including cursor position and scroll.
- Remembering which display/space a window was on.
