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
//
// Not a Scintilla margin: margins can only draw markers, so this is a sibling
// widget inside EditorView's layout and talks to `sci_` through its public
// API. See docs/plans/minimap.md for the full design.
class MinimapView : public QWidget {
  Q_OBJECT
public:
  explicit MinimapView(ScintillaEdit *sci, QWidget *parent = nullptr);

  void setTheme(const Theme &theme);

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
  // Recompute the slider position from the editor's scroll. Never re-renders.
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
  static constexpr int kPhase1LineCap = 200000;

  struct Strip {
    int firstLine = 0;
    QImage image;
    bool dirty = true;
  };

  Strip &stripFor(int docLine);
  void renderStrip(Strip &s);

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
  QHash<int, QColor> styleFg_;
  QColor bg_, sliderBg_, sliderHoverBg_, sliderActiveBg_;
  int colPx_ = 1, linePx_ = 2;
  int widthPx_ = 90;
  bool enabled_ = false;
  bool dragging_ = false;
  bool hovered_ = false;
  int dragGrabDy_ = 0;
};

} // namespace trowel
