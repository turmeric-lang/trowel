#pragma once

#include "turi_api.h"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
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

// What a plugin's manifest.tur declares.  The manifest is DATA: the host
// scans it for `:capabilities [...]` and `:description "..."` and never
// evaluates it, so reading a manifest cannot run plugin code.
struct PluginManifest {
    bool present = false;
    QString description;
    QStringList capabilityNames;  // as written: "fs:read", "proc", ...
    QStringList unknownCapabilities;
    TuriCaps caps = TURI_CAP_NONE;
    bool importAllPlugins = false;  // "import:plugins": resolve imports from the plugins root
    QByteArray hash;  // sha256 hex of the manifest file, for re-prompting
};

// PluginHost owns the in-process Turmeric environments (TuriEnv), the
// registered native API, and plugin loading.
//
// Isolation model: `env_` is the host's own env ("API env") -- the REPL eval
// action, keymap.tur and syntax.tur run there, and it holds the trowel-*
// natives.  Each plugin gets a separate sandboxed env with those natives
// re-registered into it and only the stdlib capabilities its manifest
// declares and the user granted.  A plugin cannot see another plugin's
// definitions, and resetting or freeing one env leaves the rest alone.
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

    // Call a Turmeric closure with the given arguments via turi_call.  `env`
    // must be the env the closure was created in (the calling env that the
    // native received).  Errors are returned as TURI_ERROR.
    TuriValue callClosure(TuriEnv* env, TuriValue closure,
                          const QVector<TuriValue>& args);

    // The host (API) TuriEnv: REPL eval, keymap and syntax plugins.
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

    // Reload a single plugin by name: tears down its env (and the commands,
    // buttons and hooks it registered) and loads it afresh.  Other plugins
    // are untouched.
    bool reloadPlugin(const QString& name);

    // True if `name` is a currently loaded plugin.
    bool hasPlugin(const QString& name) const { return plugins_.contains(name); }
    QStringList loadedPlugins() const { return plugins_.keys(); }

    // The capabilities a loaded plugin actually holds (after the permission
    // check), or TURI_CAP_NONE.  For tests and diagnostics.
    TuriCaps grantedCaps(const QString& name) const;

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
    PluginContext* pluginCtx_ = nullptr;  // owned by the host

    struct PluginSource {
        QString path;  // .../plugin.tur (or :/plugins/... when bundled)
        bool bundled = false;
    };
    struct PluginRecord {
        QString name;
        TuriEnv* env = nullptr;
        PluginManifest manifest;
        TuriCaps granted = TURI_CAP_NONE;
    };
    QHash<QString, PluginSource> discovered_;  // name → where it lives
    QHash<QString, PluginRecord> plugins_;     // name → loaded plugin

    bool loadPlugin(const QString& name, const PluginSource& src);
    void unloadPlugin(const QString& name);
    static PluginManifest parseManifest(const QString& manifestPath);
    // Decide whether the user allows `manifest`'s capabilities for `name`:
    // consults the cache in ~/.trowel/plugin-permissions.json, prompts on a
    // miss or a changed manifest, and records the answer.
    bool capabilitiesAllowed(const QString& name, const PluginManifest& manifest);
};

}  // namespace trowel
