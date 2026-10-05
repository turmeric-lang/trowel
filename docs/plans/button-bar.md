# Button bar — plan

> **Status:** Complete. The toolbar is implemented (`setupToolBar()` in
> `main_window.cpp`) with `NerdIcon()` from `src/app/icon_font.{h,cpp}`.
> Grew beyond the two planned actions to include Debug, Restart REPL,
> Clear, Format, Outline, and Toggle Split.

Add a thin toolbar spanning above the editor + REPL split, holding
icon-only actions. First two: **Evaluate File** and **Evaluate Selection**.
Room to grow — restart REPL, save, stop, split-orientation, etc.

## Layout

- New `QToolBar` docked at `Qt::TopToolBarArea` on `MainWindow`.
- Height: **32 px** — set `setIconSize(QSize(18, 18))` and rely on toolbar
  margins to reach 32 px total. Verify on a real display; tune padding via
  a stylesheet on the toolbar if needed.
- `setMovable(false)`, `setFloatable(false)` — this is a fixed chrome
  strip, not a rearrangeable palette.
- Spans the entire window width (default `QToolBar` behavior), which
  visually covers both panes of the splitter.
- No text: `setToolButtonStyle(Qt::ToolButtonIconOnly)`.
- Tooltip = the human label ("Evaluate File", "Evaluate Selection", …).
  Qt shows these automatically when `QAction::setToolTip` is set (or
  falls back to `text()` if not).

## Icon source: Nerd Fonts

Two viable formats. Recommended in order.

### A. Bundle "Symbols Only Nerd Font" (recommended)

A ~500 KB TTF containing just the icon glyphs — no Latin, no ligatures.
Distributed as `SymbolsNerdFont-Regular.ttf` on the Nerd Fonts release
page.

- Vendor the TTF at `resources/fonts/SymbolsNerdFont-Regular.ttf`, add it
  to `resources/resources.qrc` under a `/fonts` prefix.
- On app startup (main or a small `icon_font.cpp`), call
  `QFontDatabase::addApplicationFontFromData(...)` reading the resource.
  Cache the resolved family name (usually `"Symbols Nerd Font"`).
- Helper: `QIcon nerdIcon(char32_t codepoint, QSize px)`:
    - Rasterise the glyph into a `QPixmap` via `QPainter::drawText` using
      the Nerd Font family at the requested pixel size, `Qt::white` (or
      current theme fg) as the pen, transparent background.
    - Wrap the pixmap in a `QIcon`. For proper focused/disabled variants,
      generate `QIcon::Normal` + `QIcon::Disabled` at construction.
    - Cache by `(codepoint, size, color)` in a `QHash` so we're not
      re-rasterising on every repaint.
- Trade-off: costs ~500 KB in the binary, but adding a new toolbar
  action is a one-line `nerdIcon(0xF040A)` — scales trivially.

### B. Per-icon SVGs

Nerd Fonts publishes each glyph as an SVG under
`patched-fonts/svgs/` in the upstream repo (or generatable via
FontForge). Copy the two we need into `resources/icons/` and load via
`QIcon(":/icons/play.svg")`.

- Cleaner if we only ever want ~10 icons.
- More friction to add each new icon (find the SVG, copy, register in
  qrc).
- No dependency on rasterising via QPainter — Qt's SVG renderer handles
  DPR scaling for free.

**Decision:** start with **A** unless the ~500 KB weight is objectionable.
It matches the "use Nerd Fonts" ask literally and keeps future actions
one line each.

## Actions (v1 of the bar)

Codepoints to be confirmed at implementation time from
<https://www.nerdfonts.com/cheat-sheet> — the names below are canonical.

| Action              | Nerd Font glyph          | Handler                          |
|---------------------|--------------------------|----------------------------------|
| Evaluate File       | `nf-md-play` (≈U+F040A)  | `MainWindow::runBuffer` (⌘R)     |
| Evaluate Selection  | `nf-md-playlist_play`    | `MainWindow::runSelection` (⌘⇧E) |

- Buttons reuse the exact `QAction*`s the menu already binds — no
  duplicated handlers, no drift between menu and toolbar.
- If the selection is empty, "Evaluate Selection" should still be
  enabled and produce the "Empty selection." status message (already the
  behavior in `run_buffer.cpp`). Later: subscribe to Scintilla's
  selection-changed signal and disable the action when no selection.

## File layout

```
resources/
├── fonts/
│   └── SymbolsNerdFont-Regular.ttf   # vendored, ~500 KB
├── resources.qrc                       # add <file prefix="/fonts">...</file>
src/
├── app/
│   ├── icon_font.{h,cpp}              # NEW: font registration + nerdIcon() cache
│   ├── main_window.{h,cpp}            # add setupToolBar(), member QToolBar* toolbar_
```

`icon_font.h` — minimal surface:

```cpp
namespace trowel {
void RegisterNerdFont();                    // call once at startup
QIcon NerdIcon(char32_t cp, int pixelSize);  // cached
}
```

## Wiring into MainWindow

- In `MainWindow::MainWindow`, after `setupUi()` and before
  `setupMenus()`, call a new `setupToolBar()`.
- `setupToolBar()`:
    - `auto* tb = addToolBar("Main");`
    - `tb->setMovable(false); tb->setFloatable(false);`
    - `tb->setIconSize({18, 18});`
    - `tb->setToolButtonStyle(Qt::ToolButtonIconOnly);`
    - Create actions (or lift the ones already created in `setupMenus`
      into `MainWindow` members so both menu and toolbar reference them).
    - `tb->addAction(runBufferAction_);`
    - `tb->addAction(runSelectionAction_);`
- Toolbar sits at `TopToolBarArea` above the central widget, which is
  already the `QSplitter` — so it spans both panes visually.

## Theming

- The toolbar picks up the window's palette by default, which we set
  from the theme (see `theme_loader.cpp`). If the icons look off against
  the toolbar background, render them with an explicit color drawn from
  `Theme::editorFg`. Plumb the theme into `RegisterNerdFont` /
  `NerdIcon` on construction rather than reaching for globals.

## Milestones

1. **Vendoring + font loading** — resource entry, `RegisterNerdFont`,
   verify one hardcoded codepoint renders in a scratch `QLabel`.
2. **`NerdIcon` cache + toolbar shell** — empty toolbar attached, 32 px,
   no items.
3. **Two actions wired** — Evaluate File / Evaluate Selection sharing
   the existing `QAction`s from the Run menu. Tooltip text set. ⌘R /
   ⌘⇧E still work.
4. **Polish** — verify visually on a real display, tune toolbar padding
   for exactly 32 px, decide whether "Evaluate Selection" should
   disable itself on empty selection.

## Non-goals for v1

- Movable / floatable toolbar — no.
- User-customizable button set — no.
- Overflow menu when the window is narrow — Qt's default handling is
  fine.
- Text labels — no; icon-only per the ask.
- Keyboard shortcuts on the buttons themselves beyond what the actions
  already carry.

## Future

- Restart REPL button — nf-md-restart / nf-md-refresh.
- Save / Save As — probably not; File menu covers it.
- Toggle split orientation (vertical/horizontal) — nf-md-view-split-horizontal.
- Toggle terminal visibility.
- Show a small "REPL busy" spinner in the toolbar when a Run is in flight.
