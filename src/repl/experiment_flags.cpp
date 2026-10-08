#include "repl/experiment_flags.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

namespace trowel {

namespace {

// Scan a turmeric source string for `:enable [sym1 sym2 ...]` and return the
// symbol tokens. Handles `;` line comments and `#| ... |#` block comments
// (nestable). Returns empty when no `:enable` key is found, or when the shape
// is not the trivial `:enable [...]` form.
//
// This is NOT a turmeric parser. It is a quick scanner that works for the
// common case: a manifest or user config with a literal `:enable` list. If the
// value is computed (e.g. `(apply enable ...)`), the scanner returns empty and
// the caller falls through to the next source.
QStringList scanEnableList(const QString& text) {
    // Strip comments first so `:enable` inside a comment is not matched.
    QString stripped;
    stripped.reserve(text.size());
    bool inLineComment = false;
    int blockDepth = 0;
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        const QChar next = (i + 1 < text.size()) ? text.at(i + 1) : QChar();

        if (inLineComment) {
            if (c == '\n') inLineComment = false;
            continue;
        }
        if (blockDepth > 0) {
            if (c == '#' && next == '|') { ++blockDepth; ++i; continue; }
            if (c == '|' && next == '#') { --blockDepth; ++i; continue; }
            continue;
        }
        if (c == ';') { inLineComment = true; continue; }
        if (c == '#' && next == '|') { blockDepth = 1; ++i; continue; }
        stripped.append(c);
    }

    // Find `:enable` as a standalone token (not part of a longer symbol).
    int idx = -1;
    for (int i = 0; i < stripped.size(); ++i) {
        if (stripped.mid(i, 7) == QStringLiteral(":enable")) {
            // Check the character before is not a symbol-continuation char.
            if (i > 0) {
                const QChar prev = stripped.at(i - 1);
                if (prev.isLetterOrNumber() || prev == '-' || prev == '_')
                    continue;
            }
            // Check the character after is whitespace or `[`.
            const QChar after = (i + 7 < stripped.size()) ? stripped.at(i + 7) : QChar();
            if (after.isSpace() || after == '[') {
                idx = i + 7;
                break;
            }
        }
    }
    if (idx < 0) return {};

    // Skip whitespace to find `[`.
    while (idx < stripped.size() && stripped.at(idx).isSpace()) ++idx;
    if (idx >= stripped.size() || stripped.at(idx) != '[') return {};

    // Collect symbol tokens until `]`.
    QStringList names;
    ++idx; // past `[`
    while (idx < stripped.size()) {
        while (idx < stripped.size() && stripped.at(idx).isSpace()) ++idx;
        if (idx >= stripped.size()) break;
        if (stripped.at(idx) == ']') break;
        // Read a symbol token.
        int start = idx;
        while (idx < stripped.size() && !stripped.at(idx).isSpace()
               && stripped.at(idx) != ']') {
            ++idx;
        }
        names << stripped.mid(start, idx - start);
    }
    return names;
}

// Search upward from `dir` for a `build.tur` file.
QString findBuildTur(const QString& dir) {
    QString current = dir;
    for (int i = 0; i < 32; ++i) {  // bounded climb
        const QString candidate = QDir(current).filePath(QStringLiteral("build.tur"));
        if (QFileInfo::exists(candidate)) return candidate;
        const QString parent = QDir(current).path();
        if (parent == current) break;  // reached root
        current = QDir(current).filePath(QStringLiteral(".."));
        current = QDir(current).canonicalPath();
        if (current.isEmpty()) break;
    }
    return {};
}

} // namespace

QString ExperimentsFilePath() {
    const QString xdg = qEnvironmentVariable("XDG_CONFIG_HOME");
    QString configDir;
    if (!xdg.isEmpty()) {
        configDir = xdg + QStringLiteral("/turmeric");
    } else {
        configDir = QStandardPaths::writableLocation(
                        QStandardPaths::GenericConfigLocation)
                    + QStringLiteral("/turmeric");
    }
    return QDir(configDir).filePath(QStringLiteral("experiments.tur"));
}

QString EnsureExperimentsFile() {
    const QString path = ExperimentsFilePath();
    if (QFile::exists(path)) return path;

    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(
            ";; ~/.config/turmeric/experiments.tur\n"
            ";;\n"
            ";; Experimental features to enable across all projects. Names\n"
            ";; must match `tur experiments`. A project's build.tur :enable\n"
            ";; list takes precedence over this file.\n"
            ";;\n"
            ";; Unknown names surface a diagnostic from turmeric — Trowel\n"
            ";; does not validate them.\n"
            "\n"
            ":enable []\n");
        f.commit();
    }
    return path;
}

ResolvedExperiments ResolveExperiments(const QString& workingDir) {
    ResolvedExperiments result;

    // 1. build.tur :enable — project owner's decision, wins outright.
    if (!workingDir.isEmpty()) {
        const QString buildTur = findBuildTur(workingDir);
        if (!buildTur.isEmpty()) {
            QFile f(buildTur);
            if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                const QString text = QString::fromUtf8(f.readAll());
                const QStringList names = scanEnableList(text);
                // An `:enable` key was found (even if empty) — this is the
                // source. An explicit empty list means the project opted out.
                if (text.contains(QStringLiteral(":enable"))) {
                    result.names = names;
                    result.source = QStringLiteral("build.tur");
                    result.optedOut = names.isEmpty();
                    return result;
                }
            }
        }
    }

    // 2. User settings file — applied only when the project has no :enable.
    const QString userPath = ExperimentsFilePath();
    if (QFile::exists(userPath)) {
        QFile f(userPath);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString text = QString::fromUtf8(f.readAll());
            const QStringList names = scanEnableList(text);
            if (text.contains(QStringLiteral(":enable"))) {
                result.names = names;
                result.source = QStringLiteral("user settings");
                return result;
            }
        }
    }

    // 3. No experiments.
    return result;
}

} // namespace trowel
