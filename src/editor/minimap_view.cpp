#include "editor/minimap_view.h"

#include "editor/scanner.h"
#include "plugin/scanner_plugin_syntax.h"
#include "plugin/syntax_descriptor.h"

#include <Scintilla.h>
#include <ScintillaEdit.h>

#include <QtConcurrent>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <limits>

namespace trowel {

namespace {

// All data the off-thread worker needs to render a strip. Captured on the
// GUI thread (where Scintilla is accessible) and moved to the worker; no
// Scintilla access happens off-thread.
struct RenderParams {
  QByteArray text;
  int packedState = 0;
  int lang = 0; // Language as int (see minimap_view.h)
  bool rainbow = true;
  QHash<int, QColor> styleFg;
  QColor bg;
  int widthPx = 90;
  int linePx = 2;
  int colPx = 1;
  int stripLineCount = 0;
  qreal dpr = 1.0;
  const SyntaxDescriptor *pluginDesc = nullptr;
};

// Render a strip off the GUI thread. Drives the scanners via a BufferSink
// (no Scintilla access) and paints the resulting style bytes into a QImage.
QImage renderStripWorker(RenderParams p) {
  const Language lang = static_cast<Language>(p.lang);
  LexState st = UnpackLexState(p.packedState);

  BufferSink sink(p.text.size());
  Emitter out(sink, 0);
  out.SetGapStyle(DefaultStyleFor(lang));

  // Scan each line, mirroring ScannerLexer::Lex's line loop.
  const char *const data = p.text.constData();
  const Sci_Position textLen = p.text.size();
  Sci_Position i = 0;
  Sci_Position line = 0;
  while (i < textLen) {
    Sci_Position contentEnd = i;
    while (contentEnd < textLen && data[contentEnd] != '\n' &&
           data[contentEnd] != '\r')
      ++contentEnd;
    Sci_Position lineEnd = contentEnd;
    if (lineEnd < textLen && data[lineEnd] == '\r') ++lineEnd;
    if (lineEnd < textLen && data[lineEnd] == '\n') ++lineEnd;

    ScanInput in{data, i, contentEnd, p.rainbow, line};
    if (lang == Language::PluginSyntax && p.pluginDesc) {
      ScanPluginSyntaxLine(*p.pluginDesc, in, st, out);
    } else {
      ScanLine(lang, in, st, out);
    }
    out.FillTo(lineEnd);

    ++line;
    i = lineEnd;
  }

  // Paint the style bytes into a QImage.
  const int stripH = p.stripLineCount * p.linePx;
  QImage img(QSize(p.widthPx, stripH), QImage::Format_RGB32);
  img.setDevicePixelRatio(p.dpr);
  img.fill(p.bg.rgba());

  QPainter painter(&img);
  painter.setRenderHint(QPainter::Antialiasing, false);

  int col = 0;
  int y = 0;
  for (int j = 0; j < p.text.size(); ++j) {
    const char ch = data[j];
    const unsigned char style = sink.styles()[static_cast<size_t>(j)];
    if (ch == '\n' || ch == '\r') {
      col = 0;
      y += p.linePx;
      continue;
    }
    if (ch != ' ' && ch != '\t') {
      auto it = p.styleFg.constFind(style);
      QColor c = (it != p.styleFg.end()) ? it.value() : p.bg;
      int x = col * p.colPx;
      if (x < p.widthPx)
        painter.fillRect(x, y, p.colPx, p.linePx, c);
    }
    ++col;
  }

  return img;
}

} // namespace

MinimapView::MinimapView(ScintillaEdit *sci, QWidget *parent)
    : QWidget(parent), sci_(sci) {
  // Clicking the minimap must not steal focus from the editor.
  setFocusPolicy(Qt::NoFocus);
  setAttribute(Qt::WA_NoSystemBackground);
  setAttribute(Qt::WA_TranslucentBackground);
  setMouseTracking(true);
  setFixedWidth(widthPx_);
  hide();

  // Coalesce bursts of invalidations (e.g. rapid keystrokes) into a single
  // repaint. The slider still follows scroll immediately via
  // syncToEditorScroll; only strip re-renders are deferred.
  renderDebounce_.setSingleShot(true);
  renderDebounce_.setInterval(60);
  connect(&renderDebounce_, &QTimer::timeout, this, [this] { update(); });
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

void MinimapView::setLanguage(Language lang, bool rainbow) {
  lang_ = static_cast<int>(lang);
  rainbow_ = rainbow;
  pluginDesc_ = (lang == Language::PluginSyntax)
                    ? CurrentPluginSyntaxDescriptor()
                    : nullptr;
}

void MinimapView::applySettings(bool enabled, int widthPx) {
  enabled_ = enabled;
  widthPx_ = qBound(40, widthPx, 200);
  setFixedWidth(widthPx_);
  setVisible(enabled);
  if (enabled) {
    lastStyledEnd_ = sci_ ? sci_->endStyled() : 0;
    invalidateAll();
    syncToEditorScroll();
  }
}

// --- Geometry ---

int MinimapView::displayLineCount() const {
  return static_cast<int>(sci_->visibleFromDocLine(sci_->lineCount()));
}

int MinimapView::docLineForY(int y) const {
  const int widgetLines = height() / linePx_;
  const int totalDisplay = displayLineCount();
  if (totalDisplay <= widgetLines)
    return static_cast<int>(sci_->docLineFromVisible(y / linePx_));
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
    const int editorVis = static_cast<int>(sci_->firstVisibleLine());
    const int editorLines = static_cast<int>(sci_->linesOnScreen());
    const int top = editorVis * linePx_;
    const int h = qMax(1, editorLines * linePx_);
    return QRect(0, top, width(), h);
  }
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
    int displayLine = y / linePx_;
    if (center) {
      const int half = static_cast<int>(sci_->linesOnScreen()) / 2;
      displayLine = qMax(0, displayLine - half);
    }
    sci_->setFirstVisibleLine(displayLine);
    return;
  }
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
  strips_[stripIdx].lastUsed = ++generation_;
  return strips_[stripIdx];
}

void MinimapView::launchRender(Strip &s) {
  const int lineCount = static_cast<int>(sci_->lineCount());
  if (s.firstLine >= lineCount) {
    s.image = QImage();
    s.dirty = false;
    s.rendering = false;
    return;
  }

  const int lastLine = qMin(s.firstLine + kStripLines, lineCount) - 1;
  const int startPos = static_cast<int>(sci_->positionFromLine(s.firstLine));
  const int endPos = static_cast<int>(sci_->positionFromLine(lastLine + 1));
  const int len = endPos - startPos;
  if (len <= 0) {
    s.image = QImage();
    s.dirty = false;
    s.rendering = false;
    return;
  }

  // Extract the strip text on the GUI thread — Scintilla is not thread-safe.
  QByteArray text(len, '\0');
  Sci_TextRange tr;
  tr.chrg.cpMin = startPos;
  tr.chrg.cpMax = endPos;
  tr.lpstrText = text.data();
  sci_->send(SCI_GETTEXTRANGE, 0, reinterpret_cast<sptr_t>(&tr));

  // Seed the LexState from the line state at the strip's first line. Only
  // meaningful where Scintilla has already styled that line; past
  // endStyled() the line state may be stale, so seed with default.
  int packedState = 0;
  if (s.firstLine > 0 &&
      static_cast<sptr_t>(startPos) <= sci_->endStyled()) {
    packedState = static_cast<int>(sci_->lineState(s.firstLine - 1));
  }

  // Mark as in-flight before launching the worker.
  s.dirty = false;
  s.rendering = true;

  RenderParams params;
  params.text = std::move(text);
  params.packedState = packedState;
  params.lang = lang_;
  params.rainbow = rainbow_;
  params.styleFg = styleFg_;
  params.bg = bg_;
  params.widthPx = widthPx_;
  params.linePx = linePx_;
  params.colPx = colPx_;
  params.stripLineCount = lastLine - s.firstLine + 1;
  params.dpr = devicePixelRatioF();
  params.pluginDesc = pluginDesc_;

  const int stripFirstLine = s.firstLine;
  auto *watcher = new QFutureWatcher<QImage>(this);
  connect(watcher, &QFutureWatcher<QImage>::finished, this,
          [this, watcher, stripFirstLine] {
            const QImage img = watcher->result();
            watcher->deleteLater();

            // Find the strip by firstLine — it may have been evicted or
            // cleared (invalidateAll) while the render was in flight.
            for (Strip &s : strips_) {
              if (s.firstLine == stripFirstLine) {
                s.rendering = false;
                if (!s.dirty) {
                  // Not re-invalidated during render: accept the result.
                  s.image = img;
                  update();
                }
                // If dirty, the render is stale; discard and let the next
                // paintEvent re-launch.
                return;
              }
            }
            // Strip no longer exists: result discarded.
          });

  watcher->setFuture(QtConcurrent::run(renderStripWorker, std::move(params)));
}

// --- Painting ---

void MinimapView::paintEvent(QPaintEvent *) {
  if (!enabled_ || !sci_)
    return;

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
    if (s.dirty && !s.rendering)
      launchRender(s);
    if (!s.image.isNull()) {
      const int yOff = yForDocLine(s.firstLine);
      p.drawImage(0, yOff, s.image);
    }
    // Strips that are still rendering or not yet started draw as flat bg,
    // already filled above.
  }

  // Slider overlay.
  const QRect sr = sliderRect();
  QColor sliderColor =
      dragging_ ? sliderActiveBg_ : (hovered_ ? sliderHoverBg_ : sliderBg_);
  p.fillRect(sr, sliderColor);

  // Bound memory: evict the oldest rendered strips beyond the working set.
  evictStrips();
}

void MinimapView::evictStrips() {
  int rendered = 0;
  for (const Strip &s : strips_)
    if (!s.image.isNull())
      ++rendered;
  while (rendered > kMaxStrips) {
    int oldestIdx = -1;
    quint64 oldestGen = std::numeric_limits<quint64>::max();
    for (int i = 0; i < strips_.size(); ++i) {
      if (!strips_[i].image.isNull() && strips_[i].lastUsed < oldestGen) {
        oldestGen = strips_[i].lastUsed;
        oldestIdx = i;
      }
    }
    if (oldestIdx < 0)
      break;
    strips_[oldestIdx].image = QImage();
    strips_[oldestIdx].dirty = true;
    --rendered;
  }
}

// --- Interaction ---

void MinimapView::mousePressEvent(QMouseEvent *e) {
  if (!enabled_)
    return;
  const int y = static_cast<int>(e->position().y());
  const QRect sr = sliderRect();
  if (sr.contains(e->pos())) {
    dragging_ = true;
    dragGrabDy_ = y - sr.top();
  } else {
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
  const int delta = e->angleDelta().y();
  if (delta != 0) {
    const int lines = -delta / 120 * 3;
    sci_->send(SCI_LINESCROLL, 0, lines);
  }
  syncToEditorScroll();
}

void MinimapView::resizeEvent(QResizeEvent *) {
  update();
}

void MinimapView::leaveEvent(QEvent *) {
  hovered_ = false;
  update();
}

// --- Sync ---

void MinimapView::invalidateAll() {
  strips_.clear();
  renderDebounce_.start();
}

void MinimapView::invalidateLines(int firstLine, int lastLine) {
  const int stripIdx = firstLine / kStripLines;
  const int endLine =
      (lastLine < 0) ? strips_.size() : (lastLine / kStripLines + 1);
  for (int i = stripIdx; i < qMax(endLine, strips_.size()); ++i) {
    if (i < strips_.size())
      strips_[i].dirty = true;
  }
  renderDebounce_.start();
}

void MinimapView::syncToEditorScroll() {
  // When Scintilla styles a new region (e.g. the user scrolls into an
  // unvisited area), endStyled() advances. Invalidate the corresponding
  // strips so they re-render with correct styles instead of the
  // default-state fallback used for unvisited regions.
  if (sci_) {
    const sptr_t styled = sci_->endStyled();
    if (styled > lastStyledEnd_) {
      const int firstLine =
          static_cast<int>(sci_->lineFromPosition(lastStyledEnd_));
      const int lastLine = static_cast<int>(sci_->lineFromPosition(styled));
      invalidateLines(firstLine, lastLine);
      lastStyledEnd_ = styled;
    }
  }
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
