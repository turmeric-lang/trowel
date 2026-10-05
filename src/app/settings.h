#pragma once

#include <QObject>
#include <QStringList>
#include <QFont>
#include <QString>

namespace trowel {

// Trowel's hand-editable settings, stored as strict JSON in settings.json.
//
// Settings are choices the user makes on purpose and edits by hand.  Per-file
// state (caret, scroll, folds) is a different thing and lives elsewhere.
//
// What stays in QSettings is everything that is *state*, not a *setting*:
// window geometry, the splitter, the session, recentFiles, lastOpenDir, and
// the zoom level.  None of that is meant to be edited by hand.
class Settings : public QObject {
    Q_OBJECT
public:
    static Settings& instance();

    // The path to settings.json.
    QString path() const;

    // Create the directory and the file (as `{}`) if they do not exist.
    // Returns the path.  Safe to call when the file already exists.
    void ensureFileExists();

    // Force an immediate reload (bypassing the debounce).  Called after
    // saving settings.json inside Trowel so changes apply without delay.
    void reloadNow();

    // --- Typed getters ---
    // Each returns the default when the key is missing or has the wrong type.

    // Path to the `tur` binary, or empty for auto-detect.
    QString turmericPath() const;

    // LSP settings.
    bool lspEnabled() const;
    QString lspServerPath() const;

    // Editor font.
    QFont editorFont() const;

    // Rainbow brackets and bracket pair guides.
    bool rainbowBrackets() const;
    bool bracketPairGuides() const;

    // Word wrap defaults (phase 4).
    bool wrapProse() const;
    bool wrapCode() const;

    // Remember per-document state (phase 6).
    bool rememberDocumentState() const;

signals:
    // Emitted after a reload with only the keys whose effective value changed.
    void changed(const QStringList& keys);

private:
    Settings();
    ~Settings() override;
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;

    void load();
    void reload();
    void migrateFromQSettings();
    void emitChanged(const QStringList& oldKeys);

    struct Impl;
    Impl* impl_;
};

}  // namespace trowel
