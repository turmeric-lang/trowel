#include "app/preferences_view.h"

#include "app/settings.h"
#include "editor/editor_view.h"
#include "editor/theme_loader.h"
#include "lsp/lsp_manager.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

namespace trowel {

PreferencesView::PreferencesView(QWidget* parent)
    : TabContent(parent)
{
    const Theme theme = LoadBuiltinDarkTheme();

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);

    auto* title = new QLabel(QStringLiteral("Trowel Settings"), this);
    QFont titleFont = title->font();
    const int basePt = titleFont.pointSize();
    if (basePt > 0) titleFont.setPointSize(basePt + 4);
    titleFont.setBold(true);
    title->setFont(titleFont);
    root->addWidget(title);

    auto* pathLabel = new QLabel(QStringLiteral("Turmeric path"), this);
    root->addWidget(pathLabel);

    turPathEdit_ = new QLineEdit(this);
    turPathEdit_->setPlaceholderText(QStringLiteral("Path to the `tur` executable (leave blank to auto-detect)"));
    turPathEdit_->setText(QSettings().value("repl/turBinary").toString());
    turPathEdit_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(turPathEdit_, &QLineEdit::editingFinished,
            this, &PreferencesView::commitTurmericPath);
    root->addWidget(turPathEdit_);

    rainbowCheck_ = new QCheckBox(QStringLiteral("Rainbow brackets"), this);
    rainbowCheck_->setToolTip(QStringLiteral(
        "Color matching parentheses, brackets, and braces by nesting depth."));
    rainbowCheck_->setChecked(EditorView::rainbowBracketsDefault());
    connect(rainbowCheck_, &QCheckBox::toggled,
            this, &PreferencesView::commitRainbowBrackets);
    root->addWidget(rainbowCheck_);

    bracketGuideCheck_ = new QCheckBox(QStringLiteral("Bracket pair guide"), this);
    bracketGuideCheck_->setToolTip(QStringLiteral(
        "Underline the expression enclosing the caret, in that pair's own "
        "nesting-depth color. Falls back to the indent-guide color when "
        "rainbow brackets are off."));
    bracketGuideCheck_->setChecked(EditorView::bracketPairGuidesDefault());
    connect(bracketGuideCheck_, &QCheckBox::toggled,
            this, &PreferencesView::commitBracketPairGuides);
    root->addWidget(bracketGuideCheck_);

    lspCheck_ = new QCheckBox(QStringLiteral("Language server"), this);
    lspCheck_->setToolTip(QStringLiteral(
        "Run `tur lsp` for inline errors, completion, and hover documentation. "
        "Takes effect on restart."));
    lspCheck_->setChecked(LspManager::enabledInSettings());
    connect(lspCheck_, &QCheckBox::toggled, this, &PreferencesView::commitLspEnabled);
    root->addWidget(lspCheck_);

    auto* engineLabel = new QLabel(QStringLiteral("Run engine"), this);
    root->addWidget(engineLabel);

    engineCombo_ = new QComboBox(this);
    engineCombo_->setToolTip(QStringLiteral(
        "Which Turmeric execution engine to use for Build Project / Run. "
        "\"Default\" respects the project's build.tur :engine; the others "
        "override it via TUR_ENGINE. Does not affect the REPL, which is "
        "always tree-walked."));
    // Store the string value as item data, never the combo index — indices
    // break when the list is reordered or filtered.
    engineCombo_->addItem(QStringLiteral("Default"), QStringLiteral("default"));
    engineCombo_->addItem(QStringLiteral("cc (C emitter)"), QStringLiteral("cc"));
    engineCombo_->addItem(QStringLiteral("jit (MIR JIT)"), QStringLiteral("jit"));
    engineCombo_->addItem(QStringLiteral("interp (tree-walker)"), QStringLiteral("interp"));
    const QString currentEngine = Settings::instance().runEngine();
    for (int i = 0; i < engineCombo_->count(); ++i) {
        if (engineCombo_->itemData(i).toString() == currentEngine) {
            engineCombo_->setCurrentIndex(i);
            break;
        }
    }
    connect(engineCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &PreferencesView::commitEngine);
    root->addWidget(engineCombo_);

    root->addStretch(1);

    auto* buttonRow = new QHBoxLayout();
    buttonRow->setContentsMargins(0, 0, 0, 0);
    buttonRow->addStretch(1);
    auto* restoreButton = new QPushButton(QStringLiteral("Restore Defaults"), this);
    connect(restoreButton, &QPushButton::clicked,
            this, &PreferencesView::restoreDefaults);
    buttonRow->addWidget(restoreButton);
    root->addLayout(buttonRow);

    setStyleSheet(QString(
        "QWidget { background: %1; color: %2; }"
        "QLabel { background: transparent; }"
        "QLineEdit { background: %1; color: %2; border: 1px solid %3; padding: 4px; }"
        "QComboBox { background: %1; color: %2; border: 1px solid %3; padding: 4px; }"
        "QComboBox QAbstractItemView { background: %1; color: %2; selection-background-color: %3; }"
        "QPushButton { background: %1; color: %2; border: 1px solid %3; padding: 4px 12px; }"
        "QPushButton:hover { background: %3; }"
    ).arg(theme.editorBg.name(),
          theme.editorFg.name(),
          theme.lineNumberFg.name()));
}

void PreferencesView::commitTurmericPath() {
    if (!turPathEdit_) return;
    const QString value = turPathEdit_->text().trimmed();
    QSettings settings;
    if (value.isEmpty()) {
        settings.remove("repl/turBinary");
    } else {
        settings.setValue("repl/turBinary", value);
    }
}

void PreferencesView::commitRainbowBrackets(bool enabled) {
    QSettings().setValue("editor/rainbowBrackets", enabled);
    emit rainbowBracketsChanged(enabled);
}

void PreferencesView::commitBracketPairGuides(bool enabled) {
    QSettings().setValue("editor/bracketPairGuides", enabled);
    emit bracketPairGuidesChanged(enabled);
}

void PreferencesView::commitLspEnabled(bool enabled) {
    QSettings().setValue("lsp/enabled", enabled);
    // Turning it on mid-session is safe — the manager starts lazily on the next
    // Turmeric document. Turning it off only takes effect on restart, because
    // LspManager latches Disabled at construction.
    if (!enabled) LspManager::instance()->shutdown();
}

void PreferencesView::commitEngine(int) {
    if (!engineCombo_) return;
    const QString engine = engineCombo_->currentData().toString();
    Settings::instance().setRunEngine(engine);
}

void PreferencesView::restoreDefaults() {
    QSettings().remove("repl/turBinary");
    if (turPathEdit_) turPathEdit_->clear();
    QSettings().remove("editor/rainbowBrackets");
    if (rainbowCheck_) rainbowCheck_->setChecked(true);
    QSettings().remove("editor/bracketPairGuides");
    if (bracketGuideCheck_) bracketGuideCheck_->setChecked(true);
    QSettings().remove("lsp/enabled");
    if (lspCheck_) lspCheck_->setChecked(true);
    Settings::instance().setRunEngine(QStringLiteral("default"));
    if (engineCombo_) engineCombo_->setCurrentIndex(0);
}

}
