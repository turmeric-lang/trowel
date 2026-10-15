#pragma once

#include "turi_api.h"

#include <QHash>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

class QAction;

namespace trowel {

class MainWindow;
class CommandRegistry;
class HookBus;
class PluginHost;
class EditorView;
class SnippetSession;
class SyntaxRegistry;

// What one plugin env has contributed to the UI, so unloading the plugin can
// take it all back out.  Keyed by the plugin's TuriEnv in PluginContext.
struct PluginContributions {
    QStringList commandIds;
    QVector<QPointer<QAction>> actions;
};

// Context passed to every native function as the `ud` pointer.  Holds the
// C++ objects the natives need to marshal between Turmeric and Trowel.
struct PluginContext {
    PluginHost* host = nullptr;
    MainWindow* window = nullptr;
    CommandRegistry* registry = nullptr;
    HookBus* hookBus = nullptr;
    // The active snippet session (one at a time, on the active editor).
    SnippetSession* snippet = nullptr;
    // Bookmark state: file path → set of 1-based lines. Managed by the
    // bookmark natives (trowel-bookmark-toggle/next/prev/clear) and
    // persisted to ~/.trowel/bookmarks.tur.
    QHash<QString, QSet<int>>* bookmarks = nullptr;
    // Commands and side-bar buttons registered per plugin env (per-plugin
    // isolation): the natives record into the calling env's entry.
    QHash<TuriEnv*, PluginContributions> contributions;
};

// Register all v1 native functions into the plugin env.  Called by
// PluginHost after creating the env.
void registerPluginNatives(TuriEnv* env, PluginContext* ctx);

// Register syntax-descriptor natives for syntax plugins.  Called by
// PluginHost before loading syntax.tur files.
void registerSyntaxNatives(TuriEnv* env, SyntaxRegistry* registry);

}  // namespace trowel
