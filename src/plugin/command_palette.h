#pragma once

#include "command_registry.h"

#include <QFrame>
#include <QListWidget>
#include <QString>

class QLineEdit;
class QListWidget;
class QLabel;

namespace trowel {

class CommandRegistry;

// A fuzzy-filtering command palette popup (VSCode-style).  Invoked by Cmd-P.
// Shows all registered commands in a single searchable list; selecting an
// entry runs it.
//
// The palette is a frameless QFrame that overlays the editor.  It appears on
// show(), dismisses on Esc or after running a command.  The filter is a
// subsequence match scored by match density.
class CommandPalette : public QFrame {
    Q_OBJECT
public:
    CommandPalette(CommandRegistry* registry, QWidget* parent = nullptr);

    // Show the palette centered over `host`, ~40% down from the top.
    void popup(QWidget* host);

signals:
    // Emitted when a command is selected and run.  The id is the command id.
    void commandRun(const QString& id);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void onFilterChanged(const QString& text);
    void onActivated();
    void cycleNext();
    void cyclePrev();

private:
    struct Match {
        int cmdIndex;   // index into registry->commands()
        int score;      // higher = better
        QString title;  // display text
    };

    void rebuildList();
    void runCurrent();

    CommandRegistry* registry_;
    QLineEdit* filter_ = nullptr;
    QListWidget* list_ = nullptr;
    QWidget* host_ = nullptr;
    QVector<Match> matches_;
};

}  // namespace trowel
