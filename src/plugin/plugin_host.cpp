#include "plugin_host.h"

#include "app/main_window.h"
#include "command_registry.h"
#include "editor/editor_view.h"
#include "editor/lexers.h"
#include "hook_bus.h"
#include "plugin_natives.h"
#include "snippet_session.h"
#include "syntax_descriptor.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMessageBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>

#include <ScintillaEdit.h>

namespace trowel {

PluginHost::PluginHost(MainWindow* mainWindow, QObject* parent)
    : QObject(parent), mainWindow_(mainWindow)
{
    // turi_init initialises the diagnostics subsystem.  Call once at startup.
    // No colour: output is not a terminal in the embedded context.
    turi_init(false);

    // Create an unrestricted env.  turi_env_new auto-loads the stdlib preload
    // (collections, typeclasses, macros, native stubs) so basic Turmeric
    // forms like (+ 1 2) and (vec-of ...) work out of the box.
    env_ = turi_env_new();

    // P4: the snippet session is shared across editors (one active at a time).
    snippet_ = new SnippetSession();

    // P0 proof: eval (+ 1 2) and assert the result is 3.  This is the linker
    // smoke test -- if it fails, the libturi link is broken.
    if (env_) {
        TuriValue result = turi_eval(env_, "(+ 1 2)");
        if (turi_is_error(result)) {
            char buf[256];
            turi_value_repr(buf, sizeof(buf), result);
            qWarning("[PluginHost] startup eval failed: %s", buf);
        } else if (result.tag == TURI_INT && result.as_int == 3) {
            qDebug("[PluginHost] libturi linked and evaluating in-process");
        } else {
            char buf[256];
            turi_value_repr(buf, sizeof(buf), result);
            qWarning("[PluginHost] startup eval returned %s (expected 3)", buf);
        }
    }
}

PluginHost::~PluginHost()
{
    if (env_) {
        turi_env_free(env_);
        env_ = nullptr;
    }
    delete snippet_;
    snippet_ = nullptr;
}

bool PluginHost::eventFilter(QObject* obj, QEvent* event)
{
    if (event->type() == QEvent::KeyPress && snippet_ && snippet_->active()) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Tab) {
            // Tab while a snippet is active: advance to the next tab-stop
            // instead of letting Scintilla insert an indent.
            snippet_->advance();
            // Select the new stop (or place cursor at ${0}).
            auto* e = mainWindow_ ? mainWindow_->editorView() : nullptr;
            if (e) {
                // After advance(), if the snippet is still active, select the
                // current stop; if it was cleared, the cursor stays where ${0}
                // was placed.
                if (snippet_->active()) {
                    const auto& stops = snippet_->stops();
                    int idx = snippet_->current();
                    if (idx >= 0 && idx < stops.size()) {
                        e->setSelection(stops[idx].start, stops[idx].end);
                    }
                }
            }
            return true;  // consume the event
        }
        if (ke->key() == Qt::Key_Escape) {
            snippet_->clear();
            // Let the event fall through to Scintilla's default handling.
        }
    }
    return QObject::eventFilter(obj, event);
}

void PluginHost::installSnippetFilter(EditorView* editor)
{
    if (!editor || !editor->sciWidget()) return;
    editor->sciWidget()->installEventFilter(this);
}

QString PluginHost::eval(const QString& source)
{
    if (!env_) {
        return QStringLiteral("[plugin host unavailable: libturi not linked]");
    }

    TuriValue result = turi_eval(env_, source.toUtf8().constData());

    char buf[1024];
    if (turi_is_error(result)) {
        turi_value_repr(buf, sizeof(buf), result);
        return QStringLiteral("Error: %1").arg(QString::fromUtf8(buf));
    }

    turi_value_repr(buf, sizeof(buf), result);
    return QString::fromUtf8(buf);
}

TuriValue PluginHost::evalRaw(const char* source)
{
    if (!env_) return turi_error("no env");
    return turi_eval(env_, source);
}

TuriValue PluginHost::callClosure(TuriValue closure, const QVector<TuriValue>& args)
{
    if (!env_) return turi_error("no env");
    if (closure.tag != TURI_CLOSURE) return turi_error("not a closure");

    // turi_call takes a raw pointer + count.  Make a non-const copy since
    // QVector::data() on a const vector returns a const pointer.
    if (args.isEmpty()) {
        return turi_call(env_, closure, nullptr, 0);
    }
    QVector<TuriValue> argCopy = args;
    return turi_call(env_, closure, argCopy.data(),
                     static_cast<uint32_t>(argCopy.size()));
}

void PluginHost::loadAll()
{
    if (!env_) return;

    // Register natives before loading plugins so plugin.tur can call them.
    // The context is heap-allocated and owned by the host (lives as long as
    // the env, which is freed in ~PluginHost).
    auto* pluginCtx = new PluginContext;
    pluginCtx->host = this;
    pluginCtx->window = mainWindow_;
    pluginCtx->registry = registry_;
    pluginCtx->hookBus = hookBus_;
    pluginCtx->snippet = snippet_;
    registerPluginNatives(env_, pluginCtx);

    // Set the module base dir to ~/.trowel/plugins/ so (import ...) resolves
    // plugin-to-plugin.
    const QString pluginsDir =
        QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
        + "/.trowel/plugins";
    turi_env_set_module_base_dir(env_, pluginsDir.toUtf8().constData());

    // Scan plugin directories.
    QStringList searchDirs;
    // 1. Bundled plugins (read-only).
    //    macOS: Trowel.app/Contents/Resources/plugins/
    //    Linux: <dir-of-binary>/plugins/
    // For now, only user plugins are scanned (bundled plugins are a future
    // packaging step).
    // 2. User plugins.
    searchDirs << pluginsDir;

    for (const auto& dir : searchDirs) {
        QDir d(dir);
        if (!d.exists()) continue;
        const auto entries = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const auto& name : entries) {
            const QString pluginPath = d.absoluteFilePath(name + "/plugin.tur");
            if (!QFile::exists(pluginPath)) continue;

            QFile f(pluginPath);
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                qWarning("[PluginHost] cannot read %s", qPrintable(pluginPath));
                continue;
            }
            const QByteArray source = f.readAll();
            f.close();

            TuriValue result = turi_eval_with_path(
                env_, source.constData(), pluginPath.toUtf8().constData());

            if (turi_is_error(result)) {
                const char* msg = turi_error_message(result);
                qWarning("[PluginHost] plugin %s failed: %s",
                         qPrintable(name), msg ? msg : "(unknown)");
                if (mainWindow_) {
                    mainWindow_->statusBar()->show();
                    mainWindow_->statusBar()->showMessage(
                        QStringLiteral("Plugin %1 failed: %2").arg(
                            name, QString::fromUtf8(msg ? msg : "(unknown)")),
                        5000);
                }
                continue;
            }

            loadedPlugins_ << name;
            qDebug("[PluginHost] loaded plugin: %s", qPrintable(name));
        }
    }
}

bool PluginHost::reloadPlugin(const QString& name)
{
    if (!env_) return false;

    // turi_env_reset clears user definitions but keeps registered natives.
    // This is the development loop: reset, then re-eval the plugin file.
    turi_env_reset(env_);

    // Re-eval ALL plugins (reset clears everything, not just one).
    // A per-plugin reset would need per-plugin envs (§4.4, future).
    loadedPlugins_.clear();
    loadAll();
    return loadedPlugins_.contains(name);
}

void PluginHost::loadSyntaxPlugins()
{
    if (!env_) return;

    // Create the syntax registry if it doesn't exist yet.
    if (!syntaxRegistry_) {
        syntaxRegistry_ = new SyntaxRegistry();
        // Make it available to the lexer adapter.
        SetSyntaxRegistry(syntaxRegistry_);
    }

    // Register the syntax-descriptor natives so syntax.tur can call them.
    // These are registered on the shared env alongside the plugin natives.
    registerSyntaxNatives(env_, syntaxRegistry_);

    // Scan ~/.trowel/syntax/<name>/syntax.tur
    const QString syntaxDir =
        QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
        + "/.trowel/syntax";
    QDir d(syntaxDir);
    if (!d.exists()) return;

    const auto entries = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto& name : entries) {
        const QString path = d.absoluteFilePath(name + "/syntax.tur");
        if (!QFile::exists(path)) continue;

        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        const QByteArray source = f.readAll();
        f.close();

        TuriValue result = turi_eval_with_path(
            env_, source.constData(), path.toUtf8().constData());

        if (turi_is_error(result)) {
            const char* msg = turi_error_message(result);
            qWarning("[PluginHost] syntax plugin %s failed: %s",
                     qPrintable(name), msg ? msg : "(unknown)");
            continue;
        }

        qDebug("[PluginHost] loaded syntax plugin: %s", qPrintable(name));
    }
}

void PluginHost::loadKeymap()
{
    if (!env_) return;

    // Load ~/.trowel/keymap.tur if it exists.
    const QString keymapPath =
        QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
        + "/.trowel/keymap.tur";
    if (!QFile::exists(keymapPath)) return;

    QFile f(keymapPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    const QByteArray source = f.readAll();
    f.close();

    TuriValue result = turi_eval_with_path(
        env_, source.constData(), keymapPath.toUtf8().constData());

    if (turi_is_error(result)) {
        const char* msg = turi_error_message(result);
        qWarning("[PluginHost] keymap failed: %s",
                 msg ? msg : "(unknown)");
        if (mainWindow_) {
            mainWindow_->statusBar()->showMessage(
                QStringLiteral("Keymap error: %1").arg(
                    QString::fromUtf8(msg ? msg : "(unknown)")), 5000);
        }
        return;
    }

    qDebug("[PluginHost] loaded keymap");
}

void PluginHost::startEventLoopPump()
{
    if (!env_ || eventLoopTimer_) return;

    // Pump the Turmeric async scheduler every ~16ms.  This drains ready
    // fibers and timers without blocking, so the Qt UI never stalls.
    // The scheduler is cooperative and single-threaded within the TuriEnv,
    // so "background" means "interleaved with the Qt event loop," not "on
    // another OS thread" (§4.5).
    eventLoopTimer_ = new QTimer(this);
    QObject::connect(eventLoopTimer_, &QTimer::timeout, this, [this] {
        if (env_) turi_run_event_loop(env_);
    });
    eventLoopTimer_->start(16);
}

}  // namespace trowel
