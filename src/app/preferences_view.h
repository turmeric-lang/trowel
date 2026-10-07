#pragma once

#include "app/tab_content.h"

class QCheckBox;
class QComboBox;
class QLineEdit;

namespace trowel {

class PreferencesView : public TabContent {
    Q_OBJECT
public:
    explicit PreferencesView(QWidget* parent = nullptr);

    Kind kind() const override { return Kind::Preferences; }
    QString displayName() const override { return QStringLiteral("Trowel Settings"); }

signals:
    // Emitted when the user toggles rainbow brackets so open editors can update.
    void rainbowBracketsChanged(bool enabled);
    // Likewise for the active bracket-pair guide.
    void bracketPairGuidesChanged(bool enabled);

private slots:
    void commitTurmericPath();
    void commitRainbowBrackets(bool enabled);
    void commitBracketPairGuides(bool enabled);
    void commitLspEnabled(bool enabled);
    void commitEngine(int index);
    void restoreDefaults();

private:
    QLineEdit* turPathEdit_ = nullptr;
    QCheckBox* rainbowCheck_ = nullptr;
    QCheckBox* bracketGuideCheck_ = nullptr;
    QCheckBox* lspCheck_ = nullptr;
    QComboBox* engineCombo_ = nullptr;
};

}
