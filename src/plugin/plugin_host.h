#pragma once

#include "turi_api.h"

#include <QObject>
#include <QString>
#include <QVector>

class QTimer;

namespace trowel {

class MainWindow;
class CommandRegistry;
class HookBus;
class SnippetSession;
class EditorView;
class SyntaxRegistry;
struct PluginContext;

// PluginHost owns the in-process Turmeric evaluation environment (TuriEnv)
// and, in later phases, the registered native API, hook bus, and plugin
// loading.  P0 is the linker proof: it creates an env, evals a string, and
// reports the result.  No natives, no UI beyond the debug eval action.
//
// The host is owned by MainWindow and lives on the Qt main thread.  All
// turi_eval / turi_call calls happen on this thread -- libturi is not
// thread-safe and Qt widgets are not thread-safe, so the two agree on the
// main thread by construction.
class PluginHost : public QObject {
    Q_OBJECT
public:
    explicit PluginHost(MainWindow* mainWindow, QObject* parent = nullptr);
    ~PluginHost() override;

    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    // Evaluate a Turmeric source string in the plugin env and return the
    // result as a display string.  On error, returns the diagnostic.
    // Used by the debug "Eval Turmeric..." action (P0) and later by
    // trowel:eval / plugin loading.
    QString eval(const QString& source);

    // Evaluate source and return the raw TuriValue (for internal use by
    // natives that need to eval in the plugin env).
    TuriValue evalRaw(const char* source);

    // Call a Turmeric closure with the given arguments via turi_call.
    // Used by HookBus to dispatch events.  Errors are returned as TURI_ERROR.
    TuriValue callClosure(TuriValue closure, const QVector<TuriValue>& args);

    // The underlying TuriEnv (for native registration and turi_call).
    TuriEnv* env() { return env_; }

    // True when the libturi link succeeded and the env is live.  False on
    // platforms with no prebuilt Turmeric (the host is a no-op then).
    bool isAvailable() const { return env_ != nullptr; }

    // Load all plugins from the user and bundled plugin directories.
    // Called at startup after natives are registered.
    void loadAll();

    // Create the PluginContext and register all native functions.  Must be
    // called before loadKeymap() and loadAll() so the keymap can use
    // trowel-set-keybinding.  Idempotent: safe to call once.
    void setupNatives();

    // Reload a single plugin by name (clears and re-evals its plugin.tur).
    bool reloadPlugin(const QString& name);

    // Wire the command registry and hook bus.  Called by MainWindow after
    // constructing the host and before loadAll().
    void setRegistry(CommandRegistry* registry) { registry_ = registry; }
    void setHookBus(HookBus* bus) { hookBus_ = bus; }
    CommandRegistry* registry() { return registry_; }
    HookBus* hookBus() { return hookBus_; }

    // Install the Tab-key event filter on an editor's Scintilla widget.
    // Called when a new editor buffer is created.  When a SnippetSession is
    // active and Tab is pressed, the filter consumes the event and advances
    // the snippet instead of letting Scintilla insert an indent.
    void installSnippetFilter(EditorView* editor);

    // The syntax registry (for syntax plugins).  Populated by loadSyntaxPlugins.
    SyntaxRegistry* syntaxRegistry() { return syntaxRegistry_; }

    // Load syntax plugins from ~/.trowel/syntax/<name>/syntax.tur.
    void loadSyntaxPlugins();

    // Load the user keymap from ~/.trowel/keymap.tur (P6).
    // Called after registerBuiltinCommands so command ids exist.
    void loadKeymap();

    // Start the QTimer-driven event loop pump for async Turmeric tasks (P7).
    // Calls turi_run_event_loop every ~16ms to drain ready fibers and timers.
    void startEventLoopPump();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    MainWindow* mainWindow_;
    TuriEnv* env_ = nullptr;
    CommandRegistry* registry_ = nullptr;
    HookBus* hookBus_ = nullptr;
    SnippetSession* snippet_ = nullptr;
    SyntaxRegistry* syntaxRegistry_ = nullptr;
    QTimer* eventLoopTimer_ = nullptr;
    QStringList loadedPlugins_;
    PluginContext* pluginCtx_ = nullptr;  // owned by the host
};

}  // namespace trowel
