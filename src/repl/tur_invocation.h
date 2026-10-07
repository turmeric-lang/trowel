#pragma once

#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

namespace trowel {

// Resolve the `tur` executable using the same order as every call site:
// QSettings "turmeric.path" override, then bundled binary inside Trowel.app,
// then PATH lookup. Returns an absolute path, or an empty string when none
// of the candidates exist and are executable.
QString ResolveTurBinary();

// The stdlib directory belonging to `turBinary`, or an empty string when it
// has none beside it.
//
// Every `tur` invocation Trowel makes must pin TUR_STDLIB_DIR to this, or the
// ambient environment pairs the binary we chose with somebody else's stdlib.
// Both archive shapes are probed (flat `stdlib/` beside the binary, and
// prefix `share/turmeric/stdlib/` one level up) because Turmeric has shipped
// both layouts.
QString TurStdlibDirFor(const QString& turBinary);

// A fully-resolved `tur` invocation: binary, argv, and environment. The one
// seam through which every `tur` subprocess is constructed, so global flags
// and env pinning cannot drift between call sites.
struct TurInvocation {
    // Resolved binary path, or empty with `error` set when none was found.
    QString binary;
    // Full argv: the subcommand passed to MakeTurInvocation, unchanged.
    // (Engine selection is injected as the TUR_ENGINE env var, not argv,
    // because `tur build` rejects --engine on the command line.)
    QStringList args;
    // System environment with TUR_STDLIB_DIR pinned to the resolved binary's
    // sibling stdlib, when one exists. One spelling of the pin, finally —
    // the four copies that lived across repl_session, project_runner,
    // lsp_manager, main_window, and trace_runner are gone.
    QProcessEnvironment env;
    // Human-readable "tried these three things" message when `binary` is
    // empty, so the caller can show it in a banner without re-deriving the
    // candidate paths.
    QString error;

    // The env overrides as "KEY=VALUE" strings, for the APIs that take a
    // QStringList (PtySession, LspClient, DapClient). Returns only the
    // entries that differ from the inherited environment.
    QStringList envEntries() const;
};

// Build a `tur` invocation from a subcommand. `subcommand` is the leading
// argv (e.g. {"build", dir}, {"repl"}, {"lsp"}, {"fmt", "--stdin", ...});
// the resolver fills in the binary path and the TUR_STDLIB_DIR environment
// so every call site gets them consistently. When the binary cannot be
// resolved, `binary` is empty and `error` is set — the caller should show
// `error` and not spawn.
TurInvocation MakeTurInvocation(const QStringList& subcommand,
                                 const QString& workingDir = {});

} // namespace trowel
