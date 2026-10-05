#pragma once

#include <QHash>
#include <QKeySequence>
#include <QObject>
#include <QString>
#include <QVector>

#include <functional>

namespace trowel {

// One command the palette can show and run.  Built-in commands lift their
// existing QAction handler; plugin commands (P2+) supply a Turmeric closure
// that the PluginHost calls.
struct CommandEntry {
    QString id;          // "trowel.run-buffer", "snippets.expand"
    QString title;       // "Run Buffer", "Expand Snippet"
    QString category;    // "Run", "Snippets"
    QKeySequence shortcut;  // optional
    std::function<void()> handler;  // what runs when selected
};

// Holds every command the palette can show.  Built-in commands register at
// startup; plugin commands register through the trowel:register-command native
// (P2).  Lookup is by id; run() dispatches to the handler.
//
// The registry is owned by MainWindow.  All calls happen on the Qt main thread.
class CommandRegistry : public QObject {
    Q_OBJECT
public:
    explicit CommandRegistry(QObject* parent = nullptr);

    // Add or replace a command by id.  If a command with the same id exists,
    // its fields are overwritten (this is how reload works).
    void add(const CommandEntry& cmd);

    // Remove a command by id (e.g. when a plugin unloads).
    void remove(const QString& id);

    // Look up a command by id.  Returns nullptr if not found.
    const CommandEntry* find(const QString& id) const;

    // Run a command by id.  Returns false if the id is not registered.
    bool run(const QString& id) const;

    // Set the keyboard shortcut for a command by id.  Returns false if the
    // command is not found.  Used by the keymap.tur override mechanism (P6).
    bool setShortcut(const QString& id, const QKeySequence& shortcut);

    // All registered commands, in insertion order.  The palette iterates
    // these to build its list.
    const QVector<CommandEntry>& commands() const { return commands_; }

    // Number of registered commands.
    int count() const { return commands_.size(); }

private:
    QVector<CommandEntry> commands_;
    QHash<QString, int> index_;  // id → index into commands_
};

}  // namespace trowel
