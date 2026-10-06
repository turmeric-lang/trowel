#pragma once

#include "editor/theme_loader.h"

#include <QHash>
#include <QImage>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include <Scintilla.h>

class ScintillaEdit;

namespace trowel {

// Forward-declared to avoid pulling lexers.h (and its ILexer.h include) into
// the header, which the MOC processes. The enumerators are only needed in
// the .cpp.
enum class Language : int;
struct SyntaxDescriptor;

// A block-rendered overview of the whole buffer, drawn beside the editor.
//
// Not a Scintilla margin: margins can only draw markers, so this is a sibling
// widget inside EditorView's layout and talks to `sci_` through its public
// API. See docs/plans/minimap.md for the full design.
//
// Phase 3: strip rendering runs off the GUI thread via QtConcurrent. The
// worker receives a copy of the strip text, the packed LexState at the
// strip's first line, and the style→color table; it drives the scanners
// itself and produces a QImage. No Scintilla access happens off-thread.
class MinimapView : public QWidget {
  Q_OBJECT
public:
  explicit MinimapView(ScintillaEdit *sci, QWidget *parent = nullptr);

  void setTheme(const Theme &theme);

  // Set the language and rainbow-bracket preference, so the off-thread
  // renderer can dispatch to the right scanner without touching Scintilla.
  // Called by EditorView whenever the lexer is (re)installed.
  void setLanguage(Language lang, bool rainbow);

  // Re-read the enabled/side/width settings and re-parent in the layout.
  // Called by EditorView when the preference changes.
  void applySettings(bool enabled, int widthPx);

  // The minimap reports its own state for the control API.
  struct State {
    bool enabled = false;
    int sliderTop = 0;
    int sliderHeight = 0;
    int topLine = 0;
  };
  State state() const;

public slots:
  void invalidateAll();
  // `lastLine < 0` means "to end of document". Lexer state cascades forward,
  // so an edit anywhere conservatively dirties everything below it.
  void invalidateLines(int firstLine, int lastLine);
  // Recompute the slider position from the editor's scroll. Also checks
  // whether Scintilla has styled new regions and invalidates the
  // corresponding strips. Never re-renders directly.
  void syncToEditorScroll();

protected:
  void paintEvent(QPaintEvent *) override;
  void mousePressEvent(QMouseEvent *) override;
  void mouseMoveEvent(QMouseEvent *) override;
  void mouseReleaseEvent(QMouseEvent *) override;
  void wheelEvent(QWheelEvent *) override;
  void resizeEvent(QResizeEvent *) override;
  void leaveEvent(QEvent *) override;

private:
  static constexpr int kStripLines = 512;
  // Maximum number of rendered (non-null-image) strips kept in memory. The
  // visible window is 2-3 strips; the rest are evicted to bound memory on
  // large files. Each strip image is ~widthPx * kStripLines * linePx * 4
  // bytes, so 16 strips is a few MB.
  static constexpr int kMaxStrips = 16;

  struct Strip {
    int firstLine = 0;
    QImage image;
    bool dirty = true;
    bool rendering = false; // true while an off-thread render is in flight
    quint64 lastUsed = 0;   // bumped on each access for LRU eviction
  };

  Strip &stripFor(int docLine);
  // Extract the data needed for off-thread rendering from Scintilla (GUI
  // thread), then launch a QtConcurrent worker. The strip is marked
  // rendering; on completion the image is stored and update() is called.
  void launchRender(Strip &s);
  // Drop the oldest rendered strips until at most kMaxStrips remain. Called
  // at the end of paintEvent so the cap tracks the working set, not the
  // document size.
  void evictStrips();

  // The whole doc-line <-> pixel mapping funnels through these two. Wrap
  // and folding are both on now, so they go through visible/doc-line
  // conversion rather than being the identity. No other member may do its
  // own line<->pixel arithmetic.
  int docLineForY(int y) const;
  int yForDocLine(int line) const;

  int displayLineCount() const;
  int topDocLine() const;
  QRect sliderRect() const;
  void scrollEditorToY(int y, bool center);

  ScintillaEdit *sci_;
  QVector<Strip> strips_;
  quint64 generation_ = 0; // monotonic access counter for LRU eviction
  QHash<int, QColor> styleFg_;
  QColor bg_, sliderBg_, sliderHoverBg_, sliderActiveBg_;
  int colPx_ = 1, linePx_ = 2;
  int widthPx_ = 90;
  bool enabled_ = false;
  bool dragging_ = false;
  bool hovered_ = false;
  int dragGrabDy_ = 0;
  QTimer renderDebounce_; // coalesces bursts of invalidations into one repaint
  int lang_ = 0; // Language, stored as int to avoid including lexers.h here
  bool rainbow_ = true;
  // For PluginSyntax: the descriptor captured at setLanguage time, so the
  // off-thread worker can call ScanPluginSyntaxLine directly without the
  // global g_pluginSyntaxDescIndex lookup (which would race with other
  // editors). Null for all other languages.
  const SyntaxDescriptor *pluginDesc_ = nullptr;
  // Tracks how far Scintilla has styled. When the editor scrolls into an
  // unvisited region, Scintilla styles it and endStyled() advances; the
  // newly-styled strips are invalidated so they re-render with correct
  // styles instead of the default-state fallback.
  sptr_t lastStyledEnd_ = 0;
};

} // namespace trowel
