# Minimap — plan

> **Status:** Phases 1–3 shipped. `MinimapView` (`src/editor/minimap_view.{h,cpp}`)
> is in the tree, on by default behind `editor.minimap` with a View-menu toggle
> (`Alt+M`) and `editor.minimap` control command. Phase 2 added LRU strip
> eviction (16 strips), a 60 ms render debounce, dirty-range invalidation from
> the modification position, and raised the line cap to 1M. Phase 3 moved strip
> rendering off the GUI thread via `QtConcurrent`: the `StyleSink` refactor
> (`scanner.h`) lets the same scanners paint into a `BufferSink` (off-thread
> byte array) or `DocumentSink` (Scintilla's `IDocument`); `launchRender`
> captures strip text + packed `LexState` on the GUI thread, a
> `QtConcurrent::run` worker drives `ScanLine` per line and produces a `QImage`,
> and a `QFutureWatcher` stores the result and calls `update()`. The document
> cap is removed entirely and the forced `colourise` is gone — the worker seeds
> from `sci_->lineState(firstLine - 1)` and falls back to `LexState{}` past
> `endStyled()`. Word wrap and folding are now on, so the geometry model uses
> display lines (`visibleFromDocLine`/`docLineFromVisible`) from the start
> rather than the identity the plan originally assumed.

A VSCode-style minimap down the side of the editor: a block-rendered, syntax-colored
overview of the whole buffer with a draggable viewport slider. On by default;
toggle with `Alt+M` or View > Minimap.

## Context: what Trowel's editor actually is

`EditorView` (`src/editor/editor_view.h`) is **not** a `QPlainTextEdit` or a custom
`QAbstractScrollArea`. It is a `TabContent`/`QWidget` wrapper around a
**`ScintillaEdit`** (Scintilla 5.5.5, Qt binding, fetched by
`cmake/scintilla_qt.cmake`) held in a `QVBoxLayout` with
`setContentsMargins(0, 4, 0, 0)`. Everything downstream follows from that:

- **The minimap cannot be a Scintilla margin.** Margins render markers/numbers, not
  arbitrary pixels. It has to be a **sibling `QWidget`** next to `sci_` inside
  `EditorView`'s layout, which is also why it composes trivially with the existing
  margins (line numbers 44px, symbol/diagnostics 12px, fold 0px — all internal to
  Scintilla and untouched).
- **Word wrap is off.** `setWrapMode` is never called, so `SC_WRAP_NONE`. Display
  line == document line.
- **Folding is off.** `ScannerLexer::Fold()` is a no-op and the fold margin is 0px
  wide, so no line is ever hidden.

Those last two eliminate the two hardest sync problems a minimap normally has — but
they are *current conditions, not commitments*. Word wrap and folding are both
plausible future features of the editor, and **nothing here may be built in a way that
precludes them.** That is a constraint on everything below, not an aspiration:

- Every doc-line ↔ y conversion goes through `docLineForY`/`yForDocLine`. Enabling
  wrap or folding then means reimplementing two functions over
  `SCI_VISIBLEFROMDOCLINE` / `SCI_DOCLINEFROMVISIBLE`, not rewriting the widget.
  Nothing else may do its own line↔pixel arithmetic.
- Where this plan says "line" in a *geometry* context, read **display line**; where it
  means a position in the buffer, read **document line**. They are equal today and
  will not be later. The strip cache is deliberately keyed on document lines (stable
  under wrap and folding) while the mapping layer deals in display lines.
- Prefer Scintilla APIs that already speak display lines (`SCI_LINESCROLL`,
  `SCI_SETFIRSTVISIBLELINE`, `linesOnScreen()`) over hand-rolled `line * lineHeight`
  arithmetic, which silently bakes in the 1:1 assumption.

"This only works because wrap is off" should read as a defect in review, even while
wrap is off.

The other load-bearing fact: **Trowel's lexer already stores per-line lexer state in
Scintilla's line state** (`PackLexState`/`UnpackLexState` in
`src/editor/lexer_adapter.cpp`, read back via `sci_->lineState(line)`). That gives
random-access resumability into the lexer at any line — the single thing that makes
off-thread minimap rendering tractable. See [Phase 3](#phase-3--off-thread-rendering).

## Goals

- A minimap column showing the whole document, block-rendered with the **real** lexer
  colors, not a heuristic.
- A viewport slider overlay; click to jump, drag to scroll, wheel to scroll the editor.
- Stays responsive on multi-megabyte files: no full-document render, ever.
- Theme-driven colors; a preference to toggle it, choose the side, and set the width.
- Leaves the door open for word wrap and folding: supporting them is out of scope here,
  precluding them is not acceptable.

Non-goals for this iteration: hover-preview magnification, search-hit marks,
git-change gutter, sticky-scroll header, "render actual glyphs" mode, per-tab
minimap state.

## Rendering approach

### Real styles, not a density heuristic

A density render (ink coverage per line, comments dimmed) is cheaper and needs no
lexer, but Trowel's whole visual identity is the syntax colors — rainbow brackets, the
Markdown-hosting-Turmeric-hosting-C nesting, the per-language style blocks in
`lexers.h`. A grey density strip would look like a different product. Use the real
styles.

The theme already resolves style-id → color; it just doesn't expose it. Add to
`theme_loader`:

```cpp
// Style id -> foreground QColor, for consumers that paint text themselves
// (the minimap) rather than handing colors to Scintilla.
QHash<int, QColor> StyleForegroundTable(const Theme& theme);
```

implemented over the existing `StyleKeyMap()` (currently in an anonymous namespace —
expose the *function*, not the map, so the table stays a copy).

### Block-rendered, not tiny glyphs

Render **one filled rect per character cell**: `colW` px wide (default 1) by `lineH` px
tall (default 2), colored by that byte's style, with runs of whitespace skipped. At
1×2 px a real glyph is indistinguishable from a block anyway, and blocks let us fill
whole style runs with a single `QPainter::fillRect` — typically 5–20 fills per line
instead of 80 glyph draws. Tiny-glyph mode is a possible later pref; it is not worth
the font-metrics and shaping cost now.

Cells are painted into a `QImage(Format_RGB32)` at `devicePixelRatio`, so a 1px column
is 2 physical px on Retina and stays crisp. Getting DPR wrong here is the most common
way this feature ships looking blurry — set it on the image and divide in
`drawImage`.

### Geometry model

Two regimes, chosen by whether the document fits:

- **`totalLines * lineH <= widget height`** — 1:1, document top-aligned, minimap does
  not scroll. Slider = the editor viewport.
- **Otherwise** — VSCode's *proportional slide*: `lineH` stays fixed and the minimap
  itself scrolls, so `minimapTopLine` interpolates between 0 and
  `totalLines - widgetLines` as the editor scrolls from top to bottom.

The alternative — compress `lineH` so the entire file always fits — is tempting
("true overview") but makes a line 0.01px at 100k lines, forces aggregation logic,
and re-renders the entire cache whenever the window resizes. Proportional slide keeps
cell geometry constant, which is what makes the cache row-addressable. Take the
trade: at very large files you see a window of the file, not all of it.

`totalLines` in both regimes is a **display**-line count. It equals `lineCount()`
today; under wrap or folding it is `SCI_VISIBLEFROMDOCLINE(lineCount())`. Read it
through one accessor (`displayLineCount()`) rather than calling `lineCount()` at each
site, so there is a single place to change and no site that quietly means the wrong
one. The strip cache is unaffected either way — strips are keyed on document lines.

### Cache: lazily-rendered strips

The cache is a vector of fixed **512-document-line strips**, each a `QImage`:

```cpp
struct Strip { int firstLine; QImage image; bool dirty; };
```

Only strips intersecting the currently visible minimap region are ever rendered.
Under proportional slide that is 2–3 strips at any moment, so both memory and render
cost are bounded by the *widget*, not the *document*. Strips that scroll out are
dropped (or kept in a small LRU, ~16 strips).

This is what makes invalidation cheap, and it resolves an otherwise nasty problem:
because Trowel's lexer carries `LexState` forward across lines, typing `#|` at line 10
can restyle the rest of the file. The minimap therefore has to mark **every strip from
the edited one to EOF** dirty. That is free precisely because dirty strips are never
rendered until someone scrolls to them.

**No cap on document size is needed for rendering.** Phases 1–2 do carry a cap, but
only because of the *style source*, not the render — see below.

### Where the style bytes come from

Two options, and the phasing uses both:

**(A) Ask Scintilla — `SCI_GETSTYLEDTEXT`.** Exact truth, zero duplicated lexing, but
must run on the GUI thread (Scintilla is not thread-safe), returns a 2-byte cell array
(double memory for the range), and — the real problem — styling is **lazy**: Scintilla
only styles up to `endStyled()`. Asking for styles past that point silently returns
zeros unless we first call `sci_->colourise(start, end)`, and colourising from an
unstyled point is O(distance from `endStyled()`). Fine per-strip, catastrophic if
someone naively colourises the whole doc.

**(B) Run the scanners ourselves, off-thread.** For a strip we need only: the strip's
raw text (a `textRange` copy) and the packed `LexState` at its first line, which
`sci_->lineState(firstLine - 1)` hands us as a single `int`. The worker unpacks it with
`UnpackLexState` and drives `ScanLine` per line. No Scintilla access from the worker at
all.

(B) needs one small refactor: `Emitter` writes directly to `Scintilla::IDocument`.
Introduce an abstract sink so the same scanners can paint into a plain byte array:

```cpp
// scanner.h
class StyleSink {
public:
    virtual ~StyleSink() = default;
    virtual void Paint(Sci_Position from, Sci_Position len, int style) = 0;
};
class DocumentSink final : public StyleSink { /* today's IDocument path */ };
class BufferSink   final : public StyleSink { /* std::vector<unsigned char> */ };
```

`Emitter` takes a `StyleSink&`; the backfill/gap logic and every
`ScanLine(...)` signature stay byte-identical, so no scanner file changes and the
existing lexer smoke tests (`tests/smoke/test_lexer_languages.py`,
`test_lexer_theme.py`) cover the refactor.

Caveat to document in code: `lineState(n)` is only meaningful where Scintilla has
already styled line `n`. When `positionFromLine(firstLine) > endStyled()`, seed with
`LexState{}` instead. The consequence is a cosmetically-wrong minimap strip in a
region the user has not visited — acceptable for a 1px overview, and self-correcting
the moment Scintilla styles it (which invalidates the strip anyway).

## Interaction

- **Click** anywhere → center the editor viewport on `docLineForY(y)` via
  `sci_->setFirstVisibleLine(line - linesOnScreen()/2)`. Granularity is one line;
  Scintilla exposes no sub-line vertical scroll, and at 2px/line that is invisible.
  Note the type mismatch waiting here: `SCI_SETFIRSTVISIBLELINE` takes a *display*
  line ("the value is a visible line rather than a document line" — `ScintillaDoc`)
  while `docLineForY` returns a *document* line. They coincide today, so this call and
  the drag handler below need `visibleFromDocLine()` wrapped around the argument the
  day wrap or folding is enabled. `SCI_LINESCROLL` (wheel, below) is already in
  display lines and needs no change.
- **Press inside the slider** → begin drag, remembering the grab offset so the slider
  does not jump under the cursor. `mouseMoveEvent` maps y back to a first-visible-line
  and sets it. `mouseReleaseEvent` ends the drag.
- **Press outside the slider** → jump (as click), then immediately enter drag with the
  slider centered — matches VSCode.
- **Wheel** → forward to the editor: `sci_->send(SCI_LINESCROLL, 0, deltaLines)`, using
  the same `QWheelEvent` pixel/angle-delta handling Scintilla uses, so trackpad
  momentum feels identical over the minimap and over the text.
- **Hover** → slider highlights (`minimapSliderHoverBg`); `leaveEvent` clears it.
- Slider is drawn with `QPainter::CompositionMode_SourceOver` over the cached strips,
  so hover/drag repaints cost one `fillRect`, never a re-render.
- The widget is `Qt::NoFocus`: clicking the minimap must not steal focus from the
  editor. Also exclude it from tab order and leave `EditorView`'s drag-drop target
  behavior on `sci_`.

## Sync with the editor

| Source | Handler | Cost |
|---|---|---|
| `verticalScrolled(int)` / `updateUi(Update::VScroll)` | recompute slider + `minimapTopLine`, `update()` | slider-only repaint, no re-render |
| `modified` (Insert/DeleteText only) | `invalidateLines(firstLine, EOF)` | mark dirty; render deferred |
| `linesAdded(n)` | total line count changed → re-derive mapping | cheap |
| `verticalRangeChanged(max, page)` | viewport height changed → slider size | cheap |
| `resizeEvent` | re-derive mapping; **keep** the cache (strips are line-addressed, only the visible window changes) | cheap |
| `EditorView::installLexer()` (language or rainbow change) | `invalidateAll()` | full dirty |
| theme change / `setFont` | `invalidateAll()` (theme) / slider-only (font — cell geometry is fixed, but `linesOnScreen` moved) | — |

`EditorView`'s existing `modified` lambda already filters to content changes and bumps
`docVersion_`; extend that same lambda to call
`minimap_->invalidateLines(sci_->lineFromPosition(position), -1)`. Render is debounced
by a 60 ms single-shot `QTimer` so a burst of keystrokes yields one repaint.

**Fold/wrap:** both off today, and this table is written so that neither stays that way
by accident. All conversions go through `docLineForY`/`yForDocLine`, which are the
identity over doc lines now; enabling either turns them into
`docLineFromVisible`/`visibleFromDocLine` calls and nothing else in the sync path
moves. The one genuinely new row needed on that day: folding and wrap re-layout change
the display-line count *without* any text change, so neither `modified`
(Insert/DeleteText) nor `linesAdded` fires and the mapping would go stale. Folding
surfaces as `modified` with `SC_MOD_CHANGEFOLD` — currently filtered out — and wrap
re-layout as a viewport change after resize. Neither invalidates a *strip*; the cache
is document-line-keyed, which is precisely why it survives both. Only the mapping is
re-derived.

## Decorations

Once the base render is trusted (phase 4), overlay lanes on top of the strips:

- **Diagnostics** — `EditorView` already holds `diagnostics_`; draw a 3px error/warning
  tick in a reserved right-hand lane using `theme.diagnosticError` /
  `theme.diagnosticWarning`. This is the highest-value overlay and the reason to have
  a minimap at all on a long file.
- **Selection** — fill selected lines with `theme.selectionBg` at low alpha.
- **Caret line** — 1px `theme.caret` rule.
- **Occurrences** — the one lane [`lsp-navigation.md`](lsp-navigation.md) §2
  adds. `EditorView::occurrences()` holds the ranges for the symbol under the
  caret; wash them with `theme.occurrenceHighlight` rather than filling them.
  c2mp's reasoning applies unchanged: *the token colours underneath have to stay
  legible, and a mark that hides the code it marks defeats the purpose.*

  **The ordering constraint this lane came with is now satisfied.** It said the
  lane must never be fed by word-based matching — `total` inside `subtotal`,
  inside a comment, inside a string — because a wrong answer under the caret is
  survivable while the same answer painted down the whole file is not. The
  source is now `textDocument/documentHighlight`, which is token-based and
  measurably excludes comments and strings, so the lane is safe to build. Read
  `occurrences()`; do not re-derive matches here.

All four are painted in `paintEvent` over the cached image, never baked into it, so
they cost nothing to invalidate.

## Theme integration

Add to `struct Theme` (`src/editor/theme_loader.h`) and to
`resources/turmeric-dark.theme.json`:

```
minimapBg, minimapSliderBg, minimapSliderHoverBg, minimapSliderActiveBg
```

Each falls back when absent so a user's existing theme file keeps working:
`minimapBg → editorBg`, slider colors → `selectionBg` at 25% / 40% / 55% alpha.
`ApplyThemeToEditor` is untouched; `MinimapView::setTheme(const Theme&)` pulls both
these four and `StyleForegroundTable(theme)`.

## Configuration

Following the exact `preferences_view.cpp` pattern (widget → `commitX()` → `QSettings`
→ signal → `MainWindow::applyX()` fan-out over open buffers, mirroring rainbow
brackets):

| Key | Type | Default | Notes |
|---|---|---|---|
| `editor/minimap` | bool | `true` | |
| `editor/minimapSide` | QString | `"right"` | `"right"` \| `"left"` |
| `editor/minimapWidth` | int | `90` | clamped 40..200 px |

- `PreferencesView`: a `QCheckBox` ("Minimap"), a `QComboBox` (side) and a `QSpinBox`
  (width) that are disabled while the checkbox is off. All three added to
  `restoreDefaults()`.
- New signal `PreferencesView::minimapSettingsChanged()`.
- `MainWindow::applyMinimapSettings()` next to `applyRainbowBrackets()`, iterating
  `buffers_` and calling `EditorView::applyMinimapSettings()` on each editor tab.
- A **View menu toggle** wired to the same setting, so it is reachable without opening
  preferences (and, importantly, testable via the existing `menu.invoke` control
  command).

`EditorView` owns the widget and reads the settings statically, exactly like
`rainbowBracketsDefault()`:

```cpp
static bool minimapEnabledDefault();
static MinimapView::Side minimapSideDefault();
static int minimapWidthDefault();
void applyMinimapSettings();   // re-reads QSettings, re-parents in the layout
```

## Class sketch

```cpp
// src/editor/minimap_view.h
#pragma once
#include "editor/theme_loader.h"
#include <QHash>
#include <QImage>
#include <QTimer>
#include <QVector>
#include <QWidget>
class ScintillaEdit;

namespace trowel {

// A block-rendered overview of the whole buffer, drawn beside the editor.
// Not a Scintilla margin: margins can only draw markers, so this is a sibling
// widget inside EditorView's layout and talks to `sci_` through its public API.
class MinimapView : public QWidget {
    Q_OBJECT
public:
    enum class Side { Left, Right };

    explicit MinimapView(ScintillaEdit* sci, QWidget* parent = nullptr);

    void setTheme(const Theme& theme);      // invalidates everything
    void setColumnPixels(int px);           // width of one character cell, default 1
    void setLinePixels(int px);             // height of one line, default 2
    void setVisibleColumns(int cols);       // widget width = cols * columnPixels

public slots:
    void invalidateAll();
    // `lastLine < 0` means "to end of document". Lexer state cascades forward,
    // so an edit anywhere conservatively dirties everything below it — which is
    // free, because dirty strips are only rendered when scrolled into view.
    void invalidateLines(int firstLine, int lastLine);
    void syncToEditorScroll();              // slider only; never re-renders

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    static constexpr int kStripLines = 512;
    struct Strip { int firstLine = 0; QImage image; bool dirty = true; };

    Strip& stripFor(int docLine);           // renders on demand
    void renderStrip(Strip& s);             // phase 1-2: GUI thread; phase 3: worker

    // The whole doc-line <-> pixel mapping funnels through these two. Wrap and
    // folding are both off today so they are the identity over document lines;
    // if either is ever enabled they become docLineFromVisible/visibleFromDocLine
    // and nothing else in this class has to change. That property is the point:
    // no other member may do its own line<->pixel arithmetic, however tempting,
    // because doing so is what would preclude ever turning wrap or folding on.
    int docLineForY(int y) const;
    int yForDocLine(int line) const;

    int displayLineCount() const;           // == lineCount() only while wrap/fold off
    int topDocLine() const;                 // proportional-slide anchor
    QRect sliderRect() const;
    void scrollEditorToY(int y, bool center);

    ScintillaEdit* sci_;
    QVector<Strip> strips_;
    QHash<int, QColor> styleFg_;
    QColor bg_, sliderBg_, sliderHoverBg_, sliderActiveBg_;
    int colPx_ = 1, linePx_ = 2, cols_ = 90;
    bool dragging_ = false, hovered_ = false;
    int dragGrabDy_ = 0;
    QTimer renderDebounce_;
};

}
```

## Files to add / change

**Add**
- `src/editor/minimap_view.h`, `src/editor/minimap_view.cpp`
- `tests/smoke/test_minimap.py`

**Change**
- `src/editor/editor_view.h` / `.cpp` — `QVBoxLayout` → outer `QVBoxLayout` holding a
  `QHBoxLayout` of `sci_` + `minimap_`; own the `MinimapView`; forward dirty ranges from
  the existing `modified` lambda; forward `verticalScrolled` / `updateUi`;
  `applyMinimapSettings()` + the three static defaults; call `minimap_->invalidateAll()`
  from `installLexer()`; re-render decorations from `setDiagnostics()`.
- `src/editor/theme_loader.h` / `.cpp` — four `minimap*` `QColor`s with fallbacks;
  `StyleForegroundTable(const Theme&)`.
- `src/editor/scanner.h`, `src/editor/lexer_adapter.cpp` — `StyleSink` /
  `DocumentSink` / `BufferSink`; `Emitter` writes through a sink (**phase 3 only**).
- `src/app/preferences_view.h` / `.cpp` — three controls, `commitMinimap*`,
  `minimapSettingsChanged()`, `restoreDefaults()`.
- `src/app/main_window.h` / `.cpp` — `applyMinimapSettings()`, View-menu toggle action,
  wire `PreferencesView::minimapSettingsChanged`.
- `src/control/control_handlers.cpp` — `editor.minimap` command returning
  `{enabled, side, width, sliderTop, sliderHeight, topLine}` so smoke tests can assert
  on it without pixel-diffing.
- `resources/turmeric-dark.theme.json` — the four new keys.
- `CMakeLists.txt` — add `minimap_view.cpp` / `.h` to `trowel_lib`.
- `docs/smoke-tests.md` — the manual checklist below.

## Phased rollout

Each phase is independently shippable.

### Phase 1 — a working minimap, off by default
Widget, strip cache, block render sourcing style bytes from Scintilla via
`SCI_GETSTYLEDTEXT` on the GUI thread (option A), per-strip `colourise` before the
fetch. Slider overlay, click/drag/wheel. `editor/minimap` pref + View-menu toggle,
**default off**. Hard cap: if `lineCount() > 200_000` the widget hides itself — the
GUI-thread style fetch is the only thing that cannot absorb a pathological file, and
this is the safety valve until phase 3 removes the need for it.
Ships behind an off-by-default flag, so the blast radius is zero.

### Phase 2 — incremental invalidation
Dirty-range plumbing from `modified` and `linesAdded`, LRU strip eviction, 60 ms render
debounce, decorations-over-cache separation so scrolling and hovering never re-render.
This is what turns phase 1 from "works" into "feels free while typing". Raise the cap
to 1M lines. **Flip the default to on** and add the `Alt+M` shortcut — the debounce
and eviction make the GUI-thread render safe enough for everyday use.

### Phase 3 — off-thread rendering
The `StyleSink` refactor, then move `renderStrip` into a `QtConcurrent` worker seeded
with `(strip text copy, sci_->lineState(firstLine - 1))`. Strips arrive asynchronously
and repaint on completion; un-rendered strips draw as flat `minimapBg`. Removes the
cap entirely and removes the forced `colourise`.

### Phase 4 — polish
Prefs UI for side and width; left-side layout; diagnostics / selection / caret-line
overlays; optional `setVScrollBar(false)` so the minimap replaces the native
scrollbar the way VSCode does (deliberately last — do not remove the user's scrollbar
until minimap drag has proven itself).

## Risks

- **Riskiest: the GUI-thread style fetch in phases 1–2.** `SCI_GETSTYLEDTEXT` past
  `endStyled()` returns zeros unless we colourise first, and a careless
  `colourise(0, -1)` on a 20 MB file is a multi-second freeze. The mitigation is
  structural — strips are rendered lazily and only ever colourise their own range —
  but it is one bad line of code away from a hang, and it is the reason phases 1–2 keep
  a document-size cap. Phase 3 deletes the risk rather than managing it.
- **Lexer state cascade.** `#|`, an unterminated string, or a Markdown fence restyles
  everything below. Handled by dirtying to EOF; correct only because rendering is lazy.
  If lazy rendering is ever abandoned, this becomes an O(document) cost per keystroke.
- **HiDPI.** 1px cells at DPR 2 must be authored as 2 physical px on a
  `devicePixelRatio`-aware `QImage`. Getting this wrong yields a blurry, half-line-
  offset minimap that still "works", so it will not be caught by a functional test —
  put it on the manual checklist.
- **Layout regressions in `EditorView`.** Wrapping `sci_` in an `QHBoxLayout` touches
  focus order, drag-drop targeting, and the `contentsMargins(0,4,0,0)` alignment. The
  minimap must be `Qt::NoFocus` and outside tab order. `sciWidget()` and every control-
  API consumer must be unaffected.
- **Line-state seeding past `endStyled()`** produces a cosmetically-wrong strip in an
  unvisited region. Acceptable and self-correcting; must be commented so a future
  reader does not "fix" it by colourising the world.
- **Proportional slide surprises people** who expect the whole file. It is the VSCode
  behavior and the right default; note it in the smoke-test doc so it is not filed as
  a bug.
- **Silently baking in "wrap and folding are off."** The 1:1 document↔display line
  assumption creeps back one `line * linePx_` at a time, and nothing fails — until the
  day either feature ships, when the minimap is subtly wrong everywhere and the cause
  is smeared across the file. No test can catch this today, because today the
  assumption is true. It is a review checklist item and the reason the mapping is
  funneled through two functions and the line count through one accessor.

## Verification

1. `just build` — must stay clean under `-Wall -Wextra -Wpedantic -Werror`.
2. `just smoke` — existing suite must not regress; new `tests/smoke/test_minimap.py`:
   toggle via `menu.invoke` → `editor.minimap` reports `enabled`; open a fixture and
   assert `sliderTop`/`sliderHeight` move as expected after `editor.set_cursor` to EOF;
   assert the widget hides above the phase-1 cap.
3. `just run` and drive it by hand — the real check:
   - minimap colors visibly match the text for `.tur`, `.md` (with a Turmeric fence
     inside), `.json`, and a `Justfile`; rainbow brackets show as rainbow in the minimap.
   - type a `#|` near the top of a 5k-line file → everything below recolors, no hitch.
   - drag the slider through a 100k-line file → smooth, no dropped frames.
   - resize the window and the splitter → no re-render stall, slider stays correct.
   - toggle the pref off/on with several tabs open → all editors update.
   - on a Retina display, cells are crisp and the slider is not a half-line off.
4. Profile phase 3 with Instruments on a 50 MB file: no GUI-thread sample should sit
   inside `renderStrip`.
