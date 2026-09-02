#include "debug/debugger_view.h"

#include "app/icon_font.h"
#include "debug/timeline_strip.h"
#include "editor/theme_loader.h"

#include <QAbstractItemView>

#include <algorithm>
#include <QAction>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace trowel {

namespace {
// Frame id carried on the stack list's rows. Read back on selection rather
// than inferred from the row index: the two are the same today (constraint 11)
// and this does not break the day they stop being.
constexpr int kFrameIdRole = Qt::UserRole + 1;

// Breakpoint identity on a panel row. A row is keyed by path + line, not by
// its index — the panel is rebuilt from the model on every change, and an
// index would be stale the moment a breakpoint above it is removed.
constexpr int kBpPathRole = Qt::UserRole + 2;
constexpr int kBpLineRole = Qt::UserRole + 3;
constexpr int kBpConditionColumn = 1;

// Toolbar glyph height. Matches the sidebar's, so the two icon sets read as
// one vocabulary rather than two.
constexpr int kIconSize = 15;

// Mix `amount` of `over` into `base`. `QColor::lighter` scales HSV value, so on
// a near-black editor background (#0C0A08) it produces another near-black —
// the row lifts this theme needs have to be blended toward the foreground
// instead.
QColor Mix(const QColor& base, const QColor& over, double amount) {
    return QColor(
        int(base.red()   + (over.red()   - base.red())   * amount),
        int(base.green() + (over.green() - base.green()) * amount),
        int(base.blue()  + (over.blue()  - base.blue())  * amount));
}

// `Qt::ItemIsEditable` is a property of the whole item, not of one column, so
// making the condition editable would make the location editable too — and a
// location is a fact about the model, not a text field. Refusing to build an
// editor for column 0 is the narrowest way to say that.
class ConditionOnlyDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option,
                          const QModelIndex& index) const override {
        if (index.column() != kBpConditionColumn) return nullptr;
        return QStyledItemDelegate::createEditor(parent, option, index);
    }
};

// Two columns, sized so neither the header nor a horizontal scrollbar has to
// appear. `Interactive` with an explicit initial width rather than
// `ResizeToContents`: these panes are ~160px wide when the REPL pane is at its
// default size, and a content-sized first column pushed "Location" off the
// edge -- the header read "Locati" and every pane grew a bright scrollbar
// underneath. The user can still drag the divider.
void configureColumns(QTreeWidget* tree, int stretchColumn, int fixedWidth,
                      Qt::Alignment lastColumnAlign = Qt::AlignLeft) {
    QHeaderView* h = tree->header();
    const int fixed = stretchColumn == 0 ? 1 : 0;
    h->setSectionResizeMode(stretchColumn, QHeaderView::Stretch);
    h->setSectionResizeMode(fixed, QHeaderView::Interactive);
    h->setStretchLastSection(stretchColumn == 1);
    h->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    // A header aligned against its own column's contents is worse than no
    // header: "Line" sat hard left over right-aligned numbers, so the label
    // and the values it named pointed at opposite edges.
    tree->headerItem()->setTextAlignment(1, lastColumnAlign | Qt::AlignVCenter);
    tree->setColumnWidth(fixed, fixedWidth);
    // Elide rather than scroll. A path or a rendered value is routinely wider
    // than the pane, and the full text is already in the tooltip.
    tree->setTextElideMode(Qt::ElideMiddle);
    tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
}
}  // namespace

// Declared in the header; see there for why it is held by concrete type.
class PlaceholderTree : public QTreeWidget {
public:
    PlaceholderTree(const QString& placeholder, QWidget* parent)
        : QTreeWidget(parent), placeholder_(placeholder) {}

    void setPlaceholder(const QString& text) { placeholder_ = text; viewport()->update(); }
    void setPlaceholderColor(const QColor& c) { color_ = c; viewport()->update(); }

protected:
    void paintEvent(QPaintEvent* e) override {
        QTreeWidget::paintEvent(e);
        if (topLevelItemCount() > 0 || placeholder_.isEmpty()) return;
        QPainter p(viewport());
        p.setPen(color_);
        QFont f = font();
        f.setItalic(true);
        p.setFont(f);
        p.drawText(viewport()->rect().adjusted(12, 0, -12, 0),
                   Qt::AlignCenter | Qt::TextWordWrap, placeholder_);
    }

private:
    QString placeholder_;
    QColor color_ = QColor("#6b6b6b");
};

DebuggerView::DebuggerView(QWidget* parent)
    : QWidget(parent)
    , toolbar_(new QToolBar(this))
    , timeline_(new TimelineStrip(this))
    , warning_(new QLabel(this))
    , panes_(new QSplitter(Qt::Horizontal, this))
    , vsplit_(new QSplitter(Qt::Vertical, this))
    , stack_(new PlaceholderTree(QStringLiteral("No call stack.\nRun Debug Buffer to start a session."), this))
    , variables_(new PlaceholderTree(QStringLiteral("No locals.\nThey appear when the program stops."), this))
    , breakpoints_(new PlaceholderTree(QStringLiteral("No breakpoints.\nClick the gutter, or press F9."), this))
    , console_(new QPlainTextEdit(this))
    , evalRow_(new QWidget(this))
    , evalPrompt_(new QLabel(this))
    , evalInput_(new QLineEdit(this))
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Toolbar: Continue (F5), Step Over (F10), Step In (F11), Step Out
    // (Shift+F11), Stop (Shift+F5). No Pause button — the adapter cannot
    // honour it (constraint 6), so it would be a button that does nothing
    // while running and is redundant while paused.
    //
    // Icons rather than words, because the rest of the chrome is icons and a
    // row of five text buttons in a pane this size is most of its width.
    // Every one keeps its name as a tooltip.
    toolbar_->setIconSize(QSize(kIconSize, kIconSize));
    toolbar_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    toolbar_->setMovable(false);
    toolbar_->setFloatable(false);

    auto* cont = toolbar_->addAction("Continue");
    // No shortcut of its own: F5 lives on MainWindow's Debug Buffer action,
    // which continues when a session is paused and launches otherwise. Two
    // window-scoped actions claiming F5 is an ambiguous shortcut, and Qt
    // resolves that by firing neither.
    cont->setToolTip("Continue (F5)");
    connect(cont, &QAction::triggered, this, &DebuggerView::continueRequested);
    stepActions_.append(cont);

    auto* over = toolbar_->addAction("Step Over");
    over->setShortcut(QKeySequence("F10"));
    over->setToolTip("Step Over (F10)");
    connect(over, &QAction::triggered, this, &DebuggerView::stepOverRequested);
    stepActions_.append(over);

    auto* into = toolbar_->addAction("Step In");
    into->setShortcut(QKeySequence("F11"));
    into->setToolTip("Step In (F11)");
    connect(into, &QAction::triggered, this, &DebuggerView::stepInRequested);
    stepActions_.append(into);

    auto* out = toolbar_->addAction("Step Out");
    out->setShortcut(QKeySequence("Shift+F11"));
    out->setToolTip("Step Out (Shift+F11)");
    connect(out, &QAction::triggered, this, &DebuggerView::stepOutRequested);
    stepActions_.append(out);

    // Reverse execution. Only meaningful over a recording, so these are hidden
    // until a replay session says otherwise. Shortcuts mirror the forward ones
    // with Shift, which is what VS Code and nvim-dap both use.
    auto* backSep = toolbar_->addSeparator();
    auto* revCont = toolbar_->addAction("Reverse Continue");
    revCont->setToolTip("Reverse Continue — run backwards to the previous breakpoint");
    connect(revCont, &QAction::triggered, this, &DebuggerView::reverseContinueRequested);

    auto* revOver = toolbar_->addAction("Reverse Step Over");
    revOver->setShortcut(QKeySequence("Shift+F10"));
    revOver->setToolTip("Reverse Step Over (Shift+F10)");
    connect(revOver, &QAction::triggered, this, &DebuggerView::reverseStepOverRequested);

    auto* back = toolbar_->addAction("Step Back");
    back->setShortcut(QKeySequence("Shift+F12"));
    back->setToolTip("Step Back (Shift+F12)");
    connect(back, &QAction::triggered, this, &DebuggerView::stepBackRequested);

    reverseActions_ = {backSep, revCont, revOver, back};
    stepActions_.append({revCont, revOver, back});

    toolbar_->addSeparator();

    stopAction_ = toolbar_->addAction("Stop");
    stopAction_->setShortcut(QKeySequence("Shift+F5"));
    stopAction_->setToolTip("Stop (Shift+F5)");
    connect(stopAction_, &QAction::triggered, this, &DebuggerView::stopRequested);

    layout->addWidget(toolbar_);
    layout->addWidget(timeline_);
    timeline_->setVisible(false);

    // Caveats go in a banner, not in the breakpoints list.
    //
    // The basename-collision warning used to be a row in that list — same
    // column, same indentation, distinguished only by a small glyph and a
    // colour, and elided in the middle to "two open…d util.tur", which reads
    // like a filename. The first question anyone asked was why some
    // breakpoints had checkboxes and some did not. They did not: one of them
    // was not a breakpoint. Now every row in that list is one.
    warning_->setWordWrap(true);
    warning_->setContentsMargins(8, 4, 8, 4);
    warning_->setVisible(false);
    layout->addWidget(warning_);

    // Call stack: two columns rather than one padded string. `fn` and
    // `file:line` are different kinds of fact and want to align down their own
    // edges — a single column made every row a ragged line of its own.
    stack_->setColumnCount(2);
    stack_->setHeaderLabels({QStringLiteral("Frame"), QStringLiteral("Line")});
    stack_->setRootIsDecorated(false);
    stack_->setFrameShape(QFrame::NoFrame);
    stack_->setUniformRowHeights(true);
    configureColumns(stack_, /*stretch=*/0, /*fixedWidth=*/52,
                     /*lastColumnAlign=*/Qt::AlignRight);
    connect(stack_, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* item, QTreeWidgetItem*) {
                if (!item) return;
                emit frameSelected(item->data(0, kFrameIdRole).toInt());
            });

    // Variables: a two-column tree, flat for now. `variablesReference` is
    // always 0 upstream (constraint 7), so there is no expansion machinery —
    // a tree only so the widget is ready if Turmeric grows structured values.
    // Nothing here ever requests children.
    variables_->setColumnCount(2);
    variables_->setHeaderLabels({QStringLiteral("Name"), QStringLiteral("Value")});
    variables_->setRootIsDecorated(false);
    variables_->setFrameShape(QFrame::NoFrame);
    variables_->setUniformRowHeights(true);
    configureColumns(variables_, /*stretch=*/1, /*fixedWidth=*/76);

    // Breakpoints: checkable rows over the model. The condition column is
    // editable in place; both syntaxes the adapter accepts are documented in
    // the tooltip, because `i > 3` and `(> i 3)` both work and neither is
    // guessable from the other.
    breakpoints_->setColumnCount(2);
    breakpoints_->setHeaderLabels({QStringLiteral("Breakpoint"),
                                   QStringLiteral("Condition")});
    breakpoints_->setRootIsDecorated(false);
    breakpoints_->setFrameShape(QFrame::NoFrame);
    breakpoints_->setUniformRowHeights(true);
    configureColumns(breakpoints_, /*stretch=*/0, /*fixedWidth=*/78);
    breakpoints_->setEditTriggers(QAbstractItemView::DoubleClicked |
                                  QAbstractItemView::SelectedClicked);
    breakpoints_->setItemDelegate(new ConditionOnlyDelegate(breakpoints_));
    connect(breakpoints_, &QTreeWidget::itemChanged, this,
            [this](QTreeWidgetItem* item, int column) {
                if (!item) return;
                const QString path = item->data(0, kBpPathRole).toString();
                const int line = item->data(0, kBpLineRole).toInt();
                if (path.isEmpty() || line < 1) return;
                if (column == kBpConditionColumn) {
                    emit breakpointConditionEdited(path, line,
                                                   item->text(kBpConditionColumn).trimmed());
                } else {
                    emit breakpointEnableToggled(path, line,
                                                 item->checkState(0) == Qt::Checked);
                }
            });
    connect(breakpoints_, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item, int column) {
                // Double-clicking the condition column starts an edit; only
                // the location column means "take me there".
                if (!item || column == kBpConditionColumn) return;
                const QString path = item->data(0, kBpPathRole).toString();
                const int line = item->data(0, kBpLineRole).toInt();
                if (!path.isEmpty() && line >= 1) emit breakpointActivated(path, line);
            });
    breakpoints_->setContextMenuPolicy(Qt::ActionsContextMenu);
    auto* removeBp = new QAction(QStringLiteral("Remove Breakpoint"), breakpoints_);
    connect(removeBp, &QAction::triggered, this, [this] {
        QTreeWidgetItem* item = breakpoints_->currentItem();
        if (!item) return;
        const QString path = item->data(0, kBpPathRole).toString();
        const int line = item->data(0, kBpLineRole).toInt();
        if (!path.isEmpty() && line >= 1) emit breakpointRemoved(path, line);
    });
    breakpoints_->addAction(removeBp);

    panes_->addWidget(stack_);
    panes_->addWidget(variables_);
    panes_->addWidget(breakpoints_);
    panes_->setStretchFactor(0, 3);
    panes_->setStretchFactor(1, 4);
    panes_->setStretchFactor(2, 3);
    panes_->setChildrenCollapsible(false);

    // Read-only: there is no debuggee stdin to write back to (constraint 9),
    // and DAP `output` events are plain text, not an ANSI stream — so this is
    // a log, not a terminal.
    console_->setReadOnly(true);
    console_->setFrameShape(QFrame::NoFrame);
    console_->setPlaceholderText(
        QStringLiteral("Program output appears here while a session runs."));
    const QFont mono = MonospaceUiFont();
    console_->setFont(mono);
    evalInput_->setFont(mono);
    evalInput_->setFrame(false);
    // No placeholder while it is usable. The prompt beside it already says
    // what the line is, the same way a REPL's does — and a REPL waiting for
    // input shows a prompt and a caret, not a sentence about itself. The
    // placeholder is reclaimed for the disabled case, where there is something
    // worth saying: why you cannot type here. See setEvaluateEnabled.
    evalInput_->setPlaceholderText(QString());
    // Left margin 0: the prompt label supplies the left inset, so the caret
    // sits one space after `turi>` rather than a padding's width further on.
    evalInput_->setTextMargins(0, 3, 6, 3);
    connect(evalInput_, &QLineEdit::returnPressed, this, [this] {
        const QString expr = evalInput_->text().trimmed();
        if (expr.isEmpty()) return;
        evalInput_->clear();
        emit evaluateRequested(expr);
    });

    // `turi>`, not `turmeric>`: the REPL pane runs `tur repl` and prompts
    // `turmeric>`, but this line is evaluated by the interpreter through
    // `tur dap`, and `turi>` is what the interpreter is called throughout the
    // toolchain. Two different evaluators, two different prompts, on purpose.
    evalPrompt_->setText(QStringLiteral("turi>"));
    evalPrompt_->setFont(mono);
    // The prompt belongs to the input: clicking it focuses the line, which is
    // what a click on a prompt should do.
    evalPrompt_->setBuddy(evalInput_);
    evalPrompt_->setObjectName(QStringLiteral("evalPrompt"));

    // The prompt and the field are one control, so the rule that separates
    // them from the console above spans both — it lives on the row, not on the
    // QLineEdit, which would have left the prompt sitting above the line.
    evalRow_->setObjectName(QStringLiteral("evalRow"));
    auto* evalLayout = new QHBoxLayout(evalRow_);
    evalLayout->setContentsMargins(6, 0, 0, 0);
    evalLayout->setSpacing(6);
    evalLayout->addWidget(evalPrompt_);
    evalLayout->addWidget(evalInput_, 1);

    auto* consoleBox = new QWidget(this);
    auto* consoleLayout = new QVBoxLayout(consoleBox);
    consoleLayout->setContentsMargins(0, 0, 0, 0);
    consoleLayout->setSpacing(0);
    consoleLayout->addWidget(console_, 1);
    consoleLayout->addWidget(evalRow_);

    // Panes above, console below, both resizable: how much of the pane the
    // stack deserves depends entirely on how deep the recursion is, and only
    // the person looking at it knows. The console starts larger because it is
    // the one that has something in it before a session ever pauses.
    vsplit_->addWidget(panes_);
    vsplit_->addWidget(consoleBox);
    vsplit_->setStretchFactor(0, 3);
    vsplit_->setStretchFactor(1, 4);
    vsplit_->setChildrenCollapsible(false);
    layout->addWidget(vsplit_, 1);

    applyTheme(LoadBuiltinDarkTheme());

    // Start disabled: nothing to step until a session is paused.
    setReverseAvailable(false);
    setPaused(false);
    setRunning(false);
}

void DebuggerView::restyleIcons() {
    if (stepActions_.size() < 7 || !stopAction_) return;
    // Disabled actions are drawn by the style at reduced opacity, so one
    // colour is enough — the icons do not need a second, dimmer rasterization.
    stepActions_[0]->setIcon(NerdIcon(NF::DebugContinue, kIconSize, accent_));
    stepActions_[1]->setIcon(NerdIcon(NF::DebugStepOver, kIconSize, accent_));
    stepActions_[2]->setIcon(NerdIcon(NF::DebugStepInto, kIconSize, accent_));
    stepActions_[3]->setIcon(NerdIcon(NF::DebugStepOut, kIconSize, accent_));
    stepActions_[4]->setIcon(NerdIcon(NF::DebugReverseContinue, kIconSize, accent_));
    stepActions_[5]->setIcon(NerdIcon(NF::Rewind, kIconSize, accent_));
    stepActions_[6]->setIcon(NerdIcon(NF::DebugStepBack, kIconSize, accent_));
    stopAction_->setIcon(NerdIcon(NF::Stop, kIconSize, accent_));
}

void DebuggerView::applyTheme(const Theme& theme) {
    accent_ = theme.matchedBraceFg;
    dim_ = theme.lineNumberFg;

    // A header the same weight as its content is a header you read by
    // accident. Before this the column titles were the default near-white,
    // which made them the highest-contrast thing in a pane whose content was
    // empty — the eye went straight to "Name / Value" and found nothing.
    const QString bg = theme.editorBg.name();
    const QString fg = theme.editorFg.name();
    const QString dim = dim_.name();
    // The editor's `selectionBackground` is a dark red tuned for a few
    // characters of text. A full-width table row in it is a saturated block
    // that shouts louder than anything it contains — and red means nothing
    // here, in an app whose accent is amber. A selected row is a neutral lift
    // with the accent carried by the text instead.
    const QString sel = Mix(theme.editorBg, theme.editorFg, 0.16).name();
    const QString hover = Mix(theme.editorBg, theme.editorFg, 0.08).name();
    // The rules between panes: present enough to separate, quiet enough not to
    // become a grid.
    QColor ruleColor = dim_;
    ruleColor.setAlpha(70);
    const QString rule = QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(ruleColor.red()).arg(ruleColor.green())
        .arg(ruleColor.blue()).arg(ruleColor.alpha());
    // The header strip sits a touch above the pane so the two read as layers.
    const QString headerBg = Mix(theme.editorBg, theme.editorFg, 0.05).name();

    setStyleSheet(QStringLiteral(R"(
QToolBar { background: %1; border: none; padding: 3px 4px; spacing: 1px; }
QToolBar::separator { background: %5; width: 1px; margin: 4px 6px; }
QToolButton { border: none; border-radius: 4px; padding: 4px; }
QToolButton:hover:enabled { background: %6; }
QToolButton:pressed:enabled { background: %4; }
QTreeWidget, QPlainTextEdit, QLineEdit { background: %1; color: %2; border: none;
    selection-background-color: %4; selection-color: %8; }
QTreeWidget { alternate-background-color: %1; outline: none; }
QTreeWidget::item { padding: 2px 4px; border: none; }
QTreeWidget::item:selected { background: %4; color: %8; }
QTreeWidget::item:selected:active { background: %4; color: %8; }
QTreeWidget::item:hover:!selected { background: %6; }
/* The platform checkbox is a blue macOS control when checked and, on a
   near-black background, invisible when not — so a disabled breakpoint read as
   a row that simply had no checkbox, which is exactly how it was reported.
   Both states are drawn here, in the theme's own colours, at a fixed size so
   the column's text starts at the same x either way. */
QTreeWidget::indicator { width: 12px; height: 12px; border-radius: 3px;
    border: 1px solid %3; background: %1; margin-left: 2px; }
QTreeWidget::indicator:unchecked:hover { border-color: %8; }
QTreeWidget::indicator:checked { background: %8; border-color: %8; }
QHeaderView::section { background: %7; color: %3; border: none;
    border-bottom: 1px solid %5; padding: 3px 6px; font-weight: normal; }
QSplitter::handle { background: %5; }
QSplitter::handle:horizontal { width: 1px; }
QSplitter::handle:vertical { height: 1px; }
/* The rule sits on the row, not the field: it has to span the prompt too, or
   it reads as a line drawn under a stray label. */
QLineEdit { padding: 2px; }
#evalRow { background: %1; border-top: 1px solid %5; }
#evalPrompt { color: %8; background: transparent; }
#evalPrompt:disabled { color: %3; }
QScrollBar:vertical { background: %1; width: 10px; margin: 0; border: none; }
QScrollBar:horizontal { background: %1; height: 10px; margin: 0; border: none; }
QScrollBar::handle:vertical { background: %5; border-radius: 5px; min-height: 24px; }
QScrollBar::handle:horizontal { background: %5; border-radius: 5px; min-width: 24px; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; border: none; }
QScrollBar::add-page, QScrollBar::sub-page { background: %1; }
QScrollBar::corner { background: %1; border: none; }
)").arg(bg, fg, dim, sel, rule, hover, headerBg, accent_.name()));

    warning_->setStyleSheet(QStringLiteral(
        "QLabel { color: %1; background: %2; border-bottom: 1px solid %3; }")
        .arg(accent_.name(), Mix(theme.editorBg, accent_, 0.10).name(),
             Mix(theme.editorBg, theme.editorFg, 0.12).name()));
    for (PlaceholderTree* t : {stack_, variables_, breakpoints_}) {
        t->setPlaceholderColor(dim_);
    }
    restyleIcons();
    if (timeline_) timeline_->applyTheme(theme);
}

void DebuggerView::setTimelineVisible(bool visible) {
    timeline_->setVisible(visible);
}

void DebuggerView::setOutput(const QString& text) {
    console_->setPlainText(text);
    console_->moveCursor(QTextCursor::End);
    console_->ensureCursorVisible();
}

void DebuggerView::appendOutput(const QString& text) {
    if (text.isEmpty()) return;
    // appendPlainText inserts a newline between calls; DAP `output` events
    // already carry their own newlines, so move the cursor and insert raw to
    // avoid doubling them up.
    const bool atEnd = console_->textCursor().atEnd();
    console_->moveCursor(QTextCursor::End);
    console_->insertPlainText(text);
    if (atEnd) console_->ensureCursorVisible();
}

void DebuggerView::appendEvaluation(const QString& expression, const QString& result,
                                    bool ok) {
    // Prefixed, because the console interleaves these with the program's own
    // stdout and an unmarked value would read as something the program printed.
    appendOutput(QStringLiteral("\n> %1\n%2%3\n")
                     .arg(expression,
                          ok ? QString() : QStringLiteral("error: "),
                          result));
}

void DebuggerView::clearOutput() {
    console_->clear();
}

void DebuggerView::setFrames(const QVector<DebugSession::Frame>& frames, int selectedId) {
    // Blocked so rebuilding the list does not emit a selection change that
    // would be read as the user picking a frame, re-issuing scopes+variables
    // for a frame nobody asked about.
    const QSignalBlocker blocker(stack_);
    stack_->clear();
    for (const DebugSession::Frame& f : frames) {
        auto* item = new QTreeWidgetItem(stack_);
        item->setText(0, f.name);
        // The line alone, not `file:line`. Every frame of a normal stack is in
        // the same file, so repeating its name on every row spent the column
        // that the function name needed — and at this pane's width it elided
        // both into uselessness ("test...ur:8").
        item->setText(1, f.line > 0 ? QString::number(f.line) : QString());
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        // The full path is in the tooltip, where it also disambiguates a
        // basename collision.
        if (!f.filePath.isEmpty()) {
            item->setToolTip(0, QStringLiteral("%1:%2").arg(f.filePath).arg(f.line));
        }
        item->setForeground(1, dim_);
        item->setData(0, kFrameIdRole, f.id);
        if (f.id == selectedId) stack_->setCurrentItem(item);
    }
}

void DebuggerView::setVariables(const QVector<DebugSession::Variable>& vars) {
    variables_->clear();
    for (const DebugSession::Variable& v : vars) {
        auto* item = new QTreeWidgetItem(variables_);
        item->setText(0, v.name);
        item->setText(1, v.value);
        if (!v.type.isEmpty()) item->setToolTip(0, v.type);
    }
}

void DebuggerView::setBreakpoints(const QVector<BreakpointModel::Breakpoint>& bps,
                                  const QStringList& collidingBasenames) {
    // itemChanged fires while the tree is being filled — setCheckState and
    // setText both emit it — and every one of those would read as the user
    // clicking a checkbox.
    const QSignalBlocker blocker(breakpoints_);
    breakpoints_->clear();

    if (collidingBasenames.isEmpty()) {
        warning_->setVisible(false);
    } else {
        warning_->setText(QStringLiteral(
            "⚠ Two open files are named %1. `tur dap` matches breakpoints by "
            "basename, so a breakpoint in one binds in both.")
            .arg(collidingBasenames.join(QStringLiteral(", "))));
        warning_->setVisible(true);
    }

    for (const BreakpointModel::Breakpoint& b : bps) {
        auto* item = new QTreeWidgetItem(breakpoints_);
        item->setText(0, QStringLiteral("%1:%2")
                             .arg(QFileInfo(b.path).fileName()).arg(b.line));
        item->setToolTip(0, b.path);
        item->setCheckState(0, b.enabled ? Qt::Checked : Qt::Unchecked);
        item->setText(kBpConditionColumn, b.condition);
        // A disabled breakpoint reads as inert rather than as a row you might
        // have missed: it is not sent to the adapter at all.
        if (!b.enabled) item->setForeground(0, dim_);
        item->setToolTip(kBpConditionColumn, QStringLiteral(
            "Stop only when this holds. Two spellings are accepted: `i > 3` "
            "and `(> i 3)`. A condition that fails to evaluate stops anyway, "
            "so a broken one never silently swallows a breakpoint."));
        item->setData(0, kBpPathRole, b.path);
        item->setData(0, kBpLineRole, b.line);
        item->setFlags(item->flags() | Qt::ItemIsEditable | Qt::ItemIsUserCheckable);
    }
    // Hide the condition column when nothing has one. This pane is ~145px wide
    // at the default split; two columns there clipped the location to "t…5" and
    // the header to "Breakpoi". A conditional breakpoint is the exception, so
    // the column earns its width only when one exists.
    const bool anyCondition = std::any_of(
        bps.cbegin(), bps.cend(),
        [](const BreakpointModel::Breakpoint& b) { return !b.condition.isEmpty(); });
    breakpoints_->setColumnHidden(kBpConditionColumn, !anyCondition);
}

void DebuggerView::setPaused(bool paused) {
    for (QAction* a : stepActions_) a->setEnabled(paused);
    // Stop is enabled whenever a session is live (paused or running).
    if (stopAction_) stopAction_->setEnabled(true);
    // Two independent conditions, and both have to hold: you can only evaluate
    // while paused, and only in a session that has a live frame at all.
    setEvalLineEnabled(paused && evaluateAllowed_);
    if (paused) {
        // Reached from stateChanged(Paused), which arrives *after*
        // setRunning(true) has already put the running text in place. Without
        // this the panes read "Running." at a stop, which is the one moment
        // they are certainly not.
        stack_->setPlaceholder(QStringLiteral("No frames reported at this stop."));
        variables_->setPlaceholder(QStringLiteral("No locals in this frame."));
    }
}

void DebuggerView::setRunning(bool running) {
    // Stop is the only button that means anything while running.
    for (QAction* a : stepActions_) a->setEnabled(false);
    if (stopAction_) stopAction_->setEnabled(running);
    setEvalLineEnabled(false);
    stack_->setPlaceholder(running
        ? QStringLiteral("Running.\nThe stack appears when the program stops.")
        : QStringLiteral("No call stack.\nRun Debug Buffer to start a session."));
    variables_->setPlaceholder(running
        ? QStringLiteral("Running.\nLocals appear when the program stops.")
        : QStringLiteral("No locals.\nThey appear when the program stops."));
    if (!running) {
        stack_->clear();
        variables_->clear();
    }
}

void DebuggerView::setReverseAvailable(bool available) {
    for (QAction* a : reverseActions_) a->setVisible(available);
}

void DebuggerView::setEvalLineEnabled(bool enabled) {
    evalInput_->setEnabled(enabled);
    evalPrompt_->setEnabled(enabled);
}

void DebuggerView::setEvaluateEnabled(bool enabled, const QString& whyNot) {
    evaluateAllowed_ = enabled;
    setEvalLineEnabled(enabled);
    // Empty when it works: the prompt is the whole affordance. The reason it
    // does not work is the only thing worth spending the placeholder on.
    evalInput_->setPlaceholderText(enabled ? QString() : whyNot);
}

}
