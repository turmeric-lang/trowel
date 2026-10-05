#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

namespace trowel {

// Per-file state that Trowel remembers automatically as you work.
//
// This is *not* settings (which the user edits by hand in settings.json).
// It is machine-written state the user never edits, stored in a separate
// document-state.json.  See the phase 6 plan for the rationale.
//
// Stored per canonical absolute path.  Keyed by QFileInfo::canonicalFilePath.

struct DocState {
    // Caret and anchor as {line, column} — line/column survive small
    // external edits better than byte offsets, and are clamped on restore.
    int caretLine = 0;
    int caretColumn = 0;
    int anchorLine = 0;
    int anchorColumn = 0;

    // The document line at the top of the view, plus the horizontal offset.
    // A document line stays stable if wrap or the font changes; a display
    // line does not.
    int topLine = 0;
    int xOffset = 0;

    // Collapsed fold header lines.
    QVector<int> foldedHeaders;

    // Per-buffer wrap override: "on", "off", or empty for "use the default".
    // Stored as an int: 0 = default, 1 = on, 2 = off.
    int wrapOverride = 0;

    // File size and mtime at the time of saving, so restore can detect
    // that the file changed outside Trowel.
    qint64 fileSize = 0;
    qint64 fileMtime = 0;

    // When this entry was last saved, for pruning old entries.
    qint64 lastSeen = 0;
};

// Owns the document-state.json file.  Loads lazily on first use and saves
// with a 2-second debounce.  Shared by every window in the one running
// instance (single-instance is enforced), so no locking is needed.
//
// The store keeps at most 500 entries.  When saving, it drops the entries
// with the oldest lastSeen.
class DocumentStateStore : public QObject {
    Q_OBJECT
public:
    static DocumentStateStore& instance();

    // Look up saved state for `path` (canonical absolute path).
    // Returns nullopt when nothing is saved.
    std::optional<DocState> lookup(const QString& path);

    // Save state for `path`.  Debounced: the actual write happens 2s later
    // unless flush() is called first.
    void remember(const QString& path, const DocState& state);

    // Remove the entry for `path` (e.g. when the file is deleted).
    void forget(const QString& path);

    // Remove every entry.  Used by File > Open Recent > Clear Menu.
    void clear();

    // Force any pending write to happen now.  Called on quit.
    void flush();

    // The path to document-state.json.  Exposed so the control API can
    // report it to tests running in a sandboxed HOME.
    QString filePath() const;

private:
    DocumentStateStore();
    ~DocumentStateStore() override;
    DocumentStateStore(const DocumentStateStore&) = delete;
    DocumentStateStore& operator=(const DocumentStateStore&) = delete;

    void load();
    void saveNow();

    struct Impl;
    Impl* impl_;
};

}  // namespace trowel
