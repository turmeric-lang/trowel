#include "app/document_state.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>

#include <limits>

namespace trowel {

struct DocumentStateStore::Impl {
    // path -> DocState, loaded lazily.
    bool loaded = false;
    QHash<QString, DocState> entries;
    QTimer* saveTimer = nullptr;
    bool dirty = false;

    Impl() {
        saveTimer = new QTimer();
        saveTimer->setSingleShot(true);
        saveTimer->setInterval(2000);
        // The connection is wired in the constructor body of the store,
        // because the lambda captures `this` (the store).
    }
};

DocumentStateStore& DocumentStateStore::instance() {
    static DocumentStateStore s;
    return s;
}

DocumentStateStore::DocumentStateStore()
    : impl_(new Impl) {
    connect(impl_->saveTimer, &QTimer::timeout, this, [this] {
        if (impl_->dirty) saveNow();
    });
}

DocumentStateStore::~DocumentStateStore() {
    // Flush on destruction so a quit does not lose the last write.
    if (impl_->dirty) saveNow();
    delete impl_;
}

QString DocumentStateStore::filePath() const {
    // Resolve the data directory the same way Settings resolves the config
    // directory: env var first, then XDG, then QStandardPaths.  On macOS,
    // QStandardPaths::AppDataLocation goes through NSSearchPathForDirectoriesInDomains
    // which does NOT respect $HOME, so tests with a sandboxed HOME would
    // write to the real home.  Falling back to QDir::homePath() fixes that.
    if (const QString env = qEnvironmentVariable("TROWEL_DATA_DIR");
        !env.isEmpty()) {
        QDir().mkpath(env);
        return env + QStringLiteral("/document-state.json");
    }
    if (const QString xdg = qEnvironmentVariable("XDG_DATA_HOME");
        !xdg.isEmpty()) {
        const QString dir = xdg + QStringLiteral("/trowel");
        QDir().mkpath(dir);
        return dir + QStringLiteral("/document-state.json");
    }
    // On macOS, QStandardPaths::AppDataLocation does not respect $HOME.
    // Construct the path manually so tests with a sandboxed HOME work.
    const QString home = QDir::homePath();
    const QString dir = home + QStringLiteral("/Library/Application Support/trowel");
    QDir().mkpath(dir);
    return dir + QStringLiteral("/document-state.json");
}

void DocumentStateStore::load() {
    if (impl_->loaded) return;
    impl_->loaded = true;

    QFile f(filePath());
    if (!f.open(QIODevice::ReadOnly)) return;

    const QByteArray data = f.readAll();
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return;

    const QJsonObject root = doc.object();
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (!it.value().isObject()) continue;
        const QJsonObject o = it.value().toObject();
        DocState s;
        s.caretLine = o.value("caretLine").toInt(0);
        s.caretColumn = o.value("caretColumn").toInt(0);
        s.anchorLine = o.value("anchorLine").toInt(0);
        s.anchorColumn = o.value("anchorColumn").toInt(0);
        s.topLine = o.value("topLine").toInt(0);
        s.xOffset = o.value("xOffset").toInt(0);
        s.wrapOverride = o.value("wrapOverride").toInt(0);
        s.fileSize = static_cast<qint64>(o.value("fileSize").toVariant().toLongLong());
        s.fileMtime = static_cast<qint64>(o.value("fileMtime").toVariant().toLongLong());
        s.lastSeen = static_cast<qint64>(o.value("lastSeen").toVariant().toLongLong());
        for (const QJsonValue& v : o.value("foldedHeaders").toArray()) {
            s.foldedHeaders.append(v.toInt());
        }
        impl_->entries.insert(it.key(), s);
    }
}

void DocumentStateStore::saveNow() {
    impl_->dirty = false;
    if (impl_->saveTimer->isActive()) impl_->saveTimer->stop();

    // Prune to 500 entries, dropping the oldest by lastSeen.
    while (impl_->entries.size() > 500) {
        QString oldestKey;
        qint64 oldestTime = std::numeric_limits<qint64>::max();
        for (auto it = impl_->entries.begin(); it != impl_->entries.end(); ++it) {
            if (it.value().lastSeen < oldestTime) {
                oldestTime = it.value().lastSeen;
                oldestKey = it.key();
            }
        }
        if (oldestKey.isEmpty()) break;
        impl_->entries.remove(oldestKey);
    }

    QJsonObject root;
    for (auto it = impl_->entries.begin(); it != impl_->entries.end(); ++it) {
        const DocState& s = it.value();
        QJsonObject o;
        o["caretLine"] = s.caretLine;
        o["caretColumn"] = s.caretColumn;
        o["anchorLine"] = s.anchorLine;
        o["anchorColumn"] = s.anchorColumn;
        o["topLine"] = s.topLine;
        o["xOffset"] = s.xOffset;
        o["wrapOverride"] = s.wrapOverride;
        o["fileSize"] = static_cast<double>(s.fileSize);
        o["fileMtime"] = static_cast<double>(s.fileMtime);
        o["lastSeen"] = static_cast<double>(s.lastSeen);
        QJsonArray folds;
        for (int h : s.foldedHeaders) folds.append(h);
        o["foldedHeaders"] = folds;
        root[it.key()] = o;
    }

    QSaveFile f(filePath());
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    f.commit();
}

std::optional<DocState> DocumentStateStore::lookup(const QString& path) {
    load();
    const QString key = QFileInfo(path).canonicalFilePath();
    if (key.isEmpty()) return std::nullopt;
    auto it = impl_->entries.find(key);
    if (it == impl_->entries.end()) return std::nullopt;
    return it.value();
}

void DocumentStateStore::remember(const QString& path, const DocState& state) {
    load();
    const QString key = QFileInfo(path).canonicalFilePath();
    if (key.isEmpty()) return;
    impl_->entries[key] = state;
    impl_->entries[key].lastSeen = QDateTime::currentSecsSinceEpoch();
    impl_->dirty = true;
    if (!impl_->saveTimer->isActive()) impl_->saveTimer->start();
}

void DocumentStateStore::forget(const QString& path) {
    load();
    const QString key = QFileInfo(path).canonicalFilePath();
    if (key.isEmpty()) return;
    impl_->entries.remove(key);
    impl_->dirty = true;
    if (!impl_->saveTimer->isActive()) impl_->saveTimer->start();
}

void DocumentStateStore::clear() {
    load();
    impl_->entries.clear();
    impl_->dirty = true;
    saveNow();  // immediate, so the file is empty before the test checks
}

void DocumentStateStore::flush() {
    if (impl_->dirty) saveNow();
}

}  // namespace trowel
