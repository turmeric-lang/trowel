#include "command_registry.h"

namespace trowel {

CommandRegistry::CommandRegistry(QObject* parent)
    : QObject(parent)
{
}

void CommandRegistry::add(const CommandEntry& cmd)
{
    auto it = index_.constFind(cmd.id);
    if (it != index_.constEnd()) {
        commands_[it.value()] = cmd;
        return;
    }
    index_.insert(cmd.id, commands_.size());
    commands_.append(cmd);
}

void CommandRegistry::remove(const QString& id)
{
    auto it = index_.constFind(id);
    if (it == index_.constEnd()) return;
    const int idx = it.value();
    commands_.removeAt(idx);
    index_.remove(id);
    // Rebuild the index — removal shifts everything after idx.
    index_.clear();
    for (int i = 0; i < commands_.size(); ++i)
        index_.insert(commands_[i].id, i);
}

const CommandEntry* CommandRegistry::find(const QString& id) const
{
    auto it = index_.constFind(id);
    if (it == index_.constEnd()) return nullptr;
    return &commands_[it.value()];
}

bool CommandRegistry::run(const QString& id) const
{
    auto it = index_.constFind(id);
    if (it == index_.constEnd()) return false;
    const auto& cmd = commands_[it.value()];
    if (cmd.handler) cmd.handler();
    return true;
}

bool CommandRegistry::setShortcut(const QString& id, const QKeySequence& shortcut)
{
    auto it = index_.find(id);
    if (it == index_.end()) return false;
    commands_[it.value()].shortcut = shortcut;
    return true;
}

}  // namespace trowel
