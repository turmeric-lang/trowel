#include "repl/run_buffer.h"

#include "editor/dialect.h"
#include "editor/editor_view.h"
#include "repl/repl_session.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QStandardPaths>

namespace trowel {

namespace {

QString scratchDir() {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    const QString dir = base + "/scratch";
    QDir().mkpath(dir);
    return dir;
}

// Scratch-file extension for the buffer's dialect -- `.tur.sweet` for the sweet
// Turmeric and Saffron readers, `.scm` for `r7rs`, `.tur` otherwise. Those are
// the only two suffixes the toolchain maps to a non-default reader. The
// buffer's own `#lang` line, when it has one, travels with the contents and
// covers everything no suffix can express (`r7rs/sweet`, and the Saffron and
// curly-infix/neoteric bases).
QString extensionFor(const EditorView* editor) {
    return QString::fromLatin1(DialectScratchExtension(editor->dialect()));
}

// Put the session in the buffer's language before running it, and say so.
//
// The READER travels with the file, so a sweet or curly-infix buffer needs
// nothing. The LANGUAGE does not: it selects a prelude the session either has
// or has not loaded, and a Scheme buffer run against a Turmeric session fails
// on `r7rs-display` no matter how the file is spelled. See
// DialectNeedsSessionSwitch for the measurements.
//
// The switch RESETS the session. That is upstream's behaviour and not something
// Trowel can soften, so the cost is reported rather than hidden -- and reported
// differently depending on whether it actually cost anything, which is what
// `dirtiedSinceReset` is for. No confirmation prompt: one on every
// cross-language run would be intolerable, and the thing being discarded is a
// REPL environment the user can rebuild by re-running.
//
// Not waited on. The REPL consumes its input line by line in order, so the
// `#lang` line and the command behind it arrive in the right sequence.
QString switchSessionIfNeeded(ReplSession* repl, Dialect buffer) {
    const Dialect session = repl->dialect();
    if (!DialectNeedsSessionSwitch(session, buffer)) return {};

    const bool losing = repl->dirtiedSinceReset();
    if (!repl->switchDialect(buffer)) return {};
    return losing
        ? QString(" (session switched to %1; its definitions were discarded)")
              .arg(DialectBaseToken(buffer))
        : QString(" (session switched to %1)").arg(DialectBaseToken(buffer));
}

// Escape a filesystem path for embedding in a turmeric string literal.
QString escapeForTurmericString(const QString& path) {
    QString out;
    out.reserve(path.size() + 2);
    for (QChar c : path) {
        if (c == '\\' || c == '"') out.append('\\');
        out.append(c);
    }
    return out;
}

// Run a whole program: `:run <path>`, the REPL's own entry point for "execute
// this file".
//
// NOT `(load "<path>")`. `load` evaluates the top-level forms and stops, so a
// program shaped the way Turmeric programs are shaped — a `defn main` and
// little else at the top level — defined `main`, printed `=> #<fn main>`, and
// never ran. Run Buffer reported success on a program that had not executed.
//
// `:run` (repl.c, cmd_run) evaluates the file and then invokes `main` when it
// resolves to a closure, staying silent when there is none, so a script of
// bare top-level forms still behaves as it always did. It also installs a
// fresh environment first, which is why no separate `:reset` is sent — that
// was this function's job before and is now `:run`'s.
//
// The path is the rest of the line, unquoted and unescaped: cmd_run takes
// everything after `:run ` verbatim, so a path with spaces needs no quoting
// and quoting it would make the quotes part of the filename.
QByteArray runCommandFor(const QString& path) {
    return QString(":run %1").arg(path).toUtf8();
}

// Evaluate a fragment in the current environment: `(load "<path>")`.
//
// Deliberately still `load`, and deliberately no reset — a selection is
// evaluated against whatever the session already holds, and must not invoke
// `main` just because the buffer it came from defines one.
QByteArray loadCommandFor(const QString& path) {
    return QString("(load \"%1\")").arg(escapeForTurmericString(path)).toUtf8();
}

// Write `contents` to a scratch file and hand the path back, or set `r.message`
// and return empty on failure.
QString writeScratch(const QByteArray& contents, const QString& ext,
                     RunResult& r) {
    const QString dir = scratchDir();
    const quint32 stamp = QRandomGenerator::global()->generate();
    const QString path = QString("%1/buffer-%2%3")
        .arg(dir)
        .arg(stamp, 8, 16, QChar('0'))
        .arg(ext);

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        r.message = QString("Could not write scratch file at %1").arg(path);
        return {};
    }
    file.write(contents);
    file.close();
    return path;
}

RunResult writeScratchAndLoad(ReplSession* repl,
                              const QByteArray& contents, const QString& ext) {
    RunResult r;
    const QString path = writeScratch(contents, ext, r);
    if (path.isEmpty()) return r;
    if (!repl->sendCommand(loadCommandFor(path))) {
        r.message = "REPL is not running.";
        return r;
    }
    r.ok = true;
    r.scratchPath = path;
    r.message = QString("Loaded %1").arg(QFileInfo(path).fileName());
    return r;
}

// Run a whole buffer: always `:run`, for every dialect.
//
// This used to branch, routing `.tur.sweet` and `.sweet` through `:reset` plus
// `(load ...)` because `:run` on a file whose EXTENSION selected a non-default
// reader threw away the preload it had just installed and then dropped the
// elaboration error on the floor -- measured against the bundled v0.42.2, which
// defined nothing at all and said nothing about it.
//
// That is fixed upstream, and the workaround had become the bug it was working
// around: `load` evaluates the top-level forms and stops, so a sweet program
// shaped like a program -- a `defn main` and little else -- was DEFINED and
// never run, which is the defect `6a7d17b` fixed for `.tur` and left standing
// for sweet. Re-measured against the pinned v0.60.1, `:run` on a headerless
// `.tur.sweet` with a `defn main` prints and returns normally, while `load` on
// the same file still answers `=> #<fn main>` and prints nothing.
//
// No `:reset` either: `:run` installs a fresh environment itself.
RunResult sendWholeBuffer(ReplSession* repl, const QString& path) {
    RunResult r;
    if (!repl->sendCommand(runCommandFor(path))) {
        r.message = "REPL is not running.";
        return r;
    }
    r.ok = true;
    r.message = QString("Ran %1").arg(QFileInfo(path).fileName());
    return r;
}

RunResult writeScratchAndRun(ReplSession* repl,
                             const QByteArray& contents, const QString& ext) {
    RunResult r;
    const QString path = writeScratch(contents, ext, r);
    if (path.isEmpty()) return r;
    r = sendWholeBuffer(repl, path);
    if (r.ok) r.scratchPath = path;
    return r;
}

// True when `name` is one of the two spellings the toolchain accepts for a
// project manifest. See the Turmeric "developing spices" guide: `build.tur`
// and `build.tur.sweet` are equivalent everywhere, with the plain one winning
// when both are present.
bool isBuildManifestName(const QString& name) {
    return name.compare("build.tur", Qt::CaseInsensitive) == 0
        || name.compare("build.tur.sweet", Qt::CaseInsensitive) == 0;
}

// Extensions that make a file Turmeric-family source -- `.tur`, `.tur.sweet`,
// `.scm`, `.sweet`. Asked of the dialect table rather than re-listed, so adding
// a dialect extension cannot enable highlighting while leaving Run Buffer
// greyed out (which is what happened to `.scm`).
bool hasTurmericExtension(const QString& name) {
    Dialect ignored = Dialect::Turmeric;
    return DialectForFileName(name, ignored);
}

}

EvalMode EvalModeForPath(const QString& path) {
    if (path.isEmpty()) return EvalMode::Buffer;  // untitled scratch buffer
    const QString name = QFileInfo(path).fileName();
    if (isBuildManifestName(name)) return EvalMode::Project;
    return hasTurmericExtension(name) ? EvalMode::Buffer : EvalMode::Disabled;
}

QString ProjectDirForPath(const QString& buildManifestPath) {
    if (buildManifestPath.isEmpty()) return {};
    const QFileInfo info(buildManifestPath);
    if (!isBuildManifestName(info.fileName())) return {};
    return info.absolutePath();
}

RunResult RunBuffer(EditorView* editor, ReplSession* repl) {
    RunResult r;
    if (!editor || !repl) {
        r.message = "Editor or REPL is not available.";
        return r;
    }
    if (!repl->isRunning()) {
        r.message = "REPL is not running — start it from Run > Restart REPL.";
        return r;
    }

    // A whole-buffer run gets a fresh env — `:run` installs one itself.
    // Selections go through RunRange instead and keep the current env.

    const QString note = switchSessionIfNeeded(repl, editor->dialect());

    // If saved on disk and clean, run the file in place — so any error the
    // REPL reports names the user's own file rather than a scratch copy.
    if (!editor->filePath().isEmpty() && !editor->isModified()) {
        r = sendWholeBuffer(repl, editor->filePath());
    } else {
        // Dirty or untitled — write to scratch first.
        r = writeScratchAndRun(repl, editor->text(), extensionFor(editor));
    }
    if (r.ok) r.message += note;
    return r;
}

RunResult RunRange(EditorView* editor, ReplSession* repl, int startPos, int endPos) {
    RunResult r;
    if (!editor || !repl) {
        r.message = "Editor or REPL is not available.";
        return r;
    }
    if (!repl->isRunning()) {
        r.message = "REPL is not running — start it from Run > Restart REPL.";
        return r;
    }
    if (endPos <= startPos) {
        r.message = "Empty selection.";
        return r;
    }
    QByteArray contents = editor->textInRange(startPos, endPos);
    if (contents.isEmpty()) {
        r.message = "Empty selection.";
        return r;
    }
    const Dialect dialect = editor->dialect();

    // A selection from mid-buffer leaves the `#lang` line behind, which would
    // run the region under a different reader than the one it was written for.
    // A scratch file's EXTENSION can carry two bases (`.tur.sweet` and `.scm`)
    // and no more, so for the other eight the line is the only carrier there
    // is -- `r7rs/sweet` has no extension at all, and neither Saffron nor the
    // curly-infix and neoteric readers have one.
    if (startPos > 0) {
        QByteArray directive = editor->langDirectiveLine();
        // Synthesize one when the buffer had none to copy. A `.scm` file is
        // Scheme by extension with no header to find, and that is the
        // idiomatic way to write one -- so a selection out of it would
        // otherwise be read as Turmeric.
        if (directive.isEmpty() && dialect != Dialect::Turmeric) {
            directive = QByteArray("#lang ") + DialectBaseToken(dialect) + "\n";
        }
        if (!directive.isEmpty()) contents.prepend(directive);
    }

    const QString note = switchSessionIfNeeded(repl, dialect);
    r = writeScratchAndLoad(repl, contents, extensionFor(editor));
    if (r.ok) r.message += note;
    return r;
}

}
