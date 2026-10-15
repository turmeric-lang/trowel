#include "app/docs_pack.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace trowel {

namespace {

DocsEntry ParseEntry(const QJsonObject& o, DocsEntry::Section section) {
    DocsEntry e;
    e.section = section;
    e.slug = o["slug"].toString();
    e.path = o["path"].toString();
    e.title = o["title"].toString();
    e.category = o["category"].toString();
    e.description = o["description"].toString();
    e.added = o["added"].toString();
    e.words = o["words"].toString();
    for (const QJsonValue& s : o["symbols"].toArray()) e.symbols << s.toString();
    return e;
}

// An index entry that points outside the pack would let a crafted index.json
// read arbitrary files; refuse it rather than sanitise.
bool PathIsInsidePack(const QString& path) {
    if (path.isEmpty() || path.startsWith('/')) return false;
    return !QDir::cleanPath(path).startsWith("..");
}

}

QString DocsPack::locate() {
    const QString env = qEnvironmentVariable("TROWEL_DOCS_DIR");
    if (!env.isEmpty()) return QFileInfo(env + "/index.json").isFile() ? env : QString();

    const QString appDir = QCoreApplication::applicationDirPath();
#ifdef Q_OS_MACOS
    // <App>.app/Contents/MacOS/trowel -> <App>.app/Contents/Resources/turmeric-docs
    const QString dir = QDir::cleanPath(appDir + "/../Resources/turmeric-docs");
#else
    const QString dir = QDir::cleanPath(appDir + "/turmeric-docs");
#endif
    return QFileInfo(dir + "/index.json").isFile() ? dir : QString();
}

bool DocsPack::load(const QString& dir) {
    entries_.clear();
    version_.clear();
    error_.clear();
    dir_ = dir;

    QFile f(dir + "/index.json");
    if (!f.open(QIODevice::ReadOnly)) {
        error_ = "no index.json in " + dir;
        return false;
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (!doc.isObject()) {
        error_ = "index.json is not valid JSON: " + pe.errorString();
        return false;
    }
    const QJsonObject root = doc.object();
    if (!root["guides"].isArray() || !root["api"].isArray()) {
        error_ = "index.json has no guides/api lists";
        return false;
    }
    version_ = root["version"].toString();
    for (const QJsonValue& v : root["guides"].toArray()) {
        DocsEntry e = ParseEntry(v.toObject(), DocsEntry::Section::Guide);
        if (!e.slug.isEmpty() && PathIsInsidePack(e.path)) entries_.append(e);
    }
    for (const QJsonValue& v : root["api"].toArray()) {
        DocsEntry e = ParseEntry(v.toObject(), DocsEntry::Section::Api);
        if (!e.slug.isEmpty() && PathIsInsidePack(e.path)) entries_.append(e);
    }
    if (entries_.isEmpty()) error_ = "index.json lists no articles";
    return !entries_.isEmpty();
}

const DocsEntry* DocsPack::find(const QString& pathOrSlug) const {
    for (const DocsEntry& e : entries_) if (e.path == pathOrSlug) return &e;
    // A bare slug names a guide first: "arc" is also a guide-less api module,
    // but "quickstart" must not be shadowed by an api page of the same name.
    for (const DocsEntry& e : entries_)
        if (e.section == DocsEntry::Section::Guide && e.slug == pathOrSlug) return &e;
    for (const DocsEntry& e : entries_) if (e.slug == pathOrSlug) return &e;
    return nullptr;
}

QVector<const DocsEntry*> DocsPack::search(const QString& query, int limit) const {
    const QStringList terms = query.toLower().simplified().split(' ', Qt::SkipEmptyParts);
    QVector<const DocsEntry*> out;
    if (terms.isEmpty()) return out;

    struct Hit { const DocsEntry* e; int score; };
    QVector<Hit> hits;
    for (const DocsEntry& e : entries_) {
        const QString title = e.title.toLower();
        const QString slug = e.slug.toLower();
        int score = 0;
        bool all = true;
        for (const QString& t : terms) {
            int s = 0;
            if (title == t || slug == t) s = 40;
            else if (title.startsWith(t) || slug.startsWith(t)) s = 25;
            else if (title.contains(t) || slug.contains(t)) s = 15;
            for (const QString& sym : e.symbols) {
                const QString ls = sym.toLower();
                if (ls == t) s = std::max(s, 35);
                else if (ls.startsWith(t)) s = std::max(s, 12);
                else if (ls.contains(t)) s = std::max(s, 6);
            }
            if (s == 0 && e.description.contains(t, Qt::CaseInsensitive)) s = 4;
            if (s == 0 && e.words.contains(t)) s = 1;
            if (s == 0) { all = false; break; }
            score += s;
        }
        if (all) hits.append({&e, score});
    }
    std::stable_sort(hits.begin(), hits.end(),
                     [](const Hit& a, const Hit& b) { return a.score > b.score; });
    for (const Hit& h : hits) {
        if (out.size() >= limit) break;
        out.append(h.e);
    }
    return out;
}

QVector<const DocsEntry*> DocsPack::recentlyAdded(int count) const {
    QVector<const DocsEntry*> all;
    for (const DocsEntry& e : entries_) if (!e.added.isEmpty()) all.append(&e);
    // ISO dates sort lexically; stable so ties keep the index's own order.
    std::stable_sort(all.begin(), all.end(),
                     [](const DocsEntry* a, const DocsEntry* b) { return a->added > b->added; });
    if (all.size() > count) all.resize(count);
    return all;
}

QStringList DocsPack::categories(DocsEntry::Section section) const {
    QStringList out;
    for (const DocsEntry& e : entries_)
        if (e.section == section && !out.contains(e.category)) out << e.category;
    return out;
}

QString DocsPack::articleHtml(const DocsEntry& entry) const {
    if (!PathIsInsidePack(entry.path)) return {};
    QFile f(dir_ + "/" + entry.path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

QString DocsPack::resolveLink(const QString& fromPath, const QString& href) {
    QString h = href;
    const int hash = h.indexOf('#');
    if (hash >= 0) h.truncate(hash);
    const int q = h.indexOf('?');
    if (q >= 0) h.truncate(q);
    if (h.isEmpty()) return fromPath;  // "#section": the same article
    if (h.contains("://") || h.startsWith("mailto:")) return {};

    const QString base = fromPath.contains('/')
        ? fromPath.left(fromPath.lastIndexOf('/') + 1) : QString();
    const QString joined = h.startsWith('/') ? h.mid(1) : base + h;
    const QString clean = QDir::cleanPath(joined);
    if (clean.startsWith("..") || clean.startsWith('/')) return {};
    return clean;
}

}
