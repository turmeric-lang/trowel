# Tab bar — plan

> **Status:** Complete. `TabBar` (`src/app/tab_bar.{h,cpp}`) and the
> multi-buffer model (`Buffer` struct, `QStackedWidget`) are live.
> `setClosable(int, bool)` was added per-index (not as a whole-bar flag)
> to support the non-closable REPL-pane tabs in
> [`debugger-support.md`](debugger-support.md).

Add a horizontal tab strip below the button bar and above the
editor/REPL splitter. Each tab represents one open buffer (file or
scratch). `File > Open` opens an **additional** buffer instead of
replacing the current one. For v1 the strip is always visible, even
with a single tab.

## Layout

```
+---------------------------------------------------+
| button bar (toolbar, ~32 px)                      |
+---------------------------------------------------+
| tab bar (~24 px)                                  |
+---------------------------------------------------+
|                        |                          |
| editor                 | REPL                     |
|                        |                          |
+---------------------------------------------------+
```

- The tab strip is a plain `QWidget` (call it `TabBar`) placed above
  the splitter. `MainWindow`'s central widget becomes a vertical
  container (`QWidget` + `QVBoxLayout`, zero margins/spacing) holding
  the tab bar and the existing `QSplitter`.
- Height: fixed. Compute from the widget's font metrics —
  `fontMetrics().height() + 2 * vpad` where `vpad ≈ 4 px`. Set with
  `setFixedHeight(...)` at construction and after font changes. Do
  **not** hardcode 24 px; let it grow with the font.
- Background + border-bottom match the button bar so the two strips
  read as one chrome block:
    - Background: `Theme::editorBg`.
    - 1 px bottom border: `Theme::lineNumberFg` (the same divider
      color the toolbar uses).
- Always visible for v1 — no auto-hide when there's a single tab.

## Tab appearance

Each tab is a rectangle laid out left-to-right:

- Text: buffer display name, centered horizontally and vertically in
  the tab's available area. Painted in `Theme::editorFg`; the active
  tab uses the same color but bold (or a slightly brighter fg —
  decide at implementation time, keep it a one-liner to flip).
- Modified marker: append `" •"` to the display name when the
  buffer's editor reports `isModified()`.
- Width: `fontMetrics().horizontalAdvance(displayName) + hpad*2 +
  closeSlotWidth`. `hpad ≈ 12 px`. `closeSlotWidth ≈ 20 px` so the
  close glyph, when shown, has room without shifting the label.
    - Optional cap: `maxTabWidth ≈ 240 px`, elide with `Qt::ElideMiddle`.
- Divider between tabs: a 1 px vertical line in `Theme::lineNumberFg`,
  4 px top/bottom margin — the same treatment as `QToolBar::separator`
  in the button bar stylesheet.
- Active tab: no accent bar for v1. Distinguish via bold text (see
  above). Room to add a 2 px top or bottom accent later.
- Hover: the close glyph — Unicode multiplication sign `×`
  (`U+00D7`) — appears on the right side of the tab, vertically
  centered, with ~8 px right padding. Not shown on non-hovered tabs.
  Painted in `Theme::lineNumberFg`; brightens to `Theme::editorFg`
  when the cursor is specifically over the `×` glyph's hit rect.
- No icons, no drag-reorder, no context menu in v1.

## Display name rules

- File-backed buffer → `QFileInfo(path).fileName()`.
- Untitled buffer → `"Untitled"`. If multiple, disambiguate with a
  running counter: `"Untitled"`, `"Untitled 2"`, …
- Duplicate basenames across different directories → for v1 keep the
  bare filename in the tab and rely on the tooltip
  (`setToolTip(fullPath)`) to disambiguate. Revisit if it bites.

## Interaction

- **Left click** on a tab body → activate that buffer.
- **Left click** on the `×` glyph → close that buffer (route through
  the same `maybeSave()` prompt the window close uses).
- **Middle click** anywhere on a tab → close (nice-to-have; keep only
  if trivial).
- **Hover** → repaint that tab to show the `×`.
- Keyboard: `Ctrl+Tab` / `Ctrl+Shift+Tab` cycle buffers,
  `Ctrl+W` closes the current one. Bind as `QAction`s on `MainWindow`
  so shortcuts work regardless of focus.
- Closing the **last** tab → open a fresh Untitled buffer rather than
  leaving the editor blank/undefined. Same behavior as `newFile()`.

## Model: buffers, not just editors

Right now `MainWindow` owns a single `EditorView*`. Multi-buffer needs
a small model shift.

- Introduce a `Buffer` struct held in a `std::vector<std::unique_ptr<Buffer>>`
  on `MainWindow`:

  ```cpp
  struct Buffer {
      EditorView* view;        // owned by the QStackedWidget
      QString displayName;     // cached, recomputed on path change
      int untitledIndex = 0;   // 0 for named; >=1 for "Untitled N"
  };
  ```

- Replace `editor_` (single view) with a `QStackedWidget* editorStack_`
  as the left pane of the splitter. Each `Buffer` owns one
  `EditorView` inside the stack; switching the active tab calls
  `editorStack_->setCurrentWidget(buf.view)`.
- `EditorView* currentEditor()` returns the active buffer's view.
  Every existing call site that touches `editor_` becomes a call to
  `currentEditor()` — audit is small (~a dozen sites in `main_window.cpp`).
- Signals from each `EditorView` (`modifiedChanged`, `filePathChanged`)
  connect to a `MainWindow` slot that updates the corresponding tab's
  display name + repaints the tab bar. Use `sender()` or a lambda
  capture to identify which buffer.
- `RunBuffer` / `RunRange` take `currentEditor()` as before — the
  REPL is shared across buffers.

### File > Open changes

- Today: `openPath()` calls `editor_->loadFile(path)` in place.
- New behavior: if the current buffer is **untitled and empty and
  unmodified**, reuse it (avoids leaving a stale "Untitled" behind on
  first open from a fresh window). Otherwise, create a new `Buffer`
  with a fresh `EditorView`, `loadFile(path)`, append to the vector,
  add the tab, activate it.
- `File > New` always creates a new Untitled buffer + tab; never
  replaces the current one.
- The "was there anything to save?" prompt moves from the outer
  `openPath` / `newFile` flow to the per-buffer close flow.

## Persistence

- On quit: persist the ordered list of file paths + which one was
  active. Skip untitled buffers (nothing to reference).
    - `QSettings settings; settings.setValue("openBuffers", QStringList{...});
      settings.setValue("activeBuffer", index);`
- On startup: restore the list, opening each path. Missing files are
  dropped with a status-bar warning (same pattern as the recent-files
  menu).
- Backwards-compat: if a session was saved with the old single-buffer
  `lastFile` key, migrate on first launch by treating `lastFile` as a
  one-element `openBuffers` list, then remove the old key.

## Painting

Custom-drawn — subclass `QWidget`, not `QTabBar`. `QTabBar` fights
back against the centered-text + hover-close + separator styling we
want; a plain widget is less code overall.

- Override `paintEvent`, `mouseMoveEvent`, `mousePressEvent`,
  `leaveEvent`.
- Track `hoveredTabIndex_` (-1 when none) and `hoverCloseHit_` (bool)
  in state; both are set from `mouseMoveEvent` and cleared on
  `leaveEvent`.
- Cache per-tab rects in a `std::vector<TabGeom>` recomputed on
  layout changes (buffer added/removed/renamed, font changed, resize).
- Use `QPainter::drawText` with `Qt::AlignCenter` in the text rect,
  reserving the right-hand close slot from the geometry so the label
  stays truly centered in "text area" — not shifted by the invisible
  close glyph.

Enough logic that it deserves its own file: `src/app/tab_bar.{h,cpp}`.

## File layout

```
src/
├── app/
│   ├── tab_bar.{h,cpp}          # NEW: custom tab-strip widget
│   ├── main_window.{h,cpp}      # multi-buffer model + wiring
```

Public surface of `TabBar`:

```cpp
class TabBar : public QWidget {
    Q_OBJECT
public:
    explicit TabBar(QWidget* parent = nullptr);

    void setTabs(const QStringList& displayNames, int activeIndex);
    void setActive(int index);
    void setModified(int index, bool modified);

signals:
    void activateRequested(int index);
    void closeRequested(int index);

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    // ...
};
```

`MainWindow` owns the `TabBar` and translates its two signals into
the buffer-model operations described above.

## Theming

- `TabBar` takes a `Theme` (or the specific colors it needs) via a
  setter so it can be reskinned alongside the rest of the UI. Same
  colors as the toolbar for consistency:
    - background `theme.editorBg`
    - divider / border `theme.lineNumberFg`
    - text `theme.editorFg`
- No stylesheet — everything is drawn in `paintEvent`, so the palette
  is passed in explicitly.

## Milestones

1. **Model shift** — `Buffer` struct, `QStackedWidget` in the
   splitter, `currentEditor()` accessor. No visible tab bar yet; the
   app behaves exactly as before, with a single Untitled buffer in
   the stack. Prove the plumbing without UI risk.
2. **`TabBar` widget shell** — custom widget, background + border,
   fixed height from font metrics. Always visible. Empty of tabs.
3. **Static tabs** — render the current buffer list, centered text,
   dividers between tabs. Active tab in bold. No interaction.
4. **Activation + File > Open** — clicking a tab switches the stack;
   `File > Open` appends a buffer + tab; the untitled-empty-unmodified
   reuse rule. `File > New` always appends.
5. **Hover close** — repaint on hover, draw `×` with padding, click
   closes through `maybeSave()`. Auto-open Untitled when closing the
   last tab.
6. **Keyboard** — `Ctrl+Tab`, `Ctrl+Shift+Tab`, `Ctrl+W`.
7. **Persistence** — save/restore the buffer list across launches;
   migrate the old `lastFile` key.
8. **Polish** — tooltips (full path), disambiguation with counters,
   maxTabWidth + eliding, tune padding on a real display.

## Non-goals for v1

- Drag-to-reorder tabs.
- Right-click context menu (Close, Close Others, Copy Path…).
- Split editors / multiple tab strips.
- Pinned tabs.
- Overflow chevron / scroll-when-too-many — for v1 tabs shrink via
  eliding; if the strip runs out of room, just clip. Real overflow
  is a follow-up.
- Auto-hide when a single tab is open.
- Per-tab dirty dot instead of the trailing `" •"`.

## Future

- Overflow: horizontal scroll or an overflow menu when tabs exceed
  the strip width.
- Drag-reorder, plus drag-out to split.
- Right-click menu.
- Session save/restore including cursor position and scroll per
  buffer.
- Per-tab language indicator (small glyph left of the label) once
  more than one language is supported.
