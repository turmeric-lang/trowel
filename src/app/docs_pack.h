#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace trowel {

// One article of the Turmeric docs pack: a guide or an API module page.
struct DocsEntry {
    enum class Section { Guide, Api };

    Section section = Section::Guide;
    QString slug;         // "quickstart", "arc"
    QString path;         // pack-relative, "guides/quickstart.html"
    QString title;
    QString category;     // guides: "Getting Started"; api: "core", "r7rs", ...
    QString description;
    QStringList symbols;  // api only
    QString added;        // ISO date, "2026-08-20"
    QString words;        // pre-lowercased search blob from index.json
};

// The `turmeric-docs-<tag>.tar.gz` release artifact, unpacked: `index.json`
// plus chrome-free article bodies under `guides/` and `api/`. The pack is the
// contract -- the website and Try Turmeric render the same files -- so this
// class reads it and nothing else.
class DocsPack {
public:
    // Where the pack was staged next to the bundled toolchain, or an
    // empty string when this build has none. `TROWEL_DOCS_DIR` overrides.
    static QString locate();

    // Load `<dir>/index.json`. False (and error()) when it is missing or
    // is not a pack index.
    bool load(const QString& dir);
    bool isLoaded() const { return !entries_.isEmpty(); }
    QString error() const { return error_; }

    QString version() const { return version_; }
    const QVector<DocsEntry>& entries() const { return entries_; }
    const DocsEntry* find(const QString& pathOrSlug) const;

    // Entries matching every whitespace-separated term of `query`, best first.
    // Titles and API symbols outrank a hit somewhere in the body text.
    QVector<const DocsEntry*> search(const QString& query, int limit = 200) const;

    // Newest `count` entries by `added`, newest first.
    QVector<const DocsEntry*> recentlyAdded(int count) const;

    // Distinct categories of a section, in first-seen order.
    QStringList categories(DocsEntry::Section section) const;

    // The article body: an HTML fragment. Empty when it can't be read.
    QString articleHtml(const DocsEntry& entry) const;

    // Resolve a link found in article `fromPath` ("guides/a.html") to a
    // pack-relative path ("api/arc.html"), leaving any "#fragment" off.
    // Empty when the target leaves the pack.
    static QString resolveLink(const QString& fromPath, const QString& href);

private:
    QString dir_;
    QString version_;
    QString error_;
    QVector<DocsEntry> entries_;
};

}
