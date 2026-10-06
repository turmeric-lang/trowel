#include "app/settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFileSystemWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QVariant>

#include <cmath>

namespace trowel {

namespace {

// Resolve the config directory: $TROWEL_CONFIG_DIR if set, else the
// platform-standard location.  Mirrors where ~/.config/turmeric/ lives.
QString ConfigDir() {
    if (const QString env = qEnvironmentVariable("TROWEL_CONFIG_DIR");
        !env.isEmpty()) {
        return env;
    }
    if (const QString xdg = qEnvironmentVariable("XDG_CONFIG_HOME");
        !xdg.isEmpty()) {
        return xdg + "/trowel";
    }
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
           + "/trowel";
}

// The platform-default monospace font.
QFont DefaultEditorFont() {
    const QString preferred = QFontDatabase::hasFamily("Iosevka") ? "Iosevka" : "Menlo";
    return QFont(preferred, 12);
}

}  // namespace

struct Settings::Impl {
    QString configDir;
    QString filePath;
    QJsonObject values;
    QFileSystemWatcher* watcher = nullptr;
    QTimer* debounce = nullptr;
    bool loaded = false;
};

Settings& Settings::instance() {
    static Settings s;
    return s;
}

Settings::Settings()
    : impl_(new Impl)
{
    impl_->configDir = ConfigDir();
    impl_->filePath = impl_->configDir + "/settings.json";

    impl_->debounce = new QTimer(this);
    impl_->debounce->setSingleShot(true);
    connect(impl_->debounce, &QTimer::timeout, this, [this] { reload(); });

    impl_->watcher = new QFileSystemWatcher(this);
    connect(impl_->watcher, &QFileSystemWatcher::fileChanged,
            this, [this] { impl_->debounce->start(200); });
    connect(impl_->watcher, &QFileSystemWatcher::directoryChanged,
            this, [this] {
        // The file may have been replaced (atomic save), so re-add the watch.
        if (QFile::exists(impl_->filePath)) {
            impl_->watcher->addPath(impl_->filePath);
        }
        impl_->debounce->start(200);
    });

    load();
    migrateFromQSettings();

    // Create settings.json if it does not exist yet — a fresh install with no
    // QSettings values to migrate leaves no file behind, so the file the app
    // treats as its source of truth would be absent until the user opened
    // Settings. ensureFileExists() also watches the directory + file so
    // external edits trigger a reload.
    ensureFileExists();
}

Settings::~Settings() = default;

QString Settings::path() const {
    return impl_->filePath;
}

void Settings::ensureFileExists() {
    QDir().mkpath(impl_->configDir);
    if (!QFile::exists(impl_->filePath)) {
        QSaveFile f(impl_->filePath);
        if (f.open(QIODevice::WriteOnly)) {
            f.write("{}");
            f.commit();
        }
    }
    // Watch the directory and the file.
    if (!impl_->watcher->directories().contains(impl_->configDir))
        impl_->watcher->addPath(impl_->configDir);
    if (!impl_->watcher->files().contains(impl_->filePath))
        impl_->watcher->addPath(impl_->filePath);
}

void Settings::load() {
    QFile f(impl_->filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        impl_->values = {};
        impl_->loaded = true;
        return;
    }
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        // Keep last good values (empty on first load).
        impl_->loaded = true;
        return;
    }
    impl_->values = doc.object();
    impl_->loaded = true;
}

void Settings::migrateFromQSettings() {
    // Only migrate when settings.json does not exist yet.
    if (QFile::exists(impl_->filePath)) return;

    QSettings qs;
    QJsonObject migrated;

    // editorFont → editor.font.family / editor.font.size
    if (qs.contains("editorFont")) {
        const QFont f = qs.value("editorFont").value<QFont>();
        migrated["editor.font.family"] = f.family();
        migrated["editor.font.size"] = f.pointSize();
        qs.remove("editorFont");
    }
    // repl/turBinary → turmeric.path
    if (qs.contains("repl/turBinary")) {
        const QString v = qs.value("repl/turBinary").toString();
        if (!v.isEmpty()) migrated["turmeric.path"] = v;
        qs.remove("repl/turBinary");
    }
    // lsp/enabled → lsp.enabled
    if (qs.contains("lsp/enabled")) {
        migrated["lsp.enabled"] = qs.value("lsp/enabled").toBool();
        qs.remove("lsp/enabled");
    }
    // lsp/serverPath → lsp.serverPath
    if (qs.contains("lsp/serverPath")) {
        const QString v = qs.value("lsp/serverPath").toString();
        if (!v.isEmpty()) migrated["lsp.serverPath"] = v;
        qs.remove("lsp/serverPath");
    }
    // editor/rainbowBrackets → editor.rainbowBrackets
    if (qs.contains("editor/rainbowBrackets")) {
        migrated["editor.rainbowBrackets"] = qs.value("editor/rainbowBrackets").toBool();
        qs.remove("editor/rainbowBrackets");
    }
    // editor/bracketPairGuides → editor.bracketPairGuides
    if (qs.contains("editor/bracketPairGuides")) {
        migrated["editor.bracketPairGuides"] = qs.value("editor/bracketPairGuides").toBool();
        qs.remove("editor/bracketPairGuides");
    }

    if (migrated.isEmpty()) return;

    // Write the migrated settings to settings.json.
    QDir().mkpath(impl_->configDir);
    QSaveFile f(impl_->filePath);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(migrated).toJson(QJsonDocument::Indented));
        f.commit();
    }

    // Reload so the in-memory values reflect the migrated file.
    load();
}

void Settings::reloadNow() {
    reload();
}

void Settings::reload() {
    // Read the file and compute which keys changed.
    QFile f(impl_->filePath);
    if (!f.open(QIODevice::ReadOnly)) return;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return;

    const QJsonObject old = impl_->values;
    impl_->values = doc.object();

    // Collect changed keys.
    QStringList changedKeys;
    for (auto it = old.constBegin(); it != old.constEnd(); ++it) {
        if (impl_->values.value(it.key()) != it.value())
            changedKeys.append(it.key());
    }
    for (auto it = impl_->values.constBegin(); it != impl_->values.constEnd(); ++it) {
        if (!old.contains(it.key()))
            changedKeys.append(it.key());
    }
    if (!changedKeys.isEmpty())
        emit changed(changedKeys);
}

// --- Typed getters ---

QString Settings::turmericPath() const {
    const QJsonValue v = impl_->values.value("turmeric.path");
    return v.isString() ? v.toString() : QString();
}

bool Settings::lspEnabled() const {
    const QJsonValue v = impl_->values.value("lsp.enabled");
    return v.isBool() ? v.toBool() : true;
}

QString Settings::lspServerPath() const {
    const QJsonValue v = impl_->values.value("lsp.serverPath");
    return v.isString() ? v.toString() : QString();
}

QFont Settings::editorFont() const {
    const QJsonValue fam = impl_->values.value("editor.font.family");
    const QJsonValue sz = impl_->values.value("editor.font.size");
    QFont f = DefaultEditorFont();
    if (fam.isString()) f.setFamily(fam.toString());
    if (sz.isDouble()) f.setPointSize(static_cast<int>(std::round(sz.toDouble())));
    return f;
}

bool Settings::rainbowBrackets() const {
    const QJsonValue v = impl_->values.value("editor.rainbowBrackets");
    return v.isBool() ? v.toBool() : true;
}

bool Settings::bracketPairGuides() const {
    const QJsonValue v = impl_->values.value("editor.bracketPairGuides");
    return v.isBool() ? v.toBool() : true;
}

bool Settings::wrapProse() const {
    const QJsonValue v = impl_->values.value("editor.wrap.prose");
    return v.isBool() ? v.toBool() : true;
}

bool Settings::wrapCode() const {
    const QJsonValue v = impl_->values.value("editor.wrap.code");
    return v.isBool() ? v.toBool() : false;
}

bool Settings::rememberDocumentState() const {
    const QJsonValue v = impl_->values.value("editor.rememberDocumentState");
    return v.isBool() ? v.toBool() : true;
}

bool Settings::minimapEnabled() const {
    const QJsonValue v = impl_->values.value("editor.minimap");
    return v.isBool() ? v.toBool() : true;
}

void Settings::setMinimapEnabled(bool enabled) {
    // Update in-memory values immediately so callers that read right after
    // (the View menu toggle calls applyMinimapSettings synchronously) see
    // the new value without waiting for the file watcher's 200ms debounce.
    impl_->values["editor.minimap"] = enabled;
    // Write through to settings.json so the file watcher fires and every
    // window's onSettingsChanged applies the change uniformly.
    QDir().mkpath(impl_->configDir);
    QSaveFile f(impl_->filePath);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(impl_->values).toJson(QJsonDocument::Indented));
        f.commit();
    }
}

}  // namespace trowel
