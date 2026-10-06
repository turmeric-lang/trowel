#include "editor/editor_view.h"

#include "app/settings.h"
#include "editor/find_bar.h"
#include "editor/find_engine.h"
#include "editor/lexers.h"
#include "editor/minimap_view.h"
#include "editor/theme_loader.h"
#include "lsp/lsp_manager.h"

#include <ScintillaEdit.h>

#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontInfo>
#include <QHBoxLayout>
#include <QHash>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPainter>
#include <QPen>
#include <QScrollBar>
#include <QSet>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>

namespace trowel {

// The two vertical parts of the bracket-pair guide, painted over the viewport.
// No Q_OBJECT: it has no signals or slots, so it needs no moc and can live
// entirely in this file.
//
// The *spine* is a hairline in the opener's own column, spanning the rows the
// form covers. The *gutter bar* is a thicker rule at the right edge of the
// margins, spanning the same rows — it answers "which lines is the caret's
// expression in?" from the edge of the window, where the eye can find it
// without first locating the opener. They carry the same colour because they
// describe the same pair.
class BracketGuideOverlay : public QWidget {
public:
  explicit BracketGuideOverlay(QWidget *parent) : QWidget(parent) {
    // Never take a click. Without this the overlay would swallow every
    // mouse event over the text it covers.
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);
    hide();
  }

  void setLine(int x, int top, int bottom, const QColor &color) {
    x_ = x;
    top_ = top;
    bottom_ = bottom;
    color_ = color;
    syncVisible();
  }

  void clearLine() {
    x_ = -1;
    syncVisible();
  }

  void setGutterBar(const QRect &rect, const QColor &color) {
    gutter_ = rect;
    gutterColor_ = color;
    syncVisible();
  }

  void clearGutterBar() {
    gutter_ = QRect();
    syncVisible();
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    if (!gutter_.isEmpty()) {
      painter.fillRect(gutter_, gutterColor_);
    }
    if (x_ >= 0 && bottom_ > top_) {
      painter.setPen(QPen(color_, 1));
      painter.drawLine(x_, top_, x_, bottom_);
    }
  }

private:
  // Hidden only when neither part has anything to draw, so clearing one does
  // not take the other down with it.
  void syncVisible() {
    const bool anything = (x_ >= 0 && bottom_ > top_) || !gutter_.isEmpty();
    setVisible(anything);
    if (anything)
      update();
  }

  int x_ = -1;
  int top_ = 0;
  int bottom_ = 0;
  QColor color_;
  QRect gutter_;
  QColor gutterColor_;
};

namespace {
// User-list identities. Scintilla hands the list type back with the selection,
// which is how an outline pick is told apart from a placeholder row nobody
// should be able to act on.
constexpr int kOutlineListType = 1;
constexpr int kOutlineMessageListType = 2;
constexpr int kChooserListType = 3;

// Separates a row's name from its kind. An em dash rather than a hyphen so it
// cannot be mistaken for part of a Turmeric identifier, which routinely
// contains hyphens (`nav-total`, `list-head`).
const char *const kOutlineKindSeparator = " — ";

// How far the enclosing-pair scan will look before giving up, in bytes each
// way. This runs on every caret move, so it is bounded rather than allowed to
// walk a large file twice; a give-up paints nothing.
constexpr int kBracketScanLimit = 64 * 1024;

bool IsBracketChar(char c) {
  return c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}';
}

bool IsOpenBracket(char c) { return c == '(' || c == '[' || c == '{'; }

// Is the bracket at this style a real one, or is it text inside a comment or a
// string?
//
// Asked of the style rather than of a second parser: the lexer already styles a
// `(` inside a string as String and inside a `;` comment as a comment style, so
// a scan that consults the style skips both for free. This is the property that
// makes the whole scan cheap (§4.2 of the editor intelligence plan).
bool IsCodeStyle(int style) {
  switch (static_cast<TurStyle>(style)) {
  case TurStyle::LineComment:
  case TurStyle::DocComment:
  case TurStyle::BlockComment:
  case TurStyle::String:
  case TurStyle::StringEscape:
  case TurStyle::CharLit:
    return false;
  default:
    return true;
  }
}

// How long the caret must sit still before occurrences are requested. Matches
// the didChange debounce rather than inventing a second cadence, and it is the
// reason arrow-key navigation does not flood a single-threaded server.
constexpr int kOccurrenceDebounceMs = 250;

// Thickness of the gutter bar marking the caret expression's line range. Wider
// than the 1px spine on purpose: it is read peripherally, at the edge of the
// text, and a hairline there disappears against the margin's own divider.
constexpr int kGutterBarWidth = 2;
} // namespace

EditorView::EditorView(QWidget *parent)
    : TabContent(parent), sci_(new ScintillaEdit(this)) {
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 4, 0, 0);

  // The editor and its minimap sit side by side. The minimap is hidden
  // by default and only shown when the preference is on. A custom
  // vertical scrollbar always lives to the right of the minimap (or
  // directly to the right of the editor when the minimap is off) and
  // replaces Scintilla's native scrollbar in both cases.
  auto *editorRow = new QHBoxLayout();
  editorRow->setContentsMargins(0, 0, 0, 0);
  editorRow->setSpacing(0);
  editorRow->addWidget(sci_, 1);
  minimap_ = new MinimapView(sci_, this);
  editorRow->addWidget(minimap_);
  editorScrollBar_ = new QScrollBar(Qt::Vertical, this);
  editorScrollBar_->hide();
  editorRow->addWidget(editorScrollBar_);
  layout->addLayout(editorRow, 1);

  findBar_ = new FindBar(this);
  findBar_->hide();
  layout->addWidget(findBar_);

  applyDefaultStyling();

  rainbow_ = rainbowBracketsDefault();
  bracketGuides_ = bracketPairGuidesDefault();
  installLexer();
  applyWrap();
  applyFoldMargin();
  const Theme theme = LoadBuiltinDarkTheme();
  ApplyThemeToEditor(sci_, theme);
  minimap_->setTheme(theme);
  applyMinimapSettings();

  // The external scrollbar drives Scintilla's vertical scroll. Display
  // lines throughout — Scintilla's firstVisibleLine/linesOnScreen speak
  // display lines, so the scrollbar does too.
  connect(editorScrollBar_, &QScrollBar::valueChanged, this, [this](int value) {
    if (sci_)
      sci_->setFirstVisibleLine(value);
  });

  connect(sci_, &ScintillaEditBase::savePointChanged, this,
          [this](bool dirty) { emit modifiedChanged(dirty); });

  // Text edits bump the document version. Style and marker notifications also
  // arrive on this signal, so filter to actual content changes — otherwise
  // painting diagnostics would itself look like an edit and loop.
  connect(sci_, &ScintillaEditBase::modified, this,
          [this](Scintilla::ModificationFlags type,
                 Scintilla::Position position, Scintilla::Position,
                 Scintilla::Position, const QByteArray &,
                 Scintilla::Position, Scintilla::FoldLevel,
                 Scintilla::FoldLevel) {
            constexpr auto kContentChange =
                Scintilla::ModificationFlags::InsertText |
                Scintilla::ModificationFlags::DeleteText;
            if ((type & kContentChange) == Scintilla::ModificationFlags::None)
              return;
            docVersion_++;
            // A `#lang` line can be typed, pasted, or edited away at any
            // moment, so the language is re-derived per edit rather than only
            // on open.
            refreshLanguage();
            // An edit moves the brackets around the caret, so the guide has to
            // be recomputed even when the caret itself did not move.
            updateBracketGuide();
            // Scintilla has already moved the breakpoint markers by the time
            // this fires; this is where the model is told, so a breakpoint set
            // on a line that has since slid down still means the same
            // statement.
            reconcileBreakpointLines();
            // An edit can cross a power of ten and change how many digits the
            // widest line number needs.
            updateLineNumberWidth();
            // The minimap's strip cache is keyed on document lines; an edit
            // dirties everything from the edit point to EOF because lexer state
            // cascades forward. Use the modification position, not the caret —
            // the caret may have moved on by the time this fires.
            if (minimap_ && minimap_->isVisible())
              minimap_->invalidateLines(
                  static_cast<int>(
                      sci_->lineFromPosition(static_cast<sptr_t>(position))),
                  -1);
            emit contentChanged(docVersion_);
          });

  // The server advertises `(` and space as triggers. We honor `(` only:
  // space fires on nearly every keystroke in a lisp, and each request forces
  // a didChange plus a full compile on the server's single thread. Explicit
  // completion (Ctrl+Space) covers the rest.
  connect(sci_, &ScintillaEditBase::charAdded, this, [this](int ch) {
    if (ch == '\n')
      autoIndentAfterNewline();
    if (ch == '(')
      emit completionRequested(cursorPos());
    // Signature help auto-triggers on SPACE, which is the server's own
    // trigger character as of v0.61.0.
    //
    // It used to advertise `(` and answer `null` there -- in a lisp the
    // callee is typed AFTER the paren, so there was nothing to describe yet.
    // The trigger moved to `" "`, which is the first moment a callee is
    // behind the cursor, and upstream took signature help out of the
    // server's re-analysis group -- so a space now costs a document sync
    // and an index lookup rather than a full recompile. That recompile is
    // what kept this explicit-only against v0.60.1.
    if (ch == ' ')
      emit signatureHelpRequested(cursorPos());
  });

  connect(sci_, &ScintillaEditBase::dwellStart, this, [this](int x, int y) {
    const int pos = int(sci_->positionFromPoint(x, y));
    if (pos >= 0)
      emit hoverRequested(pos);
  });
  connect(sci_, &ScintillaEditBase::dwellEnd, this, [this](int, int) {
    sci_->callTipCancel();
    callTipText_.clear();
    emit hoverEnded();
  });

  // Click the breakpoint margin to toggle a breakpoint. `position` is the
  // text position under the click; lineFromPosition gives the 0-based line,
  // +1 for the 1-based line the model and DAP use.
  connect(sci_, &ScintillaEditBase::marginClicked, this,
          [this](Scintilla::Position position, Scintilla::KeyMod, int margin) {
            if (margin != margins::kGutter)
              return;
            const int line = int(sci_->lineFromPosition(int(position))) + 1;
            emit breakpointToggleRequested(line);
          });

  // The dedicated userListSelection() signal carries no arguments (see
  // ScintillaEditBase.h: "Wants some args."), and the selection arrives as
  // *text* rather than an index. The generic notify() hook is the only place
  // both the chosen string and the list type are available.
  connect(sci_, &ScintillaEditBase::notify, this,
          [this](Scintilla::NotificationData *scn) {
            if (!scn ||
                scn->nmhdr.code != Scintilla::Notification::UserListSelection)
              return;
            if (scn->listType == kChooserListType) {
              const int row =
                  chooserRows_.indexOf(QString::fromUtf8(scn->text));
              if (row >= 0)
                emit listRowChosen(row);
              return;
            }
            if (scn->listType != kOutlineListType)
              return; // a placeholder row
            const int index =
                outlineRows_.indexOf(QString::fromUtf8(scn->text));
            if (index < 0 || index >= outlineSymbols_.size())
              return;
            const LspSymbol &sym = outlineSymbols_.at(index);
            emit outlineSymbolChosen(sym.selection.startLine,
                                     sym.selection.startCharacter);
          });

  occurrenceDebounce_ = new QTimer(this);
  occurrenceDebounce_->setSingleShot(true);
  occurrenceDebounce_->setInterval(kOccurrenceDebounceMs);
  connect(occurrenceDebounce_, &QTimer::timeout, this, [this] {
    LspManager::instance()->requestDocumentHighlights(
        this, cursorPos(),
        [this](const QVector<LspRange> &ranges) { setOccurrences(ranges); });
  });

  connect(sci_, &ScintillaEditBase::updateUi, this,
          [this](Scintilla::Update updated) {
            // Selection covers caret movement too; scroll and style updates are
            // filtered out so merely scrolling does not cost a request.
            //
            // Scintilla::Update is a scoped enum with no bitwise operators, so
            // the flag test goes through the underlying type.
            using U = std::underlying_type_t<Scintilla::Update>;
            if (!(static_cast<U>(updated) &
                  static_cast<U>(Scintilla::Update::Selection)))
              return;
            // Repainted synchronously, unlike the occurrence highlight: the
            // pair is computed locally from styles that are already there, so
            // there is nothing to wait for and a debounce would only make it
            // lag the caret.
            updateBracketGuide();
            // The old set describes wherever the caret used to be. Dropping it
            // now rather than on reply means the highlight never lags the
            // caret.
            clearOccurrences();
            // Only Turmeric buffers with a path are registered with the server;
            // for the other eight languages the request would round-trip to an
            // unconditional empty answer on every caret settle.
            if (language_ != Language::Turmeric || path_.isEmpty())
              return;
            occurrenceDebounce_->start(); // restarts, coalescing bursts
          });

  // --- Find bar connections ---
  findDebounce_ = new QTimer(this);
  findDebounce_->setSingleShot(true);
  findDebounce_->setInterval(100);
  connect(findDebounce_, &QTimer::timeout, this, [this] { runFindSearch(); });

  connect(findBar_, &FindBar::searchChanged, this, [this] {
    // Sync the query from the bar into findQuery_ before the debounce
    // fires, so runFindSearch() always reads from findQuery_.
    findQuery_.text = findBar_->query();
    findQuery_.matchCase = findBar_->matchCase();
    findQuery_.wholeWord = findBar_->wholeWord();
    findQuery_.regex = findBar_->regex();
    findQuery_.inSelection = findBar_->inSelection();
    if (findQuery_.inSelection) {
      findQuery_.rangeStart = findSelectionStart_;
      findQuery_.rangeEnd = findSelectionEnd_;
    }
    findDebounce_->start();
  });
  connect(findBar_, &FindBar::findNextRequested, this, [this] { findNext(); });
  connect(findBar_, &FindBar::findPreviousRequested, this,
          [this] { findPrevious(); });
  connect(findBar_, &FindBar::replaceRequested, this,
          [this] { replaceCurrent(findBar_->replaceField()->text()); });
  connect(findBar_, &FindBar::replaceAllRequested, this,
          [this] { replaceAll(findBar_->replaceField()->text()); });
  connect(findBar_, &FindBar::closed, this, [this] { hideFind(); });
  connect(findBar_, &FindBar::selectAllOccurrencesRequested, this,
          [this] { selectAllOccurrences(); });

  attachLanguageServer();
}

// Talk to the language server from here rather than from MainWindow: the
// wiring is per-buffer, not per-window, and a tab can outlive the window it was
// created in (drag-and-drop between windows). Doing it in the view means it
// travels with the buffer and isn't duplicated per window.
void EditorView::attachLanguageServer() {
  LspManager *lsp = LspManager::instance();

  connect(this, &EditorView::filePathChanged, lsp,
          [this, lsp](const QString &) {
            // Covers both opening a file into a fresh tab and Save As, which
            // can change the language out from under us.
            lsp->closeDocument(this);
            lsp->openDocument(this);
          });
  connect(this, &EditorView::contentChanged, lsp,
          [this, lsp](int) { lsp->documentChanged(this); });
  connect(this, &QObject::destroyed, lsp,
          [this, lsp] { lsp->closeDocument(this); });

  connect(this, &EditorView::completionRequested, lsp, [this, lsp](int pos) {
    lsp->requestCompletion(this, pos, [this, pos](const QStringList &labels) {
      const int wordStart =
          static_cast<int>(sci_->wordStartPosition(pos, true));
      showCompletions(labels, qMax(0, pos - wordStart));
    });
  });
  connect(this, &EditorView::hoverRequested, lsp, [this, lsp](int pos) {
    lsp->requestHover(
        this, pos, [this, pos](const QString &text) { showHover(pos, text); });
  });
  connect(this, &EditorView::signatureHelpRequested, lsp, [this, lsp](int pos) {
    lsp->requestSignatureHelp(
        this, pos, [this, pos](const QString &text, int /*activeParameter*/) {
          // showSignatureHelp ignores empty text, which is what an
          // "answered, but no signature here" reply carries.
          showSignatureHelp(pos, text);
        });
  });
  connect(this, &EditorView::definitionRequested, lsp, [this, lsp](int pos) {
    lsp->requestDefinition(this, pos, [this](const LspLocation &location) {
      emit definitionResolved(location);
    });
  });

  connect(lsp, &LspManager::diagnosticsUpdated, this,
          [this, lsp](const QString &uri) {
            if (uri != LspManager::UriForPath(path_))
              return;
            setDiagnostics(lsp->diagnosticsFor(uri));
          });
}

void EditorView::applyDefaultStyling() {
  sci_->styleSetFont(STYLE_DEFAULT, "Menlo");
  sci_->styleSetSize(STYLE_DEFAULT, 12);
  sci_->styleClearAll();

  sci_->setMarginTypeN(margins::kLineNumber, SC_MARGIN_NUMBER);
  updateLineNumberWidth();

  // One symbol gutter for everything: diagnostics, breakpoints, and the
  // execution marker. Always present rather than shown on demand, so lines
  // do not shift horizontally the moment an error appears or a debug session
  // starts. Sensitive to clicks, which arrive on
  // ScintillaEditBase::marginClicked — clicking it toggles a breakpoint.
  sci_->setMarginTypeN(margins::kGutter, SC_MARGIN_SYMBOL);
  sci_->setMarginWidthN(margins::kGutter, margins::kGutterWidth);
  sci_->setMarginMaskN(margins::kGutter,
                       (1 << diag::kErrorMarker) | (1 << diag::kWarningMarker) |
                           (1 << dbg::kBreakpointMarker) |
                           (1 << dbg::kBreakpointDisabledMarker) |
                           (1 << dbg::kCurrentLineMarker) |
                           (1 << dbg::kSelectedFrameMarker) |
                           (1 << dbg::kBreakpointStoppedMarker) |
                           (1 << dbg::kBreakpointDisabledStoppedMarker));
  sci_->setMarginSensitiveN(margins::kGutter, true);

  // The remaining two margins: margin 2 is the fold margin (set up in
  // applyFoldMargin for languages that have a fold strategy); margin 3 is
  // held at zero.
  sci_->setMarginWidthN(3, 0);

  // Fold markers and automatic fold behaviour.  Configured once here;
  // the margin width is toggled by applyFoldMargin() when the language
  // changes.
  sci_->setMarginTypeN(2, SC_MARGIN_SYMBOL);
  sci_->setMarginMaskN(2, SC_MASK_FOLDERS);
  sci_->setMarginSensitiveN(2, true);
  sci_->setAutomaticFold(SC_AUTOMATICFOLD_SHOW | SC_AUTOMATICFOLD_CLICK |
                         SC_AUTOMATICFOLD_CHANGE);
  sci_->setFoldFlags(SC_FOLDFLAG_LINEAFTER_CONTRACTED);
  sci_->setDefaultFoldDisplayText(" \xe2\x80\xa6 "); // " … "

  // Fold markers: triangles on header lines, nothing on body/tail lines.
  // SC_MARK_ARROW is a right-pointing triangle (contracted);
  // SC_MARK_ARROWDOWN is a down-pointing triangle (expanded).
  // Body and tail markers are SC_MARK_EMPTY so non-foldable lines show
  // nothing.
  sci_->markerDefine(SC_MARKNUM_FOLDER, SC_MARK_ARROW); // contracted header
  sci_->markerDefine(SC_MARKNUM_FOLDEROPEN,
                     SC_MARK_ARROWDOWN); // expanded header
  sci_->markerDefine(SC_MARKNUM_FOLDEREND,
                     SC_MARK_ARROW); // contracted, end of range
  sci_->markerDefine(SC_MARKNUM_FOLDEROPENMID,
                     SC_MARK_ARROWDOWN); // expanded, mid range
  sci_->markerDefine(SC_MARKNUM_FOLDERSUB, SC_MARK_EMPTY);     // body line
  sci_->markerDefine(SC_MARKNUM_FOLDERTAIL, SC_MARK_EMPTY);    // tail line
  sci_->markerDefine(SC_MARKNUM_FOLDERMIDTAIL, SC_MARK_EMPTY); // mid tail line

  sci_->markerDefine(diag::kErrorMarker, SC_MARK_CIRCLE);
  sci_->markerDefine(diag::kWarningMarker, SC_MARK_CIRCLE);
  // Breakpoint markers: a filled circle when enabled, a hollow circle when
  // disabled or pending. The current-execution line gets a short arrow in
  // the margin plus a background tint on the line; a selected (non-top)
  // frame gets a hollow arrow so it reads as "looking at" rather than "at".
  sci_->markerDefine(dbg::kBreakpointMarker, SC_MARK_CIRCLE);
  sci_->markerDefine(dbg::kBreakpointDisabledMarker, SC_MARK_CIRCLE);
  // A dot, in its own margin. SC_MARK_BACKGROUND was tried first and drew
  // nothing at all: sampling the rendered pixels on the stopped line came
  // back #0C0A08 — the untouched editor background — even with the tint set
  // to opaque magenta. A dot in a margin uses exactly the mechanism the
  // breakpoint marker already proves works.
  //
  // Hollow for a frame you are only *looking at*, filled for the one the
  // program is actually stopped in.
  sci_->markerDefine(dbg::kCurrentLineMarker, SC_MARK_CIRCLE);
  sci_->markerDefine(dbg::kSelectedFrameMarker, SC_MARK_CIRCLE);
  // Stopped on a breakpoint. Same circle; the ring and the fill carry
  // different colours (set in the theme loader) so one dot says both things
  // rather than two dots in two margins saying one each.
  sci_->markerDefine(dbg::kBreakpointStoppedMarker, SC_MARK_CIRCLE);
  sci_->markerDefine(dbg::kBreakpointDisabledStoppedMarker, SC_MARK_CIRCLE);
  sci_->indicSetStyle(diag::kErrorIndicator, INDIC_SQUIGGLE);
  sci_->indicSetStyle(diag::kWarningIndicator, INDIC_SQUIGGLE);
  // Colors come from the theme; these are visible fallbacks for a theme that
  // omits the diagnostics block.
  sci_->indicSetFore(diag::kErrorIndicator, 0x0000CC);
  sci_->indicSetFore(diag::kWarningIndicator, 0x00A0D0);

  // Scintilla's default autocomplete *type* separator is '?', and a Turmeric
  // predicate is spelled `empty?` / `nil?` / `zero?` by convention.
  // `ListBoxImpl::SetList` splits each item at that separator and calls
  // `Append(word, atoi(rest))`; for a bare trailing '?' that is `atoi("")`
  // == 0, and `Append` then does `Q_ASSERT(images.contains(0))` against a map
  // nothing ever registers into — Trowel never calls RegisterImage.
  //
  // So typing `(` in a buffer whose completions include any `?`-suffixed name
  // aborted the debug build outright. Release only escaped it because
  // Q_ASSERT compiles out; it still reserved icon space for an image that
  // does not exist. Measured against the vendored Scintilla, PlatQt.cpp:1113.
  //
  // Set once here rather than beside each `autoCSetSeparator` call: it is a
  // persistent setting, and all four list paths (completions, outline,
  // references chooser, symbol list) can carry a name with a '?' in it.
  //
  // \x01 rather than a printable byte, because every printable byte is legal
  // somewhere in a lisp identifier.
  sci_->autoCSetTypeSeparator('\x01');

  // The active bracket-pair guide: one straight line under the enclosing
  // expression. Its colour is set per update from the pair's own depth style,
  // so only the shape is configured here.
  sci_->indicSetStyle(bracketguide::kIndicator, INDIC_PLAIN);

  guideOverlay_ = new BracketGuideOverlay(sci_->viewport());
  // Scintilla emits `painted` after every repaint, which covers scrolling,
  // resizing and wrapping in one hook — cheaper and more reliable than
  // chasing each of those separately.
  connect(sci_, &ScintillaEditBase::painted, this, [this] {
    repositionBracketGuideOverlay();
    // The minimap slider follows the editor's scroll. The painted
    // signal covers vertical scroll, resize, and wrap re-layout in
    // one hook — the same reason the bracket guide uses it.
    if (minimap_ && minimap_->isVisible())
      minimap_->syncToEditorScroll();
    // Keep the external scrollbar in sync when Scintilla scrolls
    // for any reason (keyboard, wheel over the text, programmatic).
    if (editorScrollBar_ && editorScrollBar_->isVisible())
      syncEditorScrollBar();
  });

  // Hover: how long the mouse must rest before dwellStart fires.
  sci_->setMouseDwellTime(500);

  sci_->setCaretLineVisible(true);
  sci_->setCaretLineLayer(SC_LAYER_UNDER_TEXT);
  // Actual color set by the theme; alpha capped at ~64/255 so text remains
  // legible on the caret line.

  sci_->setUseTabs(false);
  sci_->setTabWidth(2);
  sci_->setIndent(2);
  sci_->setBackSpaceUnIndents(true);
  sci_->setEOLMode(SC_EOL_LF);

  sci_->setViewWS(SCWS_INVISIBLE);
  // Start at 1px so tracking can grow the width to fit the longest visible
  // line — leaving the default 2000px would show a horizontal scrollbar even
  // for empty buffers.
  sci_->setScrollWidth(1);
  sci_->setScrollWidthTracking(true);
  sci_->setHScrollBar(true);
  sci_->setEndAtLastLine(false);

  // Rebind Home/End to the wrap-aware versions so the first press goes to
  // the start/end of the display row and the second to the document line.
  // These work correctly with and without wrap.
  sci_->assignCmdKey(SCK_HOME, SCI_VCHOMEWRAP);
  sci_->assignCmdKey(SCK_END, SCI_LINEENDWRAP);
}

void EditorView::setFont(const QFont &font) {
  currentFont_ = font;
  const QByteArray family = font.family().toUtf8();
  const int size = font.pointSize() > 0 ? font.pointSize() : 12;
  // Apply font to every style we know about individually. Do NOT use
  // styleClearAll — that would copy STYLE_DEFAULT to all styles and wipe the
  // per-style foreground colors installed by ApplyThemeToEditor.
  sci_->styleSetFont(STYLE_DEFAULT, family.constData());
  sci_->styleSetSize(STYLE_DEFAULT, size);
  sci_->styleSetFont(STYLE_LINENUMBER, family.constData());
  sci_->styleSetSize(STYLE_LINENUMBER, size);
  // Covers every language's block plus the rainbow bracket styles, all of
  // which live outside Scintilla's predefined 32..39 range.
  for (int s = 0; s <= kMaxStyleId; ++s) {
    sci_->styleSetFont(s, family.constData());
    sci_->styleSetSize(s, size);
  }
}

bool EditorView::rainbowBracketsDefault() {
  return Settings::instance().rainbowBrackets();
}

void EditorView::installLexer() {
  // Scintilla takes ownership and releases any previously installed lexer.
  sci_->setILexer(
      reinterpret_cast<sptr_t>(CreateLexerForLanguage(language_, rainbow_)));
  // Re-lex the whole document so existing text picks up the new styling
  // immediately.
  sci_->colourise(0, -1);
  // A language or rainbow change restyles everything, so the minimap's
  // entire strip cache is stale.
  if (minimap_)
    minimap_->invalidateAll();
}

bool EditorView::minimapEnabledDefault() {
  return Settings::instance().minimapEnabled();
}

int EditorView::minimapWidthDefault() {
  // Width is not yet a separate setting (phase 4); use the plan default.
  return 90;
}

void EditorView::applyMinimapSettings() {
  if (minimap_)
    minimap_->applySettings(minimapEnabledDefault(), minimapWidthDefault());
  // The external scrollbar is always used in place of Scintilla's native
  // one. It lives to the right of the minimap when the minimap is on, and
  // directly to the right of the editor when it is off.
  if (sci_)
    sci_->setVScrollBar(false);
  if (editorScrollBar_) {
    editorScrollBar_->setVisible(true);
    syncEditorScrollBar();
  }
}

void EditorView::syncEditorScrollBar() {
  if (!editorScrollBar_ || !editorScrollBar_->isVisible() || !sci_)
    return;
  const int total =
      static_cast<int>(sci_->visibleFromDocLine(sci_->lineCount()));
  const int page = static_cast<int>(sci_->linesOnScreen());
  const int max = qMax(0, total - page);
  // Block signals so updating the range/value does not feed back into
  // Scintilla via the valueChanged handler.
  QSignalBlocker block(editorScrollBar_);
  editorScrollBar_->setRange(0, max);
  editorScrollBar_->setPageStep(page);
  editorScrollBar_->setSingleStep(1);
  editorScrollBar_->setValue(static_cast<int>(sci_->firstVisibleLine()));
}

void EditorView::autoIndentAfterNewline() {
  // Sweet-expression source is indentation-sensitive, so losing the indent on
  // every Enter is actively painful there. The other languages Trowel handles
  // are brace- or paren-delimited and have gotten along without auto-indent,
  // so they keep the plain behavior rather than inherit a half-rule.
  if (language_ != Language::TurmericSweet)
    return;

  const int pos = cursorPos();
  const int line = static_cast<int>(sci_->lineFromPosition(pos));
  if (line <= 0)
    return;
  // Only act when the caret is at the head of the new line. Splitting a line
  // in the middle carries its own text along; injecting indent there would
  // push that text rightward instead of lining it up.
  if (pos != static_cast<int>(sci_->positionFromLine(line)))
    return;

  const QByteArray prev = sci_->getLine(line - 1);
  int n = 0;
  while (n < prev.size() && (prev[n] == ' ' || prev[n] == '\t'))
    ++n;
  if (n == 0)
    return;

  const QByteArray indent = prev.left(n);
  sci_->insertText(pos, indent.constData());
  setCursorPos(pos + n);
}

void EditorView::setRainbowBrackets(bool enabled) {
  if (rainbow_ == enabled)
    return;
  rainbow_ = enabled;
  installLexer();
}

// --- Word wrap (Phase 4) ---

bool EditorView::isWordWrap() const { return sci_->wrapMode() != SC_WRAP_NONE; }

void EditorView::setWrapOverride(WrapOverride wo) {
  wrapOverride_ = wo;
  applyWrap();
}

void EditorView::toggleWordWrap() {
  // Cycle Default -> On -> Off -> Default.
  switch (wrapOverride_) {
  case WrapOverride::Default:
    wrapOverride_ = WrapOverride::On;
    break;
  case WrapOverride::On:
    wrapOverride_ = WrapOverride::Off;
    break;
  case WrapOverride::Off:
    wrapOverride_ = WrapOverride::Default;
    break;
  }
  applyWrap();
}

void EditorView::applyWrap() {
  const bool isProse =
      (language_ == Language::Markdown || language_ == Language::PlainText);
  bool wrap;
  switch (wrapOverride_) {
  case WrapOverride::On:
    wrap = true;
    break;
  case WrapOverride::Off:
    wrap = false;
    break;
  case WrapOverride::Default:
    wrap = isProse ? Settings::instance().wrapProse()
                   : Settings::instance().wrapCode();
    break;
  }

  if (wrap) {
    sci_->setWrapMode(SC_WRAP_WORD);
    sci_->setWrapIndentMode(isProse ? SC_WRAPINDENT_SAME
                                    : SC_WRAPINDENT_INDENT);
    sci_->setWrapVisualFlags(SC_WRAPVISUALFLAG_MARGIN);
    sci_->setLayoutCache(SC_CACHE_PAGE);
  } else {
    sci_->setWrapMode(SC_WRAP_NONE);
  }
}

// --- Folding (Phase 5) ---

bool EditorView::hasFolding() const { return HasFoldStrategy(language_); }

void EditorView::applyFoldMargin() {
  // Show the fold margin only for languages that have a fold strategy,
  // so it does not appear and disappear while typing.
  sci_->setMarginWidthN(2, HasFoldStrategy(language_) ? 12 : 0);
}

// Find the header line for the caret: the line itself if it is a header,
// otherwise the nearest enclosing header via SCI_GETFOLDPARENT.  Returns -1
// if there is none.
static int FoldHeaderForCaret(ScintillaEdit *sci) {
  const int line = static_cast<int>(sci->lineFromPosition(sci->currentPos()));
  const int level = static_cast<int>(sci->foldLevel(line));
  if (level & SC_FOLDLEVELHEADERFLAG)
    return line;
  // Not a header — find the enclosing header.
  const int parent = static_cast<int>(sci->foldParent(line));
  return parent;
}

void EditorView::foldCurrent() {
  const int header = FoldHeaderForCaret(sci_);
  if (header < 0)
    return;
  sci_->foldLine(header, SC_FOLDACTION_CONTRACT);
}

void EditorView::unfoldCurrent() {
  const int header = FoldHeaderForCaret(sci_);
  if (header < 0)
    return;
  sci_->foldLine(header, SC_FOLDACTION_EXPAND);
}

void EditorView::toggleFold() {
  const int header = FoldHeaderForCaret(sci_);
  if (header < 0)
    return;
  sci_->foldLine(header, SC_FOLDACTION_TOGGLE);
}

void EditorView::foldAll() {
  sci_->foldAll(SC_FOLDACTION_CONTRACT | SC_FOLDACTION_CONTRACT_EVERY_LEVEL);
  // Move the caret to the header of the region it was in.
  const int header = FoldHeaderForCaret(sci_);
  if (header >= 0)
    sci_->gotoLine(header);
}

void EditorView::unfoldAll() { sci_->foldAll(SC_FOLDACTION_EXPAND); }

void EditorView::foldTopLevel() {
  sci_->foldAll(SC_FOLDACTION_CONTRACT);
  const int header = FoldHeaderForCaret(sci_);
  if (header >= 0)
    sci_->gotoLine(header);
}

bool EditorView::loadFile(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return false;
  }
  const QByteArray contents = file.readAll();
  // Any previous read-only state has to come off before setText, which
  // Scintilla ignores on a read-only document — otherwise reusing a tab that
  // once held a stdlib file would silently load nothing.
  sci_->setReadOnly(false);
  sci_->setText(contents.constData());
  sci_->emptyUndoBuffer();
  sci_->setSavePoint();
  setPath(path);
  // Applied here rather than at each call site: three code paths load a file
  // into a buffer, and a stdlib file that arrived through the one that forgot
  // would be quietly editable.
  sci_->setReadOnly(LspManager::instance()->isStdlibPath(path));
  emit modifiedChanged(false);
  return true;
}

bool EditorView::saveFile(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text |
                 QIODevice::Truncate)) {
    return false;
  }
  const QByteArray contents = sci_->getText(sci_->textLength() + 1);
  // getText appends a NUL byte; strip it before writing.
  file.write(contents.constData(),
             contents.size() > 0 && contents.endsWith('\0')
                 ? contents.size() - 1
                 : contents.size());
  file.close();
  sci_->setSavePoint();
  setPath(path);
  emit modifiedChanged(false);
  return true;
}

bool EditorView::saveCurrent() {
  if (path_.isEmpty())
    return false;
  return saveFile(path_);
}

QString EditorView::displayName() const {
  if (!path_.isEmpty())
    return QFileInfo(path_).fileName();
  return {};
}

bool EditorView::isModified() const { return sci_->modify(); }

bool EditorView::isEmpty() const { return sci_->textLength() == 0; }

QByteArray EditorView::text() const {
  QByteArray raw = sci_->getText(sci_->textLength() + 1);
  if (raw.endsWith('\0'))
    raw.chop(1);
  return raw;
}

QByteArray EditorView::textInRange(int startPos, int endPos) const {
  if (endPos <= startPos)
    return {};
  return sci_->textRange(startPos, endPos);
}

std::pair<int, int> EditorView::selectionRange() const {
  const int start = sci_->selectionStart();
  const int end = sci_->selectionEnd();
  return {start, end};
}

void EditorView::setText(const QByteArray &t) { sci_->setText(t.constData()); }

void EditorView::setReadOnly(bool readOnly) { sci_->setReadOnly(readOnly); }

bool EditorView::isReadOnly() const { return sci_->readOnly(); }

int EditorView::cursorPos() const {
  return static_cast<int>(sci_->currentPos());
}

int EditorView::anchorPos() const { return static_cast<int>(sci_->anchor()); }

void EditorView::setCursorPos(int pos) { sci_->gotoPos(pos); }

void EditorView::setSelection(int anchor, int caret) {
  sci_->setSel(anchor, caret);
}

std::pair<int, int> EditorView::lineColFromPos(int pos) const {
  const int line = static_cast<int>(sci_->lineFromPosition(pos));
  const int lineStart = static_cast<int>(sci_->positionFromLine(line));
  return {line, pos - lineStart};
}

int EditorView::posFromLineCol(int line, int col) const {
  const int lineStart = static_cast<int>(sci_->positionFromLine(line));
  const int lineEnd = static_cast<int>(sci_->lineEndPosition(line));
  const int p = lineStart + col;
  return p > lineEnd ? lineEnd : p;
}

int EditorView::lineCount() const {
  return static_cast<int>(sci_->lineCount());
}

int EditorView::styleAt(int pos) const {
  // styleIndexAt, not styleAt: the latter goes through Document::StyleAt(),
  // which returns a signed char, so any style id above 127 (the CMake, TOML,
  // sh, and Python bands all are) would come back negative.
  return static_cast<int>(sci_->styleIndexAt(pos));
}

std::pair<int, int>
EditorView::rangeForDiagnostic(const LspDiagnostic &d) const {
  // The buffer may have been edited since the server produced this range, so
  // clamp rather than trust it. See lsp_position.h on why `character` is a
  // byte offset here.
  const int docEnd = static_cast<int>(sci_->textLength());
  const int lastLine = static_cast<int>(sci_->lineFromPosition(docEnd));

  auto resolve = [&](int line, int character) {
    const int l = qBound(0, line, lastLine);
    const int lineStart = static_cast<int>(sci_->positionFromLine(l));
    const int lineEnd = static_cast<int>(sci_->lineEndPosition(l));
    return qBound(lineStart, lineStart + character, lineEnd);
  };

  int start = resolve(d.startLine, d.startChar);
  int end = resolve(d.endLine, d.endChar);
  if (end < start)
    std::swap(start, end);
  // A zero-width range paints nothing. Widen it by one character so an
  // insertion-point diagnostic is still visible.
  if (end == start)
    end = qMin(start + 1, docEnd);
  return {start, end};
}

void EditorView::clearDiagnosticDecorations() {
  const int docEnd = static_cast<int>(sci_->textLength());
  for (int indicator : {diag::kErrorIndicator, diag::kWarningIndicator}) {
    sci_->setIndicatorCurrent(indicator);
    sci_->indicatorClearRange(0, docEnd);
  }
  sci_->markerDeleteAll(diag::kErrorMarker);
  sci_->markerDeleteAll(diag::kWarningMarker);
}

void EditorView::setDiagnostics(const QVector<LspDiagnostic> &diagnostics) {
  diagnostics_ = diagnostics;
  clearDiagnosticDecorations();

  // Squiggles are per-range and never collide with anything, so they are
  // still painted here. The gutter dot is not: it shares a margin now, so it
  // is refreshGutterMarkers() that decides whether this line's dot is the
  // diagnostic's or something with a stronger claim.
  for (const LspDiagnostic &d : diagnostics_) {
    const bool isError = d.severity <= LspDiagnostic::Error;
    const auto [start, end] = rangeForDiagnostic(d);
    if (end <= start)
      continue;

    sci_->setIndicatorCurrent(isError ? diag::kErrorIndicator
                                      : diag::kWarningIndicator);
    sci_->indicatorFillRange(start, end - start);
  }
  refreshGutterMarkers();
}

void EditorView::setBreakpointMarkers(const QVector<BreakpointMark> &marks) {
  bpMarks_ = marks;
  refreshGutterMarkers();
}

// One marker per line, chosen by precedence:
//
//   1. stopped here AND a breakpoint  -> the composite (ring + fill)
//   2. stopped here                   -> the execution marker
//   3. a breakpoint                   -> the breakpoint marker
//   4. an error, else a warning       -> the diagnostic marker
//
// Execution outranks a breakpoint because it is transient and answers "where
// am I", which is the question you have while it is true. A breakpoint
// outranks a diagnostic because you put it there deliberately, and because a
// diagnostic is not only in the gutter — the squiggle under the text says it
// too, so losing the dot on a breakpoint line loses no information.
void EditorView::refreshGutterMarkers() {
  for (int m :
       {diag::kErrorMarker, diag::kWarningMarker, dbg::kBreakpointMarker,
        dbg::kBreakpointDisabledMarker, dbg::kCurrentLineMarker,
        dbg::kSelectedFrameMarker, dbg::kBreakpointStoppedMarker,
        dbg::kBreakpointDisabledStoppedMarker}) {
    sci_->markerDeleteAll(m);
  }

  // Which lines the diagnostics claim, weakest precedence, computed first so
  // the stronger passes can simply overwrite the entry.
  QHash<int, int> markerForLine; // 0-based line -> marker number
  for (const LspDiagnostic &d : diagnostics_) {
    const auto [start, end] = rangeForDiagnostic(d);
    if (end <= start)
      continue;
    const int line0 = static_cast<int>(sci_->lineFromPosition(start));
    const bool isError = d.severity <= LspDiagnostic::Error;
    // An error on a line already claimed by a warning wins; the reverse
    // does not.
    auto it = markerForLine.find(line0);
    if (it != markerForLine.end() && !isError)
      continue;
    markerForLine[line0] = isError ? diag::kErrorMarker : diag::kWarningMarker;
  }

  const int execLine0 = execLine_ >= 1 ? execLine_ - 1 : -1;

  // Breakpoints, which also need their handles recorded for edit tracking.
  bpHandles_.clear();
  QSet<int> breakpointLines;
  for (const BreakpointMark &m : bpMarks_) {
    if (m.line < 1)
      continue;
    const int line0 = m.line - 1;
    // Pending (not yet verified) and disabled both render hollow — the
    // delay is visible rather than mysterious (constraint 6).
    const bool hollow = !m.enabled || m.pending;
    int marker;
    if (line0 == execLine0) {
      marker = hollow ? dbg::kBreakpointDisabledStoppedMarker
                      : dbg::kBreakpointStoppedMarker;
    } else {
      marker = hollow ? dbg::kBreakpointDisabledMarker : dbg::kBreakpointMarker;
    }
    markerForLine[line0] = marker;
    breakpointLines.insert(line0);
  }

  // The execution marker, only where no breakpoint already folded it into a
  // composite above.
  if (execLine0 >= 0 && !breakpointLines.contains(execLine0)) {
    markerForLine[execLine0] =
        execIsTopFrame_ ? dbg::kCurrentLineMarker : dbg::kSelectedFrameMarker;
  }

  for (auto it = markerForLine.constBegin(); it != markerForLine.constEnd();
       ++it) {
    const int handle = static_cast<int>(sci_->markerAdd(it.key(), it.value()));
    // `markerAdd` returns a handle Scintilla carries across insertions and
    // deletions. Keeping the handle beside the line is what makes a
    // breakpoint follow its statement when a line is inserted above it;
    // storing a bare line number and hoping is the classic bug. Only
    // breakpoint lines need tracking — a diagnostic is replaced wholesale
    // on the next analysis, and an execution position on the next stop.
    if (handle < 0)
      continue; // Scintilla refused (out of range)
    if (breakpointLines.contains(it.key())) {
      bpHandles_.append({handle, it.key() + 1});
    }
  }
}

void EditorView::updateLineNumberWidth() {
  // Measure the widest number this buffer can actually show, rather than
  // reserving a fixed slab. The old value was a flat 44px — room for five
  // digits, in an editor whose files are mostly three, and it never changed.
  //
  // `textWidth` measures in the line-number style, so this follows the theme
  // font and any future zoom rather than assuming Menlo 12.
  const int lines = qMax(1, static_cast<int>(sci_->lineCount()));
  const QByteArray widest(QByteArray::number(lines).size(), '9');
  const int width =
      static_cast<int>(sci_->textWidth(STYLE_LINENUMBER, widest.constData())) +
      margins::kLineNumberPadding;
  if (width == lineNumberWidth_)
    return; // the common case, per keystroke
  lineNumberWidth_ = width;
  sci_->setMarginWidthN(margins::kLineNumber, width);
}

void EditorView::reconcileBreakpointLines() {
  if (bpHandles_.isEmpty())
    return;
  QVector<QPair<int, int>> moves;
  for (BreakpointHandle &h : bpHandles_) {
    const int now = static_cast<int>(sci_->markerLineFromHandle(h.handle));
    // -1 means Scintilla no longer knows this handle.
    //
    // Measured, because the intuition is wrong: deleting the line a marker
    // sits on does *not* produce -1. Scintilla merges the removed line's
    // markers onto the line that takes its place, so a breakpoint on a
    // deleted line lands on the next statement rather than vanishing. That
    // is Scintilla's rule, it is a reasonable one, and it is left alone —
    // this branch covers only a handle genuinely dropped underneath us.
    if (now < 0) {
      moves.append({h.line, 0});
      h.line = 0;
      continue;
    }
    if (now + 1 != h.line) {
      moves.append({h.line, now + 1});
      h.line = now + 1;
    }
  }
  if (!moves.isEmpty())
    emit breakpointLinesMoved(moves);
}

void EditorView::setExecutionLine(int line, bool isTopFrame) {
  if (line < 1) {
    clearExecutionLine();
    return;
  }
  execLine_ = line;
  execIsTopFrame_ = isTopFrame;
  refreshGutterMarkers();
  // Reveal the line. gotoLine ensures it is visible without forcing it to
  // the top, which a stepper would fight against.
  sci_->gotoLine(line - 1);
}

void EditorView::clearExecutionLine() {
  if (execLine_ == 0)
    return;
  execLine_ = 0;
  execIsTopFrame_ = false;
  refreshGutterMarkers();
}

void EditorView::clearOccurrences() {
  if (occurrences_.isEmpty())
    return;
  occurrences_.clear();
  sci_->setIndicatorCurrent(occurrence::kIndicator);
  sci_->indicatorClearRange(0, sci_->textLength());
}

void EditorView::setOccurrences(const QVector<LspRange> &ranges) {
  // Clear unconditionally rather than via clearOccurrences(), which
  // short-circuits on an empty cache — the widget can still hold paint from a
  // set this object no longer remembers (a theme reapply, a reload).
  sci_->setIndicatorCurrent(occurrence::kIndicator);
  sci_->indicatorClearRange(0, sci_->textLength());
  occurrences_ = ranges;

  const int docEnd = static_cast<int>(sci_->textLength());
  for (const LspRange &r : occurrences_) {
    // Clamped into the document as it stands now: the buffer may have been
    // edited between the request and this reply, and an out-of-range fill
    // would either assert or paint garbage.
    const int start =
        qBound(0, posFromLineCol(r.startLine, r.startCharacter), docEnd);
    const int end =
        qBound(0, posFromLineCol(r.endLine, r.endCharacter), docEnd);
    if (end <= start)
      continue;
    sci_->indicatorFillRange(start, end - start);
  }
}

void EditorView::showRenameInput(const LspRange &range,
                                 const QString &placeholder) {
  if (!renameInput_) {
    // Parented to the Scintilla widget so it scrolls out of view with the
    // text rather than floating over a document that has moved underneath
    // it. Created lazily: most buffers never rename anything.
    renameInput_ = new QLineEdit(sci_);
    renameInput_->setFrame(true);
    renameInput_->hide();
    connect(renameInput_, &QLineEdit::returnPressed, this, [this] {
      const QString name = renameInput_->text().trimmed();
      hideRenameInput();
      if (!name.isEmpty())
        emit renameCommitted(name);
    });
    // Escape and focus loss both mean "never mind". Without the second, a
    // click into the editor would leave an orphaned box over the text.
    renameInput_->installEventFilter(this);
  }

  const int docEnd = static_cast<int>(sci_->textLength());
  const int start =
      qBound(0, posFromLineCol(range.startLine, range.startCharacter), docEnd);
  const int x = static_cast<int>(sci_->pointXFromPosition(start));
  const int y = static_cast<int>(sci_->pointYFromPosition(start));

  renameInput_->setFont(currentFont_);
  renameInput_->setText(placeholder);
  renameInput_->selectAll();
  // Wide enough for a longer name than the one being replaced, since that is
  // the usual direction of travel.
  const int width = qMax(
      120, renameInput_->fontMetrics().horizontalAdvance(placeholder) + 48);
  renameInput_->setGeometry(x, y, width, renameInput_->sizeHint().height());
  renameInput_->show();
  renameInput_->raise();
  renameInput_->setFocus(Qt::OtherFocusReason);
}

void EditorView::hideRenameInput() {
  if (!renameInput_ || !renameInput_->isVisible())
    return;
  renameInput_->hide();
  // Qualified, and it has to be: ScintillaEdit declares `setFocus(bool)` —
  // the SCI_SETFOCUS API — which hides every QWidget::setFocus overload.
  // Unqualified, `Qt::OtherFocusReason` converts to `true` and this becomes
  // SCI_SETFOCUS, flipping Scintilla's internal flag while Qt keyboard focus
  // stays on the rename box that was just hidden. GCC catches the conversion
  // (-Werror=int-in-bool-context) and broke the v0.2.0 Linux release build;
  // clang compiled it silently, which is how it shipped.
  sci_->QWidget::setFocus(Qt::OtherFocusReason);
}

bool EditorView::renameInputVisible() const {
  return renameInput_ && renameInput_->isVisible();
}

QString EditorView::renameInputText() const {
  return renameInput_ ? renameInput_->text() : QString();
}

bool EditorView::eventFilter(QObject *watched, QEvent *event) {
  if (watched == renameInput_) {
    if (event->type() == QEvent::KeyPress) {
      auto *key = static_cast<QKeyEvent *>(event);
      if (key->key() == Qt::Key_Escape) {
        hideRenameInput();
        emit renameCancelled();
        return true;
      }
    } else if (event->type() == QEvent::FocusOut) {
      hideRenameInput();
      emit renameCancelled();
    }
  }
  return TabContent::eventFilter(watched, event);
}

QString EditorView::diagnosticMessageAt(int pos) const {
  const LspDiagnostic *d = diagnosticAt(pos);
  return d ? d->message : QString();
}

const LspDiagnostic *EditorView::diagnosticAt(int pos) const {
  for (const LspDiagnostic &d : diagnostics_) {
    const auto [start, end] = rangeForDiagnostic(d);
    if (pos >= start && pos <= end)
      return &d;
  }
  return nullptr;
}

void EditorView::showCompletions(const QStringList &labels, int lengthEntered) {
  if (labels.isEmpty()) {
    sci_->autoCCancel();
    return;
  }
  // Scintilla splits on the separator and expects the list pre-sorted; the
  // server returns document order, so sort here.
  QStringList sorted = labels;
  sorted.sort();
  sorted.removeDuplicates();

  sci_->autoCSetSeparator('\n');
  // Don't steal Enter/Tab when the only candidate is what the user already
  // typed — that would turn a deliberate newline into an acceptance.
  sci_->autoCSetChooseSingle(false);
  sci_->autoCSetIgnoreCase(false);
  sci_->autoCShow(lengthEntered, sorted.join('\n').toUtf8().constData());
}

std::pair<int, int> EditorView::enclosingBracketPair(int pos) const {
  const int docEnd = static_cast<int>(sci_->textLength());
  pos = qBound(0, pos, docEnd);

  // Backward: find the nearest opener the caret is inside of. A closer seen
  // on the way means a whole pair was passed over, so it cancels one opener.
  int opener = -1;
  int depth = 0;
  const int backStop = qMax(0, pos - kBracketScanLimit);
  for (int i = pos - 1; i >= backStop; --i) {
    const char c = static_cast<char>(sci_->charAt(i));
    if (!IsBracketChar(c) || !IsCodeStyle(styleAt(i)))
      continue;
    if (IsOpenBracket(c)) {
      if (depth == 0) {
        opener = i;
        break;
      }
      --depth;
    } else {
      ++depth;
    }
  }
  if (opener < 0)
    return {-1, -1};

  // Forward: the matching closer. Counted rather than read off the style —
  // rainbow styles cycle mod 7, so depth 0 and depth 7 are the same colour
  // and a style comparison would match the wrong bracket on a deep form.
  int closer = -1;
  depth = 0;
  const int fwdStop = qMin(docEnd, opener + 1 + kBracketScanLimit);
  for (int i = opener + 1; i < fwdStop; ++i) {
    const char c = static_cast<char>(sci_->charAt(i));
    if (!IsBracketChar(c) || !IsCodeStyle(styleAt(i)))
      continue;
    if (IsOpenBracket(c)) {
      ++depth;
    } else if (depth == 0) {
      closer = i;
      break;
    } else {
      --depth;
    }
  }
  if (closer < 0)
    return {-1, -1}; // unbalanced, or past the scan bound
  return {opener, closer};
}

void EditorView::clearBracketGuide() {
  bracketGuideSpan_ = {-1, -1};
  guideOpener_ = guideCloser_ = -1;
  guideLine_ = GuideLine{};
  gutterBar_ = GuideLine{};
  if (guideOverlay_) {
    guideOverlay_->clearLine();
    guideOverlay_->clearGutterBar();
  }
  sci_->setIndicatorCurrent(bracketguide::kIndicator);
  sci_->indicatorClearRange(0, sci_->textLength());
}

void EditorView::repositionBracketGuideOverlay() {
  if (!guideOverlay_)
    return;
  guideLine_ = GuideLine{};
  gutterBar_ = GuideLine{};
  if (guideOpener_ < 0 || guideCloser_ < 0) {
    guideOverlay_->clearLine();
    guideOverlay_->clearGutterBar();
    return;
  }

  const int openerLine = static_cast<int>(sci_->lineFromPosition(guideOpener_));
  const int closerLine = static_cast<int>(sci_->lineFromPosition(guideCloser_));

  guideOverlay_->setGeometry(sci_->viewport()->rect());

  const int styleForColor =
      rainbow_ ? styleAt(guideCloser_) : static_cast<int>(STYLE_INDENTGUIDE);
  const sptr_t bgr = sci_->styleFore(styleForColor);
  // Scintilla stores colours as 0xBBGGRR, the reverse of QColor's argument
  // order, so the channels are unpacked rather than cast.
  const QColor color(static_cast<int>(bgr & 0xFF),
                     static_cast<int>((bgr >> 8) & 0xFF),
                     static_cast<int>((bgr >> 16) & 0xFF));

  // The gutter bar covers the lines the expression *occupies*, closer's line
  // included — it is a row range, not a span between two points, so it ends
  // at the bottom of the closer's line rather than at its top. A single-line
  // pair therefore still gets one row of bar, which is the whole point: it is
  // the one decoration that says which lines without needing an extent.
  const int barTop = static_cast<int>(sci_->pointYFromPosition(
      static_cast<int>(sci_->positionFromLine(openerLine))));
  const int barBottom =
      static_cast<int>(sci_->pointYFromPosition(
          static_cast<int>(sci_->positionFromLine(closerLine)))) +
      static_cast<int>(sci_->textHeight(closerLine)) *
          static_cast<int>(sci_->wrapCount(closerLine));
  // Right-aligned against the last margin, so it sits on the gutter/text
  // boundary and reads as an edge marker rather than as another indent guide.
  int marginsWidth = 0;
  for (int m = 0; m <= SC_MAX_MARGIN; ++m) {
    marginsWidth += static_cast<int>(sci_->marginWidthN(m));
  }
  const int barX = qMax(0, marginsWidth - kGutterBarWidth);
  // Clipped to the viewport: a form taller than the window would otherwise
  // hand Qt a rect reaching far past either edge.
  const int clippedTop = qMax(0, barTop);
  const int clippedBottom = qMin(sci_->viewport()->height(), barBottom);
  if (clippedBottom > clippedTop) {
    guideOverlay_->setGutterBar(
        QRect(barX, clippedTop, kGutterBarWidth, clippedBottom - clippedTop),
        color);
    gutterBar_ = GuideLine{true, barX, clippedTop, clippedBottom};
  } else {
    guideOverlay_->clearGutterBar();
  }

  // The spine, in the opener's own column. A single-line pair has no vertical
  // extent; the horizontal segment already says everything there is to say
  // about it.
  if (openerLine == closerLine) {
    guideOverlay_->clearLine();
    guideOverlay_->raise();
    return;
  }

  const int x = static_cast<int>(sci_->pointXFromPosition(guideOpener_));
  // From the top of the opener's line to the top of the closer's, which is
  // Monaco's extent: it emits a guide *on* each line from the opener's
  // through the one before the closer, rather than in the gap between them.
  //
  // The distinction is not cosmetic. For the overwhelmingly common lisp shape
  // — a form whose closer sits on the very next line — a gap-based spine has
  // zero height and vanishes exactly where it is most wanted.
  const int top = static_cast<int>(sci_->pointYFromPosition(guideOpener_));
  const int bottom = static_cast<int>(sci_->pointYFromPosition(guideCloser_));
  if (bottom <= top) {
    guideOverlay_->clearLine();
    guideOverlay_->raise();
    return;
  }

  guideOverlay_->setLine(x, top, bottom, color);
  guideOverlay_->raise();
  guideLine_ = GuideLine{true, x, top, bottom};
}

void EditorView::updateBracketGuide() {
  clearBracketGuide();
  if (!bracketGuides_)
    return;
  // Only the lisps carry rainbow depth styles, and the scan's comment/string
  // skipping is written against TurStyle. Other languages get nothing rather
  // than a guide computed from styles that mean something else.
  if (language_ != Language::Turmeric)
    return;

  const auto [opener, closer] = enclosingBracketPair(cursorPos());
  if (opener < 0 || closer < 0)
    return; // top level, or the scan gave up

  const int openerLine = static_cast<int>(sci_->lineFromPosition(opener));
  const int closerLine = static_cast<int>(sci_->lineFromPosition(closer));

  int start = 0;
  if (openerLine == closerLine) {
    // Monaco draws a single-line pair's segment between the brackets rather
    // than under them (guidesTextModelPart.js: opener's *end* column to the
    // closer's column), and it does draw one — `includeSingleLinePairs` is
    // hardcoded true. Matched here rather than reinvented.
    start = opener + 1;
  } else {
    // Multi-line: the segment sits on the closing line, running from the
    // guide column to the closer. Monaco's guide column is the smaller of
    // the two bracket columns, which keeps the line inside the text when
    // the closer is indented past its opener.
    const int openerCol =
        opener - static_cast<int>(sci_->positionFromLine(openerLine));
    const int closerLineStart =
        static_cast<int>(sci_->positionFromLine(closerLine));
    const int closerCol = closer - closerLineStart;
    start = closerLineStart + qMin(openerCol, closerCol);
  }
  // Closer inclusive: the segment should visibly terminate at the bracket it
  // belongs to rather than stopping one character short of it.
  const int end = closer + 1;
  if (end <= start)
    return;

  // The pair's own depth colour, read back off the closing bracket's style.
  // Opener and closer share a style by construction (the scanner increments
  // after emitting an opener and decrements before emitting a closer), so
  // this cannot drift out of sync with the parens it belongs to.
  //
  // With rainbow brackets off there is no depth colour — brackets fall back
  // to the flat Delim/CurlyInfix styles — so the guide takes the neutral
  // indent-guide colour instead of hiding. Hiding would tie two unrelated
  // preferences together, and the guide's job (showing how far the current
  // expression reaches) is just as useful in one colour.
  const int styleForColor =
      rainbow_ ? styleAt(closer) : static_cast<int>(STYLE_INDENTGUIDE);
  sci_->indicSetFore(bracketguide::kIndicator, sci_->styleFore(styleForColor));

  sci_->setIndicatorCurrent(bracketguide::kIndicator);
  sci_->indicatorFillRange(start, end - start);
  bracketGuideSpan_ = {start, end};

  guideOpener_ = opener;
  guideCloser_ = closer;
  repositionBracketGuideOverlay();
}

bool EditorView::bracketPairGuidesDefault() {
  return Settings::instance().bracketPairGuides();
}

void EditorView::setBracketPairGuides(bool enabled) {
  bracketGuides_ = enabled;
  updateBracketGuide();
}

int EditorView::symbolIndexAtCaret(const QVector<LspSymbol> &symbols) const {
  const auto [line, col] = lineColFromPos(cursorPos());

  // Pass one: the caret is literally on a name. That is unambiguous and beats
  // any containment answer, including a nested one.
  for (int i = 0; i < symbols.size(); ++i) {
    if (symbols.at(i).selection.contains(line, col))
      return i;
  }

  // Pass two: the smallest range that contains the caret. Smallest, because
  // with nesting the innermost definition is the one you are editing.
  //
  // Dead against the pinned server, and deliberately kept: v0.42.0 reports
  // `range` identical to `selectionRange` for every symbol — both span the
  // name alone, never the definition's body — so nothing can contain a caret
  // that pass one did not already claim. This is the rule c2mp actually uses
  // and it starts working the day the server reports real extents.
  int best = -1;
  long long bestSpan = 0;
  for (int i = 0; i < symbols.size(); ++i) {
    const LspRange &r = symbols.at(i).range;
    if (!r.contains(line, col))
      continue;
    // Line count dominates; the column difference only breaks ties within
    // a single line, which is why it is added rather than compared.
    const long long span =
        (static_cast<long long>(r.endLine - r.startLine) << 20) +
        (r.endCharacter - r.startCharacter);
    if (best < 0 || span < bestSpan) {
      best = i;
      bestSpan = span;
    }
  }
  if (best >= 0)
    return best;

  // Pass three: the last definition starting at or before the caret.
  //
  // Only reachable because pass two is currently dead. It answers "which
  // top-level form am I in" from position alone, which is exactly right for
  // a file of top-level definitions and degrades to "the one above me" for
  // anything else. Note this is positional arithmetic over ranges the server
  // gave us, not a guess about meaning — unlike the textual occurrence
  // matching §7 rejects, it cannot point at something unrelated.
  for (int i = symbols.size() - 1; i >= 0; --i) {
    const LspRange &r = symbols.at(i).selection;
    if (r.startLine < line ||
        (r.startLine == line && r.startCharacter <= col)) {
      return i;
    }
  }
  return -1;
}

void EditorView::showSymbolList(const QVector<LspSymbol> &symbols) {
  outlineSymbols_ = symbols;
  outlineRows_.clear();
  if (symbols.isEmpty()) {
    sci_->autoCCancel();
    return;
  }

  for (const LspSymbol &sym : symbols) {
    const QString kind = LspSymbolKindLabel(sym.kind);
    outlineRows_ << (kind.isEmpty()
                         ? sym.name
                         : sym.name + QString::fromUtf8(kOutlineKindSeparator) +
                               kind);
  }

  const int current = symbolIndexAtCaret(symbols);

  sci_->autoCSetSeparator('\n');
  // Document order, not alphabetical. Scintilla sorts by default and would
  // otherwise scramble the one property the outline exists to show.
  sci_->autoCSetOrder(SC_ORDER_CUSTOM);
  sci_->autoCSetIgnoreCase(false);
  sci_->autoCSetChooseSingle(false);
  // lengthEntered 0: the outline is not completing a prefix at the caret, so
  // nothing in the buffer should be treated as already typed.
  sci_->userListShow(kOutlineListType,
                     outlineRows_.join('\n').toUtf8().constData());
  if (current >= 0) {
    sci_->autoCSelect(outlineRows_.at(current).toUtf8().constData());
  }
}

void EditorView::showChooserList(const QStringList &rows) {
  chooserRows_ = rows;
  if (rows.isEmpty()) {
    sci_->autoCCancel();
    return;
  }
  sci_->autoCSetSeparator('\n');
  sci_->autoCSetOrder(SC_ORDER_CUSTOM);
  sci_->autoCSetIgnoreCase(false);
  sci_->autoCSetChooseSingle(false);
  sci_->userListShow(kChooserListType, rows.join('\n').toUtf8().constData());
}

void EditorView::beginEditGroup() { sci_->beginUndoAction(); }
void EditorView::endEditGroup() { sci_->endUndoAction(); }

void EditorView::replaceRange(const LspRange &range, const QString &text) {
  const int docEnd = static_cast<int>(sci_->textLength());
  const int start =
      qBound(0, posFromLineCol(range.startLine, range.startCharacter), docEnd);
  const int end =
      qBound(start, posFromLineCol(range.endLine, range.endCharacter), docEnd);
  // The target range, not the selection: replaceSel would move the caret to
  // every edited site in turn and leave it wherever the last one happened to
  // be, which for a multi-site rename is nowhere the user was.
  sci_->setTargetRange(start, end);
  const QByteArray utf8 = text.toUtf8();
  sci_->replaceTarget(utf8.size(), utf8.constData());
}

void EditorView::showOutlineMessage(const QString &message) {
  outlineSymbols_.clear();
  outlineRows_.clear();
  if (message.isEmpty()) {
    sci_->autoCCancel();
    return;
  }
  sci_->autoCSetSeparator('\n');
  sci_->autoCSetOrder(SC_ORDER_CUSTOM);
  sci_->autoCSetChooseSingle(false);
  // A separate list type, so the selection handler ignores a click on it. A
  // user list cannot disable a row, so making the row inert is the next best
  // thing to greying it out.
  sci_->userListShow(kOutlineMessageListType, message.toUtf8().constData());
}

void EditorView::showHover(int pos, const QString &markdown) {
  // The server wraps signatures in ``` fences and the docstring below them.
  // Call tips are plain text, so drop the fence lines rather than showing
  // literal backticks.
  QStringList lines;
  for (const QString &line : markdown.split('\n')) {
    if (line.trimmed().startsWith(QLatin1String("```")))
      continue;
    lines << line;
  }
  while (!lines.isEmpty() && lines.first().trimmed().isEmpty())
    lines.removeFirst();
  while (!lines.isEmpty() && lines.last().trimmed().isEmpty())
    lines.removeLast();
  if (lines.isEmpty())
    return;

  callTipText_ = lines.join('\n');
  sci_->callTipShow(pos, callTipText_.toUtf8().constData());
}

void EditorView::showSignatureHelp(int pos, const QString &text) {
  // The same call-tip surface as hover, and deliberately so: one popup
  // convention for "what is this thing", whether you asked by dwelling on a
  // name or by opening a call.
  //
  // Cancelled first rather than shown over the top. A tip left from the
  // enclosing call would otherwise sit there while a nested call is typed,
  // describing the wrong callee -- and Scintilla keeps the first one.
  if (text.trimmed().isEmpty())
    return;
  sci_->callTipCancel();
  callTipText_ = text;
  sci_->callTipShow(pos, callTipText_.toUtf8().constData());
}

QString EditorView::callTipText() const {
  return (sci_ && sci_->callTipActive()) ? callTipText_ : QString();
}

void EditorView::setPath(const QString &path) {
  if (path_ == path)
    return;
  path_ = path;
  // Covers opening a file into a fresh tab as well as Save As to a new
  // extension.
  refreshLanguage();
  emit filePathChanged(path_);
}

void EditorView::refreshLanguage() {
  const Language lang = LanguageForBuffer(path_, languageProbeText());
  if (lang == language_)
    return;
  language_ = lang;
  installLexer();
  applyWrap();       // re-resolve wrap default for the new language class
  applyFoldMargin(); // show/hide fold margin for the new language
}

Dialect EditorView::dialect() const {
  // The same two-line probe `refreshLanguage` uses: a `#lang` line is on line
  // 1, or line 2 behind a `#!` shebang, and nowhere else.
  return DialectForBuffer(path_, languageProbeText());
}

bool EditorView::setLangDirective(Dialect d) {
  const QByteArray text = this->text();
  int start = 0;
  int end = 0;
  const bool had = LangDirectiveSpan(text, start, end);

  if (!had && d == Dialect::Turmeric)
    return false; // nothing to write

  // A header is removed, not rewritten, when the selection returns to the
  // default: `#lang turmeric` is what a file with no header already means,
  // and leaving one behind would make the picker's "off" state visible in the
  // source forever.
  if (d == Dialect::Turmeric) {
    int cut = end;
    // Take the blank line an insert would have added back with it. Only one,
    // and only when it is blank -- a file whose real second line happens to
    // be empty keeps it.
    if (cut < text.size() && text[cut] == '\n') {
      cut += 1;
    } else if (cut + 1 < text.size() && text[cut] == '\r' &&
               text[cut + 1] == '\n') {
      cut += 2;
    }
    beginEditGroup();
    sci_->deleteRange(start, cut - start);
    endEditGroup();
    refreshLanguage();
    return true;
  }

  const QByteArray line = QByteArray("#lang ") + DialectBaseToken(d) + "\n";

  beginEditGroup();
  if (had) {
    // Replace exactly the directive line. Everything after it, blank line
    // included, is left alone.
    sci_->deleteRange(start, end - start);
    sci_->insertText(start, line.constData());
  } else {
    // New header at the very top, followed by a blank line so the first
    // real form is not glued to it.
    sci_->insertText(0, (line + "\n").constData());
  }
  endEditGroup();
  refreshLanguage();
  return true;
}

QByteArray EditorView::langDirectiveLine() const {
  return LangDirectiveLine(languageProbeText());
}

QByteArray EditorView::languageProbeText() const {
  // Two lines, not one: a `#lang` directive may sit on line 2 when line 1 is
  // a `#!` shebang. Bounding the probe keeps this affordable on every edit.
  const int probeLines = 2;
  const int end = (lineCount() > probeLines)
                      ? static_cast<int>(sci_->positionFromLine(probeLines))
                      : static_cast<int>(sci_->textLength());
  return textInRange(0, end);
}

// --- Find / Replace (Phase 3) --------------------------------------------

void EditorView::runFindSearch() {
  // Uses findQuery_ directly — it is synced from the bar by the
  // searchChanged handler, and from setFindQuery by the control API.
  if (findQuery_.text.isEmpty()) {
    findResult_ = {};
    if (findBar_ && findBar_->isVisible()) {
      findBar_->setMatchCount(0, 0);
      findBar_->setError({});
    }
    clearFindHighlights();
    return;
  }

  const int caret = cursorPos();
  findResult_ = FindAll(sci_, findQuery_, caret);

  if (findBar_ && findBar_->isVisible()) {
    findBar_->setError(findResult_.error);
    findBar_->setMatchCount(findResult_.currentIndex, findResult_.count);
  }
  highlightFindMatches();
  selectCurrentMatch();
}

void EditorView::highlightFindMatches() {
  sci_->setIndicatorCurrent(find::kMatchIndicator);
  sci_->indicatorClearRange(0, sci_->textLength());
  for (const auto &m : findResult_.matches) {
    sci_->indicatorFillRange(m.start, m.end - m.start);
  }
}

void EditorView::selectCurrentMatch() {
  if (findResult_.currentIndex < 0 ||
      findResult_.currentIndex >= findResult_.matches.size())
    return;
  const auto &m = findResult_.matches[findResult_.currentIndex];
  sci_->setSelection(m.end, m.start);
  revealLine(sci_->lineFromPosition(m.start));
  // Update the current-match indicator.
  sci_->setIndicatorCurrent(find::kCurrentIndicator);
  sci_->indicatorClearRange(0, sci_->textLength());
  sci_->indicatorFillRange(m.start, m.end - m.start);
}

void EditorView::clearFindHighlights() {
  sci_->setIndicatorCurrent(find::kMatchIndicator);
  sci_->indicatorClearRange(0, sci_->textLength());
  sci_->setIndicatorCurrent(find::kCurrentIndicator);
  sci_->indicatorClearRange(0, sci_->textLength());
}

void EditorView::showFind(bool replace) {
  if (!findBar_)
    return;
  // Capture the selection for In Selection mode and for pre-filling the query.
  const int selStart = sci_->selectionStart();
  const int selEnd = sci_->selectionEnd();
  const int lineStart = sci_->lineFromPosition(selStart);
  const int lineEnd = sci_->lineFromPosition(selEnd);
  if (selStart != selEnd && lineStart == lineEnd) {
    // Single-line selection → pre-fill the query.
    findBar_->setQuery(QString::fromUtf8(sci_->textRange(selStart, selEnd)));
  }
  findSelectionStart_ = selStart;
  findSelectionEnd_ = selEnd;
  findBar_->open(replace);
  runFindSearch();
}

void EditorView::hideFind() {
  if (!findBar_)
    return;
  findBar_->closeBar();
  clearFindHighlights();
  sci_->QWidget::setFocus(Qt::OtherFocusReason);
}

bool EditorView::findBarVisible() const {
  return findBar_ && findBar_->isVisible();
}

void EditorView::findNext() {
  if (findQuery_.text.isEmpty())
    return;
  const int caret = cursorPos();
  FindMatch m = FindNext(sci_, findQuery_, caret);
  if (m.start < 0) {
    // Wrap around.
    m = FindNext(sci_, findQuery_, 0);
  }
  if (m.start >= 0) {
    sci_->setSelection(m.end, m.start);
    revealLine(sci_->lineFromPosition(m.start));
    // Update current index for the count display.
    for (int i = 0; i < findResult_.matches.size(); ++i) {
      if (findResult_.matches[i].start == m.start) {
        findResult_.currentIndex = i;
        if (findBar_ && findBar_->isVisible())
          findBar_->setMatchCount(i, findResult_.count);
        break;
      }
    }
    // Update the current-match indicator.
    sci_->setIndicatorCurrent(find::kCurrentIndicator);
    sci_->indicatorClearRange(0, sci_->textLength());
    sci_->indicatorFillRange(m.start, m.end - m.start);
  }
}

void EditorView::findPrevious() {
  if (findQuery_.text.isEmpty())
    return;
  const int caret = cursorPos();
  FindMatch m = FindPrevious(sci_, findQuery_, caret);
  if (m.start < 0) {
    // Wrap around.
    m = FindPrevious(sci_, findQuery_, sci_->textLength());
  }
  if (m.start >= 0) {
    sci_->setSelection(m.end, m.start);
    revealLine(sci_->lineFromPosition(m.start));
    for (int i = 0; i < findResult_.matches.size(); ++i) {
      if (findResult_.matches[i].start == m.start) {
        findResult_.currentIndex = i;
        if (findBar_ && findBar_->isVisible())
          findBar_->setMatchCount(i, findResult_.count);
        break;
      }
    }
    sci_->setIndicatorCurrent(find::kCurrentIndicator);
    sci_->indicatorClearRange(0, sci_->textLength());
    sci_->indicatorFillRange(m.start, m.end - m.start);
  }
}

void EditorView::useSelectionForFind() {
  const int selStart = sci_->selectionStart();
  const int selEnd = sci_->selectionEnd();
  if (selStart != selEnd) {
    findBar_->setQuery(QString::fromUtf8(sci_->textRange(selStart, selEnd)));
    findQuery_.text = findBar_->query();
  }
}

void EditorView::selectAllOccurrences() {
  if (findQuery_.text.isEmpty())
    return;
  // Re-run the search to get all matches.
  findResult_ = FindAll(sci_, findQuery_, 0);
  if (findResult_.matches.isEmpty())
    return;

  // Build a multi-selection from all matches.
  sci_->clearSelections();
  sci_->setSelection(findResult_.matches[0].end, findResult_.matches[0].start);
  for (int i = 1; i < findResult_.matches.size(); ++i) {
    sci_->addSelection(findResult_.matches[i].end,
                       findResult_.matches[i].start);
  }
}

void EditorView::setFindQuery(const QString &text, bool matchCase,
                              bool wholeWord, bool regex, bool inSelection) {
  if (!findBar_)
    return;
  findBar_->setQuery(text);
  findBar_->setFlags(matchCase, wholeWord, regex, inSelection);
  findQuery_.text = text;
  findQuery_.matchCase = matchCase;
  findQuery_.wholeWord = wholeWord;
  findQuery_.regex = regex;
  findQuery_.inSelection = inSelection;
  if (inSelection) {
    // Capture the current selection as the search range, since
    // showFind() may not have been called yet.
    findSelectionStart_ = sci_->selectionStart();
    findSelectionEnd_ = sci_->selectionEnd();
    findQuery_.rangeStart = findSelectionStart_;
    findQuery_.rangeEnd = findSelectionEnd_;
  }
  runFindSearch();
}

int EditorView::replaceCurrent(const QString &replacement) {
  if (findResult_.currentIndex < 0 ||
      findResult_.currentIndex >= findResult_.matches.size())
    return 0;
  const auto &m = findResult_.matches[findResult_.currentIndex];
  sci_->setTargetRange(m.start, m.end);
  if (findQuery_.regex) {
    // Re-run the search on this match's range so replaceTargetRE
    // resolves capture groups for *this* match.
    const QByteArray qb = findQuery_.text.toUtf8();
    sci_->searchInTarget(qb.length(), qb.constData());
  }
  ReplaceTarget(sci_, replacement, findQuery_.regex);
  // Move to the next match.
  findNext();
  return 1;
}

int EditorView::replaceAll(const QString &replacement) {
  if (findQuery_.text.isEmpty())
    return 0;

  // Re-run the search to get all matches.
  findResult_ = FindAll(sci_, findQuery_, 0);
  if (findResult_.matches.isEmpty())
    return 0;

  sci_->beginUndoAction();
  int count = 0;
  const QByteArray qb = findQuery_.text.toUtf8();
  // Replace in descending order so earlier positions are not invalidated.
  for (int i = findResult_.matches.size() - 1; i >= 0; --i) {
    const auto &m = findResult_.matches[i];
    sci_->setTargetRange(m.start, m.end);
    if (findQuery_.regex) {
      // Re-run the search on this match's range so replaceTargetRE
      // resolves capture groups for *this* match, not the last one
      // found by FindAll.
      sci_->searchInTarget(qb.length(), qb.constData());
    }
    ReplaceTarget(sci_, replacement, findQuery_.regex);
    ++count;
  }
  sci_->endUndoAction();

  // Re-run the search to update highlights.
  runFindSearch();
  return count;
}

EditorView::FindState EditorView::findState() const {
  FindState s;
  s.open = findBar_ && findBar_->isVisible();
  s.query = findQuery_.text;
  s.matchCase = findQuery_.matchCase;
  s.wholeWord = findQuery_.wholeWord;
  s.regex = findQuery_.regex;
  s.inSelection = findQuery_.inSelection;
  s.count = findResult_.count;
  s.current = findResult_.currentIndex;
  s.error = findResult_.error;
  return s;
}

void EditorView::revealLine(int line) {
  // Unfold the target line if it is inside a folded region, then scroll
  // it into view.
  sci_->ensureVisibleEnforcePolicy(line);
  const int visibleStart = sci_->firstVisibleLine();
  const int linesOnScreen = sci_->linesOnScreen();
  if (line < visibleStart || line >= visibleStart + linesOnScreen) {
    sci_->scrollRange(0, line);
  }
}

} // namespace trowel
