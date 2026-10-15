#include "plugin_host.h"

#include "app/main_window.h"
#include "app/settings.h"
#include "command_registry.h"
#include "editor/editor_view.h"
#include "editor/lexers.h"
#include "hook_bus.h"
#include "plugin_natives.h"
#include "snippet_session.h"
#include "syntax_descriptor.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSet>
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
    const QStringList names = plugins_.keys();
    for (const auto& name : names) unloadPlugin(name);
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

TuriValue PluginHost::callClosure(TuriEnv* env, TuriValue closure,
                                  const QVector<TuriValue>& args)
{
    if (!env) return turi_error("no env");
    if (closure.tag != TURI_CLOSURE) return turi_error("not a closure");

    // turi_call takes a raw pointer + count.  Make a non-const copy since
    // QVector::data() on a const vector returns a const pointer.
    if (args.isEmpty()) {
        return turi_call(env, closure, nullptr, 0);
    }
    QVector<TuriValue> argCopy = args;
    return turi_call(env, closure, argCopy.data(),
                     static_cast<uint32_t>(argCopy.size()));
}

void PluginHost::setupNatives()
{
    if (!env_ || pluginCtx_) return;  // already set up

    // The context is heap-allocated and owned by the host (lives as long as
    // the env, which is freed in ~PluginHost).
    pluginCtx_ = new PluginContext;
    pluginCtx_->host = this;
    pluginCtx_->window = mainWindow_;
    pluginCtx_->registry = registry_;
    pluginCtx_->hookBus = hookBus_;
    pluginCtx_->snippet = snippet_;
    pluginCtx_->bookmarks = new QHash<QString, QSet<int>>;
    // Load persisted bookmarks from ~/.trowel/bookmarks.tur.
    {
        QFile f(QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
                + "/.trowel/bookmarks.tur");
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            while (!f.atEnd()) {
                const QByteArray line = f.readLine().trimmed();
                if (line.isEmpty()) continue;
                const int colon = line.lastIndexOf(':');
                if (colon <= 0) continue;
                const QString path = QString::fromUtf8(line.left(colon));
                const int lineNum = line.mid(colon + 1).toInt();
                if (lineNum > 0) (*pluginCtx_->bookmarks)[path].insert(lineNum);
            }
            f.close();
        }
    }
    registerPluginNatives(env_, pluginCtx_);
}

static QString trowelHome()
{
    return QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
           + "/.trowel";
}

TuriCaps PluginHost::grantedCaps(const QString& name) const
{
    auto it = plugins_.constFind(name);
    return it == plugins_.constEnd() ? TURI_CAP_NONE : it->granted;
}

PluginManifest PluginHost::parseManifest(const QString& manifestPath)
{
    PluginManifest m;
    QFile f(manifestPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return m;
    const QByteArray raw = f.readAll();
    f.close();

    m.present = true;
    m.hash = QCryptographicHash::hash(raw, QCryptographicHash::Sha256).toHex();

    // Drop `;` comments, then pull out the two fields we understand.
    QString text = QString::fromUtf8(raw);
    text.remove(QRegularExpression(QStringLiteral(";[^\\n]*")));

    static const QRegularExpression capsRe(
        QStringLiteral(":capabilities\\s*\\[([^\\]]*)\\]"));
    static const QRegularExpression descRe(
        QStringLiteral(":description\\s*\"((?:[^\"\\\\]|\\\\.)*)\""));

    const auto desc = descRe.match(text);
    if (desc.hasMatch()) m.description = desc.captured(1);

    static const QHash<QString, TuriCaps> kCaps = {
        {QStringLiteral("fs:read"), TURI_CAP_FS},
        {QStringLiteral("fs:write"), TURI_CAP_FS},
        {QStringLiteral("io"), TURI_CAP_IO},
        {QStringLiteral("proc"), TURI_CAP_PROC},
        {QStringLiteral("ffi"), TURI_CAP_FFI},
        {QStringLiteral("async"), TURI_CAP_ASYNC},
        {QStringLiteral("import"), TURI_CAP_IMPORT},
        {QStringLiteral("import:plugins"), TURI_CAP_IMPORT},
        {QStringLiteral("env"), TURI_CAP_ENV},
    };
    const auto caps = capsRe.match(text);
    if (caps.hasMatch()) {
        const QStringList tokens = caps.captured(1).split(
            QRegularExpression(QStringLiteral("[\\s,]+")), Qt::SkipEmptyParts);
        for (const auto& tok : tokens) {
            auto it = kCaps.constFind(tok);
            if (it == kCaps.constEnd()) {
                m.unknownCapabilities << tok;
                continue;
            }
            if (!m.capabilityNames.contains(tok)) m.capabilityNames << tok;
            m.caps |= *it;
            if (tok == "import:plugins") m.importAllPlugins = true;
        }
    }
    return m;
}

bool PluginHost::capabilitiesAllowed(const QString& name,
                                     const PluginManifest& manifest)
{
    const QString cachePath = trowelHome() + "/plugin-permissions.json";
    QJsonObject root;
    {
        QFile f(cachePath);
        if (f.open(QIODevice::ReadOnly))
            root = QJsonDocument::fromJson(f.readAll()).object();
    }

    // A cached answer holds only while the manifest is byte-identical.
    const QJsonObject cached = root.value(name).toObject();
    if (cached.value("hash").toString().toUtf8() == manifest.hash
        && cached.contains("granted")) {
        return cached.value("granted").toBool();
    }

    // TROWEL_PLUGIN_PERMISSIONS=allow|deny answers without a dialog, for
    // headless runs (the smoke tests) where nothing can click a button.
    bool allowed = false;
    const QByteArray forced = qgetenv("TROWEL_PLUGIN_PERMISSIONS");
    if (forced == "allow") {
        allowed = true;
    } else if (forced == "deny") {
        allowed = false;
    } else {
        QStringList what;
        for (const auto& c : manifest.capabilityNames) {
            if (c == "fs:read") what << tr("read files");
            else if (c == "fs:write") what << tr("write files");
            else if (c == "io") what << tr("use the network");
            else if (c == "proc") what << tr("spawn processes");
            else if (c == "ffi") what << tr("call native libraries (FFI)");
            else if (c == "async") what << tr("run background tasks");
            else if (c == "import") what << tr("import its own modules");
            else if (c == "import:plugins") what << tr("import modules from other plugins");
            else if (c == "env") what << tr("read environment variables");
        }
        QString text = tr("The plugin \"%1\" wants to: %2.").arg(name, what.join(", "));
        if (!manifest.description.isEmpty())
            text += "\n\n" + manifest.description;
        text += "\n\n" + tr("Allow?");
        allowed = QMessageBox::question(
                      mainWindow_, tr("Plugin permissions"), text,
                      QMessageBox::Yes | QMessageBox::No,
                      QMessageBox::No) == QMessageBox::Yes;
    }

    // Persist the answer (also a denial, so the user is not nagged).  Forced
    // answers are not cached: they are an override, not a user decision.
    if (forced != "allow" && forced != "deny") {
        QJsonObject entry;
        entry["hash"] = QString::fromUtf8(manifest.hash);
        entry["granted"] = allowed;
        root[name] = entry;
        QDir().mkpath(trowelHome());
        QFile out(cachePath);
        if (out.open(QIODevice::WriteOnly | QIODevice::Truncate))
            out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    }
    return allowed;
}

bool PluginHost::loadPlugin(const QString& name, const PluginSource& src)
{
    QFile f(src.path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning("[PluginHost] cannot read %s", qPrintable(src.path));
        return false;
    }
    const QByteArray source = f.readAll();
    f.close();

    const QString pluginDir = QFileInfo(src.path).path();
    PluginManifest manifest = parseManifest(pluginDir + "/manifest.tur");
    for (const auto& bad : manifest.unknownCapabilities)
        qWarning("[PluginHost] plugin %s: unknown capability '%s' ignored",
                 qPrintable(name), qPrintable(bad));

    // Bundled plugins ship with Trowel and are trusted; user plugins that
    // ask for anything beyond TURI_CAP_NONE need the user's consent.
    TuriCaps granted = TURI_CAP_NONE;
    if (manifest.caps != TURI_CAP_NONE) {
        if (src.bundled || capabilitiesAllowed(name, manifest)) {
            granted = manifest.caps;
        } else {
            qWarning("[PluginHost] plugin %s: capabilities denied, loading "
                     "without them", qPrintable(name));
            if (mainWindow_) {
                mainWindow_->statusBar()->showMessage(
                    tr("Plugin %1 runs without its requested permissions").arg(name),
                    5000);
            }
        }
    }

    TuriEnv* env = turi_env_new_sandboxed();
    if (!env) return false;
    // The sandbox env has no trowel-* natives of its own; re-register the
    // host API into it (the shared-spice-image route is not exposed by the
    // pinned libturi).  The context is shared: the natives are the host's API.
    registerPluginNatives(env, pluginCtx_);
    if (granted != TURI_CAP_NONE) turi_env_allow(env, granted);

    // libturi refuses (import ...) outright without TURI_CAP_IMPORT.  With it,
    // imports resolve inside the plugin's own directory -- or, for
    // `import:plugins`, from the whole plugins directory so a plugin can
    // import another's modules.  Bundled plugins live in the Qt resource
    // system, which has no filesystem path.
    if (!src.bundled) {
        const QString base = (granted & TURI_CAP_IMPORT) && manifest.importAllPlugins
                                 ? QFileInfo(pluginDir).path()
                                 : pluginDir;
        turi_env_set_module_base_dir(env, base.toUtf8().constData());
    }

    PluginRecord rec;
    rec.name = name;
    rec.env = env;
    rec.manifest = manifest;
    rec.granted = granted;
    plugins_.insert(name, rec);

    TuriValue result = turi_eval_with_path(
        env, source.constData(), src.path.toUtf8().constData());

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
        unloadPlugin(name);
        return false;
    }

    qDebug("[PluginHost] loaded plugin: %s", qPrintable(name));
    return true;
}

void PluginHost::unloadPlugin(const QString& name)
{
    auto it = plugins_.find(name);
    if (it == plugins_.end()) return;
    TuriEnv* env = it->env;
    plugins_.erase(it);

    // Take back everything the plugin registered before its env (and the
    // closures inside it) disappears.
    if (hookBus_) hookBus_->unsubscribeEnv(env);
    if (pluginCtx_) {
        const PluginContributions c = pluginCtx_->contributions.take(env);
        if (registry_) {
            for (const auto& id : c.commandIds) registry_->remove(id);
        }
        for (const auto& action : c.actions) {
            if (!action) continue;
            if (mainWindow_) mainWindow_->removeSideBarAction(action);
            delete action;
        }
    }
    turi_env_free(env);
}

void PluginHost::loadAll()
{
    if (!env_) return;

    // Natives must be registered before loading plugins so plugin.tur can
    // call them.  Also needed before loadKeymap() for trowel-set-keybinding.
    setupNatives();

    const QString pluginsDir = trowelHome() + "/plugins";

    // Scan plugin directories.  Bundled plugins are scanned first, then user
    // plugins, so a user plugin with the same name as a bundled one overrides
    // it entirely — only one copy is loaded, preventing duplicate button
    // registrations while still letting user plugins replace built-in commands.
    struct Dir { QString path; bool bundled; };
    const Dir searchDirs[] = {
        {QStringLiteral(":/plugins"), true},  // bundled (Qt resource system)
        {pluginsDir, false},                  // user (~/.trowel/plugins)
    };

    discovered_.clear();
    for (const auto& dir : searchDirs) {
        QDir d(dir.path);
        if (!d.exists()) continue;
        const auto entries = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const auto& name : entries) {
            const QString pluginPath = d.absoluteFilePath(name + "/plugin.tur");
            if (QFile::exists(pluginPath))
                discovered_.insert(name, {pluginPath, dir.bundled});
        }
    }

    // Plugins the user has disabled in settings.json.  All plugins are
    // enabled by default; listing a name in "plugins.disabled" skips it.
    const QSet<QString> disabled = Settings::instance().disabledPlugins();

    for (auto it = discovered_.constBegin(); it != discovered_.constEnd(); ++it) {
        if (plugins_.contains(it.key())) continue;
        if (disabled.contains(it.key())) {
            qDebug("[PluginHost] plugin %s disabled by settings, skipping",
                   qPrintable(it.key()));
            continue;
        }
        loadPlugin(it.key(), it.value());
    }
}

bool PluginHost::reloadPlugin(const QString& name)
{
    auto it = discovered_.constFind(name);
    if (it == discovered_.constEnd()) return false;
    if (Settings::instance().disabledPlugins().contains(name)) return false;

    // Only this plugin's env goes: the others keep their state.
    unloadPlugin(name);
    return loadPlugin(name, *it);
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

    // Load the bundled keymap from Qt resources first.  This sets the
    // default shortcuts as data-driven keymap.tur, so users can see and
    // override them.  The bundled file is platform-specific.
#ifdef Q_OS_MACOS
    const QString bundledPath = QStringLiteral(":/keymap/keymap-mac.tur");
#else
    const QString bundledPath = QStringLiteral(":/keymap/keymap-other.tur");
#endif
    QFile bf(bundledPath);
    if (bf.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QByteArray bsource = bf.readAll();
        bf.close();
        TuriValue bresult = turi_eval_with_path(
            env_, bsource.constData(), bundledPath.toUtf8().constData());
        if (turi_is_error(bresult)) {
            const char* msg = turi_error_message(bresult);
            qWarning("[PluginHost] bundled keymap failed: %s",
                     msg ? msg : "(unknown)");
        } else {
            qDebug("[PluginHost] loaded bundled keymap");
        }
    }

    // Load ~/.trowel/keymap.tur if it exists (user overrides).
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

    qDebug("[PluginHost] loaded user keymap");
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
        // Copy: a task could (via reload) change plugins_ under us.
        QVector<TuriEnv*> envs;
        for (const auto& p : std::as_const(plugins_)) envs << p.env;
        for (TuriEnv* e : envs) turi_run_event_loop(e);
    });
    eventLoopTimer_->start(16);
}

}  // namespace trowel
