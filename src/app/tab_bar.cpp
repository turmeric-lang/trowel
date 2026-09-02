#include "app/tab_bar.h"

#include <QEvent>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <QWheelEvent>

#include <algorithm>

namespace trowel {

namespace {
constexpr int kVPad = 5;
constexpr int kHPad = 12;
constexpr int kCloseSlot = 24;
constexpr int kMaxTabWidth = 240;
constexpr int kCloseGlyphPadRight = 8;
constexpr int kDividerMarginY = 4;
}

TabBar::TabBar(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setAttribute(Qt::WA_Hover, true);
    bg_ = QColor("#1e1e1e");
    fg_ = QColor("#d0d0d0");
    activeFg_ = QColor("#EFA030");
    divider_ = QColor("#3a3a3a");

    QFont f = font();
    const int base = f.pointSize();
    if (base > 0) f.setPointSize(std::max(1, base - 1));
    else if (f.pixelSize() > 0) f.setPixelSize(std::max(1, f.pixelSize() - 1));
    setFont(f);

    updateFixedHeight();
}

void TabBar::setColors(const QColor& bg, const QColor& fg, const QColor& divider) {
    bg_ = bg;
    fg_ = fg;
    divider_ = divider;
    update();
}

void TabBar::setActiveFg(const QColor& fg) {
    activeFg_ = fg;
    update();
}

void TabBar::setTabs(const QStringList& displayNames, int activeIndex) {
    names_ = displayNames;
    tooltips_.clear();
    for (int i = 0; i < names_.size(); ++i) tooltips_.append(QString());
    modified_.assign(names_.size(), false);
    readOnly_.assign(names_.size(), false);
    closable_.assign(names_.size(), true);
    active_ = activeIndex;
    if (hovered_ >= names_.size()) hovered_ = -1;
    relayout();
    ensureActiveVisible();
    update();
}

void TabBar::setActive(int index) {
    if (active_ == index) return;
    active_ = index;
    relayout();
    ensureActiveVisible();
    update();
}

void TabBar::setModified(int index, bool modified) {
    if (index < 0 || index >= static_cast<int>(modified_.size())) return;
    if (modified_[index] == modified) return;
    modified_[index] = modified;
    relayout();
    update();
}

void TabBar::setReadOnly(int index, bool readOnly) {
    if (index < 0 || index >= static_cast<int>(readOnly_.size())) return;
    if (readOnly_[index] == readOnly) return;
    readOnly_[index] = readOnly;
    relayout();
    update();
}

void TabBar::setClosable(int index, bool closable) {
    if (index < 0 || index >= static_cast<int>(closable_.size())) return;
    if (closable_[index] == closable) return;
    closable_[index] = closable;
    relayout();
    update();
}

void TabBar::setDividerEdge(DividerEdge edge) {
    if (dividerEdge_ == edge) return;
    dividerEdge_ = edge;
    update();
}

void TabBar::setTooltip(int index, const QString& tip) {
    if (index < 0 || index >= tooltips_.size()) return;
    tooltips_[index] = tip;
    if (index < static_cast<int>(geoms_.size())) geoms_[index].tooltip = tip;
}

// The font labels are actually drawn in. Tab labels are bold, and bold is
// wider — so measuring in the regular font under-allocated every tab by the
// difference and the label was clipped by drawText. A closable tab hid it: the
// 24px close slot is slack the text can spill into. "Debugger" is not closable,
// so its text area is exactly the measured width and the final `r` was cut off.
//
// One function for the whole class rather than a bold QFont built at each of
// the three sites, because "measure it the way you draw it" is the invariant
// that broke.
QFont TabBar::labelFont() const {
    QFont f = font();
    f.setBold(true);
    return f;
}

void TabBar::updateFixedHeight() {
    const int h = QFontMetrics(labelFont()).height() + 2 * kVPad;
    setFixedHeight(h);
}

void TabBar::relayout() {
    geoms_.clear();
    geoms_.reserve(names_.size());
    const QFontMetrics fm(labelFont());
    int x = 0;
    const int h = height();
    for (int i = 0; i < names_.size(); ++i) {
        QString label = names_[i];
        // Spelled out rather than drawn as a padlock glyph: the tab bar renders
        // in the UI font, which on Linux routinely has no lock codepoint, and a
        // tofu box next to a filename reads as corruption rather than as a
        // lock. The bullet above is safe because every UI font has U+2022.
        if (i < static_cast<int>(readOnly_.size()) && readOnly_[i]) {
            label += QStringLiteral(" (ro)");
        }
        if (i < static_cast<int>(modified_.size()) && modified_[i]) {
            label += QStringLiteral(" •");
        }
        const bool closable = (i < static_cast<int>(closable_.size()))
            ? closable_[i] : true;
        const int closeSlot = closable ? kCloseSlot : 0;
        int textW = fm.horizontalAdvance(label);
        int w = textW + kHPad * 2 + closeSlot;
        bool elided = false;
        if (w > kMaxTabWidth) {
            w = kMaxTabWidth;
            elided = true;
        }
        TabGeom g;
        g.rect = QRect(x, 0, w, h);
        g.closeRect = QRect(x + w - closeSlot, 0, closeSlot, h);
        if (elided) {
            const int textArea = w - kHPad * 2 - closeSlot;
            g.label = fm.elidedText(label, Qt::ElideMiddle, textArea);
        } else {
            g.label = label;
        }
        g.tooltip = (i < tooltips_.size()) ? tooltips_[i] : QString();
        g.modified = (i < static_cast<int>(modified_.size())) ? modified_[i] : false;
        g.readOnly = (i < static_cast<int>(readOnly_.size())) ? readOnly_[i] : false;
        geoms_.push_back(std::move(g));
        x += w;
    }
}

int TabBar::tabAt(const QPoint& p) const {
    const QPoint q(p.x() + scrollOffset_, p.y());
    for (int i = 0; i < static_cast<int>(geoms_.size()); ++i) {
        if (geoms_[i].rect.contains(q)) return i;
    }
    return -1;
}

bool TabBar::closeHit(int index, const QPoint& p) const {
    if (index < 0 || index >= static_cast<int>(geoms_.size())) return false;
    const bool closable = (index < static_cast<int>(closable_.size()))
        ? closable_[index] : true;
    if (!closable) return false;
    const QPoint q(p.x() + scrollOffset_, p.y());
    const QRect& cr = geoms_[index].closeRect;
    // Approximate the glyph hit box: an 16x16 square centered vertically,
    // right-aligned within the close slot with kCloseGlyphPadRight padding.
    const int size = 16;
    const int cx = cr.right() - (kCloseGlyphPadRight - 2) - size / 2;
    const int cy = cr.center().y() - 1;
    const QRect glyphRect(cx - size / 2, cy - size / 2, size, size);
    return glyphRect.contains(q);
}

int TabBar::contentWidth() const {
    if (geoms_.empty()) return 0;
    return geoms_.back().rect.right() + 1;
}

int TabBar::maxScrollOffset() const {
    return std::max(0, contentWidth() - width());
}

void TabBar::clampScrollOffset() {
    scrollOffset_ = std::clamp(scrollOffset_, 0, maxScrollOffset());
}

void TabBar::ensureActiveVisible() {
    if (active_ < 0 || active_ >= static_cast<int>(geoms_.size())) return;
    const QRect& r = geoms_[active_].rect;
    const int viewLeft = scrollOffset_;
    const int viewRight = scrollOffset_ + width();
    if (r.left() < viewLeft) {
        scrollOffset_ = r.left();
    } else if (r.right() >= viewRight) {
        scrollOffset_ = r.right() - width() + 1;
    }
    clampScrollOffset();
}

void TabBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), bg_);

    // 1px border on the edge away from the content. Document tabs sit above
    // their content, so the rule is on the bottom; a bottom-mounted pane bar
    // puts the rule on top so it reads as the pane's top edge.
    //
    // These two were swapped: `Top` drew at height()-1 and `Bottom` at 0. The
    // document bar looked right only because its default was `Top` and it
    // wanted the line at the bottom, so the two errors cancelled. The pane bar
    // asked for `Top` and got a rule along the very bottom of the window,
    // where the macOS window bevel sits right under it — and no rule above it
    // separating it from the pane, which is the one it actually needed.
    p.setPen(divider_);
    if (dividerEdge_ == DividerEdge::Top) {
        p.drawLine(0, 0, width(), 0);
    } else {
        p.drawLine(0, height() - 1, width(), height() - 1);
    }

    p.translate(-scrollOffset_, 0);

    for (int i = 0; i < static_cast<int>(geoms_.size()); ++i) {
        const TabGeom& g = geoms_[i];

        // Divider on the right edge of every tab. Skip drawing the last
        // one when the bar is scrollable so it doesn't butt up against
        // the clipped right edge.
        const bool isLast = (i + 1 == static_cast<int>(geoms_.size()));
        const bool scrollable = maxScrollOffset() > 0;
        if (!(isLast && scrollable)) {
            p.setPen(divider_);
            p.drawLine(g.rect.right(), kDividerMarginY,
                       g.rect.right(), height() - kDividerMarginY - 1);
        }

        const bool closable = (i < static_cast<int>(closable_.size()))
            ? closable_[i] : true;
        const int closeSlot = closable ? kCloseSlot : 0;

        // Label: centered in the text area. A closable tab's right inset is the
        // close slot; a non-closable one insets by kHPad instead, so its label
        // keeps the same padding on both sides rather than hugging the divider.
        QRect textRect = g.rect.adjusted(kHPad, 0, -(closable ? closeSlot : kHPad), -2);
        p.setFont(labelFont());
        p.setPen(i == active_ ? activeFg_ : fg_);
        p.drawText(textRect, Qt::AlignCenter, g.label);

        // Close glyph, only on closable tabs; brightens when hovered directly.
        if (closable) {
            const int size = 18;
            const int cx = g.closeRect.right() - (kCloseGlyphPadRight - 2) - size / 2;
            const int cy = g.closeRect.center().y();
            const QRect glyphRect(cx - size / 2, cy - size / 2, size, size);
            QFont cf = font();
            cf.setBold(false);
            p.setFont(cf);
            const bool hot = (i == hovered_) && hoverClose_;
            p.setPen(hot ? fg_ : divider_);
            p.drawText(glyphRect, Qt::AlignCenter, QString(QChar(0x00D7)));
        }
    }

    // Restore font.
    p.setFont(font());
}

void TabBar::mouseMoveEvent(QMouseEvent* e) {
    const int idx = tabAt(e->pos());
    const bool onClose = (idx >= 0) && closeHit(idx, e->pos());
    if (idx != hovered_ || onClose != hoverClose_) {
        hovered_ = idx;
        hoverClose_ = onClose;
        update();
    }
    if (idx >= 0) {
        const QString& tip = geoms_[idx].tooltip;
        if (!tip.isEmpty()) {
            QToolTip::showText(e->globalPosition().toPoint(), tip, this);
        } else {
            QToolTip::hideText();
        }
    }
}

void TabBar::mousePressEvent(QMouseEvent* e) {
    const int idx = tabAt(e->pos());
    if (idx < 0) return;
    if (e->button() == Qt::MiddleButton) {
        emit closeRequested(idx);
        return;
    }
    if (e->button() == Qt::LeftButton) {
        if (closeHit(idx, e->pos())) {
            emit closeRequested(idx);
        } else {
            emit activateRequested(idx);
        }
    }
}

void TabBar::leaveEvent(QEvent*) {
    if (hovered_ != -1 || hoverClose_) {
        hovered_ = -1;
        hoverClose_ = false;
        update();
    }
}

void TabBar::resizeEvent(QResizeEvent*) {
    relayout();
    clampScrollOffset();
}

void TabBar::wheelEvent(QWheelEvent* e) {
    // Prefer pixel-precise deltas from trackpads; fall back to angle deltas.
    const QPoint pd = e->pixelDelta();
    const QPoint ad = e->angleDelta();
    int dx = 0;
    if (!pd.isNull()) {
        // Horizontal scroll or, when swiping vertically, use y as x.
        dx = pd.x() != 0 ? pd.x() : pd.y();
    } else if (!ad.isNull()) {
        const int a = ad.x() != 0 ? ad.x() : ad.y();
        // Angle delta is in 1/8 degrees; typical notch = 120 units.
        // Scroll roughly one tab (~120 px) per notch.
        dx = a * 120 / 120;
    }
    if (dx == 0) { e->ignore(); return; }
    scrollOffset_ -= dx;
    clampScrollOffset();
    update();
    e->accept();
}

bool TabBar::event(QEvent* e) {
    if (e->type() == QEvent::FontChange) {
        updateFixedHeight();
        relayout();
        update();
    }
    return QWidget::event(e);
}

}
