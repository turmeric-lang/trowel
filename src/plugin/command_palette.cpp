#include "command_palette.h"

#include <QApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QScreen>
#include <QVBoxLayout>

#include <algorithm>

namespace trowel {

// Fuzzy subsequence match: returns a score (higher = better) or -1 if `query`
// is not a subsequence of `target`.  Case-insensitive.  Consecutive matches
// score higher (density bonus), and matches near the start score higher.
static int fuzzyScore(const QString& query, const QString& target)
{
    if (query.isEmpty()) return 0;  // empty query matches everything, neutral

    const auto q = query.toLower();
    const auto t = target.toLower();

    int score = 0;
    int qi = 0;
    int consecutive = 0;
    int prevMatchPos = -1;

    for (int ti = 0; ti < t.size() && qi < q.size(); ++ti) {
        if (t[ti] == q[qi]) {
            // Density bonus: consecutive matches are worth more.
            if (prevMatchPos >= 0 && ti == prevMatchPos + 1) {
                consecutive++;
                score += 5 + consecutive;
            } else {
                consecutive = 0;
                score += 1;
            }
            // Position bonus: matches near the start are worth more.
            if (ti < 3) score += 10;
            else if (ti < 10) score += 5;

            prevMatchPos = ti;
            ++qi;
        }
    }

    if (qi < q.size()) return -1;  // not all query chars matched

    // Word-boundary bonus: matching the start of a word (after space, camelCase,
    // or after a dot in a command id) is worth extra.
    for (int ti = 0; ti < t.size() && ti < prevMatchPos; ++ti) {
        if (t[ti] == q[0] && (ti == 0 || t[ti-1] == ' ' || t[ti-1] == '.'
                              || t[ti-1] == '-' || t[ti-1] == '_')) {
            score += 3;
            break;
        }
    }

    return score;
}

CommandPalette::CommandPalette(CommandRegistry* registry, QWidget* parent)
    : QFrame(parent), registry_(registry)
{
    setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
    setFrameShape(QFrame::NoFrame);
    setAttribute(Qt::WA_DeleteOnClose, false);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    filter_ = new QLineEdit(this);
    filter_->setPlaceholderText("Type a command name...");
    filter_->setClearButtonEnabled(false);
    layout->addWidget(filter_);

    list_ = new QListWidget(this);
    list_->setUniformItemSizes(true);
    list_->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(list_);

    setStyleSheet(
        "QFrame { background: palette(window); border: 1px solid palette(mid); "
        "  border-radius: 6px; }"
        "QLineEdit { padding: 8px 12px; border: none; border-bottom: 1px solid "
        "  palette(mid); font-size: 14px; background: transparent; }"
        "QListWidget { border: none; background: palette(base); }"
        "QListWidget::item { padding: 6px 12px; }"
        "QListWidget::item:selected { background: palette(highlight); "
        "  color: palette(highlighted-text); }");

    connect(filter_, &QLineEdit::textChanged, this, &CommandPalette::onFilterChanged);
    connect(list_, &QListWidget::itemActivated, this, &CommandPalette::onActivated);

    filter_->installEventFilter(this);
}

void CommandPalette::popup(QWidget* host)
{
    host_ = host;
    rebuildList();

    // Size: ~50% of host width, centered horizontally, ~40% down from top.
    const int w = qMax(400, host->width() / 2);
    const int h = qMin(400, qMax(200, host->height() / 2));
    resize(w, h);

    const QPoint topLeft = host->mapToGlobal(QPoint(
        (host->width() - w) / 2,
        host->height() * 2 / 5 - h / 2));
    move(topLeft);

    filter_->clear();
    filter_->setFocus();
    show();
    raise();
    activateWindow();
}

bool CommandPalette::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == filter_ && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        switch (ke->key()) {
        case Qt::Key_Down:
            cycleNext();
            return true;
        case Qt::Key_Up:
            cyclePrev();
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            runCurrent();
            return true;
        case Qt::Key_Escape:
            close();
            return true;
        case Qt::Key_P: {
            // Cmd-P / Ctrl-P cycles to the next match (VSCode behavior).
            const bool mod = ke->modifiers() & (Qt::ControlModifier | Qt::MetaModifier);
            if (mod) {
                cycleNext();
                return true;
            }
            break;
        }
        default:
            break;
        }
    }
    return QFrame::eventFilter(obj, event);
}

void CommandPalette::onFilterChanged(const QString& text)
{
    matches_.clear();

    const auto& cmds = registry_->commands();
    for (int i = 0; i < cmds.size(); ++i) {
        // Match against "category: title" for a natural search experience.
        const QString haystack = cmds[i].category.isEmpty()
            ? cmds[i].title
            : cmds[i].category + ": " + cmds[i].title;
        const int s = fuzzyScore(text, haystack);
        if (s >= 0) {
            matches_.append({i, s, haystack});
        }
    }

    // Sort by score descending, then alphabetically.
    std::sort(matches_.begin(), matches_.end(),
              [](const Match& a, const Match& b) {
                  if (a.score != b.score) return a.score > b.score;
                  return a.title.compare(b.title, Qt::CaseInsensitive) < 0;
              });

    // Rebuild the list widget.
    list_->clear();
    for (const auto& m : matches_) {
        const auto& cmd = registry_->commands()[m.cmdIndex];
        QString label = m.title;
        if (!cmd.shortcut.isEmpty())
            label += "  " + cmd.shortcut.toString();
        auto* item = new QListWidgetItem(label, list_);
        item->setData(Qt::UserRole, m.cmdIndex);
    }

    if (list_->count() > 0)
        list_->setCurrentRow(0);
}

void CommandPalette::rebuildList()
{
    onFilterChanged(QString());
}

void CommandPalette::onActivated()
{
    runCurrent();
}

void CommandPalette::runCurrent()
{
    int row = list_->currentRow();
    if (row < 0 || row >= matches_.size()) return;
    const auto& cmd = registry_->commands()[matches_[row].cmdIndex];
    const QString id = cmd.id;
    close();
    registry_->run(id);
    emit commandRun(id);
}

void CommandPalette::cycleNext()
{
    if (list_->count() == 0) return;
    int row = list_->currentRow();
    if (row < 0) row = 0;
    else row = (row + 1) % list_->count();
    list_->setCurrentRow(row);
}

void CommandPalette::cyclePrev()
{
    if (list_->count() == 0) return;
    int row = list_->currentRow();
    if (row < 0) row = 0;
    else row = (row - 1 + list_->count()) % list_->count();
    list_->setCurrentRow(row);
}

}  // namespace trowel
