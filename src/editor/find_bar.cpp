#include "editor/find_bar.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace trowel {

FindBar::FindBar(QWidget* parent)
    : QWidget(parent)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 2, 4, 2);
    outer->setSpacing(2);

    // --- Find row ---
    auto* findRow = new QHBoxLayout;
    findRow->setSpacing(4);

    queryEdit_ = new QLineEdit(this);
    queryEdit_->setPlaceholderText("Find");
    queryEdit_->setClearButtonEnabled(true);

    matchCaseCheck_ = new QCheckBox("Aa", this);
    matchCaseCheck_->setToolTip("Match Case");
    matchCaseCheck_->setFixedSize(28, 28);
    wholeWordCheck_ = new QCheckBox("W", this);
    wholeWordCheck_->setToolTip("Whole Word");
    wholeWordCheck_->setFixedSize(28, 28);
    regexCheck_ = new QCheckBox(".*", this);
    regexCheck_->setToolTip("Regex");
    regexCheck_->setFixedSize(28, 28);
    inSelectionCheck_ = new QCheckBox("=", this);
    inSelectionCheck_->setToolTip("In Selection");
    inSelectionCheck_->setFixedSize(28, 28);

    countLabel_ = new QLabel(this);
    countLabel_->setMinimumWidth(60);

    prevBtn_ = new QPushButton("\u2191", this);  // up arrow
    prevBtn_->setFixedSize(28, 28);
    prevBtn_->setToolTip("Find Previous");
    nextBtn_ = new QPushButton("\u2193", this);  // down arrow
    nextBtn_->setFixedSize(28, 28);
    nextBtn_->setToolTip("Find Next");
    closeBtn_ = new QPushButton("\u00d7", this);
    closeBtn_->setFixedSize(28, 28);
    closeBtn_->setToolTip("Close");

    findRow->addWidget(queryEdit_);
    findRow->addWidget(matchCaseCheck_);
    findRow->addWidget(wholeWordCheck_);
    findRow->addWidget(regexCheck_);
    findRow->addWidget(inSelectionCheck_);
    findRow->addWidget(countLabel_);
    findRow->addWidget(prevBtn_);
    findRow->addWidget(nextBtn_);
    findRow->addWidget(closeBtn_);
    outer->addLayout(findRow);

    // --- Replace row ---
    replaceRow_ = new QWidget(this);
    auto* replaceLayout = new QHBoxLayout(replaceRow_);
    replaceLayout->setContentsMargins(0, 0, 0, 0);
    replaceLayout->setSpacing(4);

    replaceEdit_ = new QLineEdit(replaceRow_);
    replaceEdit_->setPlaceholderText("Replace");

    replaceBtn_ = new QPushButton("Replace", replaceRow_);
    replaceAllBtn_ = new QPushButton("All", replaceRow_);

    replaceLayout->addWidget(replaceEdit_);
    replaceLayout->addWidget(replaceBtn_);
    replaceLayout->addWidget(replaceAllBtn_);
    replaceLayout->addStretch();
    outer->addWidget(replaceRow_);
    replaceRow_->setVisible(false);

    // --- Connections ---
    connect(queryEdit_, &QLineEdit::textChanged, this, &FindBar::searchChanged);
    connect(matchCaseCheck_, &QCheckBox::toggled, this, &FindBar::searchChanged);
    connect(wholeWordCheck_, &QCheckBox::toggled, this, &FindBar::searchChanged);
    connect(regexCheck_, &QCheckBox::toggled, this, &FindBar::searchChanged);
    connect(inSelectionCheck_, &QCheckBox::toggled, this, &FindBar::searchChanged);

    connect(nextBtn_, &QPushButton::clicked, this, &FindBar::findNextRequested);
    connect(prevBtn_, &QPushButton::clicked, this, &FindBar::findPreviousRequested);
    connect(replaceBtn_, &QPushButton::clicked, this, &FindBar::replaceRequested);
    connect(replaceAllBtn_, &QPushButton::clicked, this, &FindBar::replaceAllRequested);
    connect(closeBtn_, &QPushButton::clicked, this, &FindBar::closed);

    // Keyboard handling via event filter on the line edits.
    queryEdit_->installEventFilter(this);
    replaceEdit_->installEventFilter(this);
}

void FindBar::setReplaceMode(bool replace) {
    replaceMode_ = replace;
    replaceRow_->setVisible(replace);
}

QString FindBar::query() const {
    return queryEdit_->text();
}

void FindBar::setQuery(const QString& text) {
    queryEdit_->setText(text);
}

bool FindBar::matchCase() const { return matchCaseCheck_->isChecked(); }
bool FindBar::wholeWord() const { return wholeWordCheck_->isChecked(); }
bool FindBar::regex() const { return regexCheck_->isChecked(); }
bool FindBar::inSelection() const { return inSelectionCheck_->isChecked(); }

void FindBar::setFlags(bool mc, bool ww, bool re, bool is) {
    matchCaseCheck_->setChecked(mc);
    wholeWordCheck_->setChecked(ww);
    regexCheck_->setChecked(re);
    inSelectionCheck_->setChecked(is);
}

void FindBar::setMatchCount(int current, int total) {
    if (total == 0) {
        countLabel_->setText("No results");
    } else if (total >= 10000) {
        countLabel_->setText(QString("%1 of 10000+").arg(current + 1));
    } else {
        countLabel_->setText(QString("%1 of %2").arg(current + 1).arg(total));
    }
}

void FindBar::setError(const QString& error) {
    if (error.isEmpty()) {
        queryEdit_->setStyleSheet("");
        queryEdit_->setToolTip("");
    } else {
        queryEdit_->setStyleSheet("QLineEdit { background: #5a0000; }");
        queryEdit_->setToolTip(error);
    }
}

void FindBar::open(bool replace) {
    setReplaceMode(replace);
    show();
    queryEdit_->setFocus();
    queryEdit_->selectAll();
}

void FindBar::closeBar() {
    hide();
}

bool FindBar::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Escape) {
            emit closed();
            return true;
        }
        if (watched == queryEdit_) {
            if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
                if (ke->modifiers() & Qt::ShiftModifier)
                    emit findPreviousRequested();
                else
                    emit findNextRequested();
                return true;
            }
        }
        if (watched == replaceEdit_) {
            if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
                if (ke->modifiers() & (Qt::ControlModifier | Qt::AltModifier))
                    emit replaceAllRequested();
                else
                    emit replaceRequested();
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

}  // namespace trowel
