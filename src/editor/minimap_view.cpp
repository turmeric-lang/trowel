#include "editor/minimap_view.h"

#include <Scintilla.h>
#include <ScintillaEdit.h>

#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

namespace trowel {

MinimapView::MinimapView(ScintillaEdit *sci, QWidget *parent)
    : QWidget(parent), sci_(sci) {
  // Clicking the minimap must not steal focus from the editor.
  setFocusPolicy(Qt::NoFocus);
  setAttribute(Qt::WA_NoSystemBackground);
  setAttribute(Qt::WA_TranslucentBackground);
  setMouseTracking(true);
  setFixedWidth(widthPx_);
  hide();
}

void MinimapView::setTheme(const Theme &theme) {
  bg_ = theme.minimapBg;
  sliderBg_ = theme.minimapSliderBg;
  sliderHoverBg_ = theme.minimapSliderHoverBg;
  sliderActiveBg_ = theme.minimapSliderActiveBg;
  styleFg_ = StyleForegroundTable(theme);
  invalidateAll();
  update();
}

void MinimapView::applySettings(bool enabled, int widthPx) {
  enabled_ = enabled;
  widthPx_ = qBound(40, widthPx, 200);
  setFixedWidth(widthPx_);
  setVisible(enabled);
  if (enabled) {
    invalidateAll();
    syncToEditorScroll();
  }
}

// --- Geometry ---

int MinimapView::displayLineCount() const {
  return static_cast<int>(sci_->visibleFromDocLine(sci_->lineCount()));
}

int MinimapView::docLineForY(int y) const {
  // y is a pixel offset from the top of the minimap widget. Convert to a
  // display line, then to a document line.
  const int widgetLines = height() / linePx_;
  const int totalDisplay = displayLineCount();
  if (totalDisplay <= widgetLines)
    return static_cast<int>(sci_->docLineFromVisible(y / linePx_));
  // Proportional slide: the minimap scrolls, so map y to a display line
  // offset by the current top.
  const int maxTop = totalDisplay - widgetLines;
  const int editorVis = static_cast<int>(sci_->firstVisibleLine());
  const int minimapTopVis =
      (maxTop > 0) ? static_cast<int>(static_cast<long long>(editorVis) *
                                      maxTop / totalDisplay)
                   : 0;
  const int displayLine = minimapTopVis + y / linePx_;
  return static_cast<int>(sci_->docLineFromVisible(displayLine));
}

int MinimapView::yForDocLine(int line) const {
  const int totalDisplay = displayLineCount();
  const int widgetLines = height() / linePx_;
  if (totalDisplay <= widgetLines) {
    return static_cast<int>(sci_->visibleFromDocLine(line)) * linePx_;
  }
  const int maxTop = totalDisplay - widgetLines;
  const int editorVis = static_cast<int>(sci_->firstVisibleLine());
  const int minimapTopVis =
      (maxTop > 0) ? static_cast<int>(static_cast<long long>(editorVis) *
                                      maxTop / totalDisplay)
                   : 0;
  const int displayLine = static_cast<int>(sci_->visibleFromDocLine(line));
  return (displayLine - minimapTopVis) * linePx_;
}

int MinimapView::topDocLine() const {
  const int totalDisplay = displayLineCount();
  const int widgetLines = height() / linePx_;
  if (totalDisplay <= widgetLines)
    return 0;
  const int maxTop = totalDisplay - widgetLines;
  const int editorVis = static_cast<int>(sci_->firstVisibleLine());
  const int minimapTopVis =
      (maxTop > 0) ? static_cast<int>(static_cast<long long>(editorVis) *
                                      maxTop / totalDisplay)
                   : 0;
  return static_cast<int>(sci_->docLineFromVisible(minimapTopVis));
}

QRect MinimapView::sliderRect() const {
  const int totalDisplay = displayLineCount();
  const int widgetLines = height() / linePx_;
  if (totalDisplay <= widgetLines) {
    // Document fits: slider = editor viewport, 1:1.
    const int editorVis = static_cast<int>(sci_->firstVisibleLine());
    const int editorLines = static_cast<int>(sci_->linesOnScreen());
    const int top = editorVis * linePx_;
    const int h = qMax(1, editorLines * linePx_);
    return QRect(0, top, width(), h);
  }
  // Proportional slide: slider represents the editor's viewport within the
  // whole document.
  const int maxTop = totalDisplay - widgetLines;
  const int editorVis = static_cast<int>(sci_->firstVisibleLine());
  const int editorLines = static_cast<int>(sci_->linesOnScreen());
  const int minimapTopVis =
      (maxTop > 0) ? static_cast<int>(static_cast<long long>(editorVis) *
                                      maxTop / totalDisplay)
                   : 0;
  const int sliderLines = qMax(1, editorLines * widgetLines / totalDisplay);
  const int top = minimapTopVis * linePx_;
  const int h = sliderLines * linePx_;
  return QRect(0, top, width(), h);
}

void MinimapView::scrollEditorToY(int y, bool center) {
  const int totalDisplay = displayLineCount();
  const int widgetLines = height() / linePx_;
  if (totalDisplay <= widgetLines) {
    // 1:1: y maps directly to a display line.
    int displayLine = y / linePx_;
    if (center) {
      const int half = static_cast<int>(sci_->linesOnScreen()) / 2;
      displayLine = qMax(0, displayLine - half);
    }
    sci_->setFirstVisibleLine(displayLine);
    return;
  }
  // Proportional slide: invert the minimap-top calculation.
  const int maxTop = totalDisplay - widgetLines;
  if (maxTop <= 0)
    return;
  const int minimapTopVis = qBound(0, y / linePx_, maxTop);
  int editorVis = static_cast<int>(static_cast<long long>(minimapTopVis) *
                                   totalDisplay / maxTop);
  if (center) {
    const int half = static_cast<int>(sci_->linesOnScreen()) / 2;
    editorVis = qMax(0, editorVis - half);
  }
  sci_->setFirstVisibleLine(editorVis);
}

// --- Strip cache ---

MinimapView::Strip &MinimapView::stripFor(int docLine) {
  const int stripIdx = docLine / kStripLines;
  while (strips_.size() <= stripIdx) {
    strips_.append(
        Strip{static_cast<int>(strips_.size()) * kStripLines, QImage(), true});
  }
  return strips_[stripIdx];
}

void MinimapView::renderStrip(Strip &s) {
  const int lineCount = static_cast<int>(sci_->lineCount());
  if (s.firstLine >= lineCount) {
    s.image = QImage();
    s.dirty = false;
    return;
  }

  const int lastLine = qMin(s.firstLine + kStripLines, lineCount) - 1;
  const int startPos = static_cast<int>(sci_->positionFromLine(s.firstLine));
  const int endPos = static_cast<int>(sci_->positionFromLine(lastLine + 1));
  const int len = endPos - startPos;
  if (len <= 0) {
    s.image = QImage();
    s.dirty = false;
    return;
  }

  // Phase 1: colourise the strip range on the GUI thread before fetching
  // styles. This is safe because strips are small (512 lines) and only the
  // visible ones are rendered. The cap (kPhase1LineCap) prevents pathological
  // files from reaching this path at all.
  if (static_cast<sptr_t>(startPos) > sci_->endStyled()) {
    sci_->colourise(startPos, endPos);
  }

  // Fetch styled text: SCI_GETSTYLEDTEXT returns a 2-byte cell array
  // (character + style byte) via Sci_TextRange.
  Sci_TextRange tr;
  tr.chrg.cpMin = startPos;
  tr.chrg.cpMax = endPos;
  QByteArray buf(len * 2, '\0');
  tr.lpstrText = buf.data();
  sci_->send(SCI_GETSTYLEDTEXT, 0, reinterpret_cast<sptr_t>(&tr));

  // Paint into a QImage at devicePixelRatio for crisp rendering.
  const qreal dpr = devicePixelRatioF();
  const int stripH = (lastLine - s.firstLine + 1) * linePx_;
  QImage img(QSize(widthPx_, stripH), QImage::Format_RGB32);
  img.setDevicePixelRatio(dpr);
  img.fill(bg_.rgba());

  QPainter p(&img);
  p.setRenderHint(QPainter::Antialiasing, false);

  int col = 0;
  int y = 0;
  for (int i = 0; i < len; ++i) {
    const char ch = buf[i * 2];
    const unsigned char style = static_cast<unsigned char>(buf[i * 2 + 1]);
    if (ch == '\n' || ch == '\r') {
      col = 0;
      y += linePx_;
      continue;
    }
    if (ch != ' ' && ch != '\t') {
      auto it = styleFg_.constFind(style);
      QColor c = (it != styleFg_.end()) ? it.value() : bg_;
      int x = col * colPx_;
      if (x < widthPx_) {
        p.fillRect(x, y, colPx_, linePx_, c);
      }
    }
    ++col;
  }

  s.image = img;
  s.dirty = false;
}

// --- Painting ---

void MinimapView::paintEvent(QPaintEvent *) {
  if (!enabled_ || !sci_)
    return;

  // Phase 1 cap: hide the widget for pathological files.
  if (static_cast<sptr_t>(sci_->lineCount()) > kPhase1LineCap) {
    QPainter p(this);
    p.fillRect(rect(), bg_);
    return;
  }

  QPainter p(this);
  p.fillRect(rect(), bg_);

  // Draw the visible strips.
  const int topLine = topDocLine();
  const int widgetLines = height() / linePx_;
  const int bottomLine = topLine + widgetLines + 1;

  const int firstStrip = topLine / kStripLines;
  const int lastStrip = bottomLine / kStripLines;
  for (int si = firstStrip; si <= lastStrip; ++si) {
    Strip &s = stripFor(si * kStripLines);
    if (s.dirty)
      renderStrip(s);
    if (!s.image.isNull()) {
      // The strip covers lines [s.firstLine, s.firstLine + kStripLines).
      // Draw it at the y offset relative to the minimap's top.
      const int yOff = yForDocLine(s.firstLine);
      p.drawImage(0, yOff, s.image);
    }
  }

  // Slider overlay.
  const QRect sr = sliderRect();
  QColor sliderColor =
      dragging_ ? sliderActiveBg_ : (hovered_ ? sliderHoverBg_ : sliderBg_);
  p.fillRect(sr, sliderColor);
}

// --- Interaction ---

void MinimapView::mousePressEvent(QMouseEvent *e) {
  if (!enabled_)
    return;
  const int y = static_cast<int>(e->position().y());
  const QRect sr = sliderRect();
  if (sr.contains(e->pos())) {
    // Press inside the slider: begin drag, remembering the grab offset.
    dragging_ = true;
    dragGrabDy_ = y - sr.top();
  } else {
    // Press outside: jump, then enter drag with the slider centered.
    scrollEditorToY(y - sr.height() / 2, false);
    dragging_ = true;
    dragGrabDy_ = sr.height() / 2;
  }
  update();
}

void MinimapView::mouseMoveEvent(QMouseEvent *e) {
  const int y = static_cast<int>(e->position().y());
  if (dragging_) {
    scrollEditorToY(y - dragGrabDy_, false);
    update();
  } else {
    const QRect sr = sliderRect();
    const bool wasHovered = hovered_;
    hovered_ = sr.contains(e->pos());
    if (hovered_ != wasHovered)
      update();
  }
}

void MinimapView::mouseReleaseEvent(QMouseEvent *) {
  dragging_ = false;
  update();
}

void MinimapView::wheelEvent(QWheelEvent *e) {
  // Forward to the editor: scroll by the same number of lines Scintilla
  // would, so trackpad momentum feels identical.
  const int delta = e->angleDelta().y();
  if (delta != 0) {
    const int lines = -delta / 120 * 3; // 3 lines per notch, matching Scintilla
    sci_->send(SCI_LINESCROLL, 0, lines);
  }
  syncToEditorScroll();
}

void MinimapView::resizeEvent(QResizeEvent *) {
  // Strips are line-addressed, so a resize only changes the visible window.
  update();
}

void MinimapView::leaveEvent(QEvent *) {
  hovered_ = false;
  update();
}

// --- Sync ---

void MinimapView::invalidateAll() {
  strips_.clear();
  update();
}

void MinimapView::invalidateLines(int firstLine, int lastLine) {
  const int stripIdx = firstLine / kStripLines;
  const int endLine =
      (lastLine < 0) ? strips_.size() : (lastLine / kStripLines + 1);
  for (int i = stripIdx; i < qMax(endLine, strips_.size()); ++i) {
    if (i < strips_.size())
      strips_[i].dirty = true;
  }
  update();
}

void MinimapView::syncToEditorScroll() {
  // Slider-only repaint; never re-renders strips.
  update();
}

MinimapView::State MinimapView::state() const {
  State st;
  st.enabled = enabled_;
  const QRect sr = sliderRect();
  st.sliderTop = sr.top();
  st.sliderHeight = sr.height();
  st.topLine = topDocLine();
  return st;
}

} // namespace trowel
