#pragma once

#include <QColor>
#include <QFont>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <vector>

namespace trowel {

class TabBar : public QWidget {
    Q_OBJECT
public:
    explicit TabBar(QWidget* parent = nullptr);

    void setTabs(const QStringList& displayNames, int activeIndex);
    void setActive(int index);
    void setModified(int index, bool modified);
    // Mark a tab whose buffer cannot be edited (a bundled stdlib file opened by
    // a definition jump). Drawn as a label suffix, exactly like the
    // modified-dot, so the two compose without fighting over the same space.
    void setReadOnly(int index, bool readOnly);
    // Whether a tab shows a close glyph and emits closeRequested. Default true
    // (document tabs); the REPL-pane bar sets it false for its fixed tabs.
    void setClosable(int index, bool closable);
    void setTooltip(int index, const QString& tip);

    void setColors(const QColor& bg, const QColor& fg, const QColor& divider);
    void setActiveFg(const QColor& fg);
    // Which edge of this widget the 1px divider rule is drawn on — literally,
    // `Top` is y=0. Default Bottom: document tabs sit above their content and
    // want the rule under them. A bottom-mounted pane bar sets Top so the rule
    // separates it from the pane above rather than tracing the window's edge.
    enum class DividerEdge { Top, Bottom };
    void setDividerEdge(DividerEdge edge);

signals:
    void activateRequested(int index);
    void closeRequested(int index);

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    bool event(QEvent* e) override;

private:
    struct TabGeom {
        QRect rect;
        QRect closeRect;
        QString label;      // possibly elided, with modified marker appended
        QString tooltip;
        bool modified = false;
        bool readOnly = false;
    };

    void relayout();
    void updateFixedHeight();
    // The font labels are drawn in (bold). Measure with this, never with
    // font() — see the definition.
    QFont labelFont() const;
    int tabAt(const QPoint& p) const;
    bool closeHit(int index, const QPoint& p) const;
    int contentWidth() const;
    int maxScrollOffset() const;
    void clampScrollOffset();
    void ensureActiveVisible();

    std::vector<TabGeom> geoms_;
    std::vector<bool> modified_;
    std::vector<bool> readOnly_;
    std::vector<bool> closable_;
    QStringList names_;
    QStringList tooltips_;
    int active_ = -1;
    int hovered_ = -1;
    bool hoverClose_ = false;

    QColor bg_;
    QColor fg_;
    QColor activeFg_;
    QColor divider_;
    DividerEdge dividerEdge_ = DividerEdge::Bottom;
    int scrollOffset_ = 0;
};

}
