#pragma once

#include "lsp/lsp_location.h"

#include <QString>
#include <QVector>

namespace trowel {

// One entry from a textDocument/publishDiagnostics batch, still in LSP
// coordinates — EditorView resolves them to Scintilla positions when painting,
// because the buffer may have moved on since the server saw it.
struct LspDiagnostic {
    // Per the LSP DiagnosticSeverity enum. Absent severity is treated as Error,
    // which is what Turmeric's server emits.
    enum Severity { Error = 1, Warning = 2, Information = 3, Hint = 4 };

    int severity = Error;
    int startLine = 0;
    int startChar = 0;
    int endLine = 0;
    int endChar = 0;
    QString message;
    QString source;

    // `relatedInformation`: other places that explain this diagnostic.
    //
    // The case this exists for is a dependency error. `tur lsp` analyses one
    // document at a time and `load` is textual inclusion, so an error inside a
    // loaded file is reported against the OPEN file, on its `(load "...")`
    // form -- clangd's model for an error inside an `#include`. The range above
    // therefore points at the load, and the real error site arrives here.
    //
    // Trowel used to declare `relatedInformation: false` and drop this, which
    // left the one actionable part of such a diagnostic on the floor: the
    // message named the file and line in prose and nothing could jump to it.
    QVector<LspSpan> related;
    // The related entries' own messages, parallel to `related`. LspSpan has
    // nowhere to put one, and the server's note ("unknown function or operator
    // 'x'") is what makes the jump target legible before you take it.
    QStringList relatedMessages;

    // True when this diagnostic was raised somewhere other than where it is
    // drawn -- i.e. it came out of a `load`ed file. Drives the "error in a
    // dependency" framing; the presence of a related location is the signal,
    // since that is exactly what the server attaches in that case.
    bool isFromDependency() const { return !related.isEmpty(); }
};

}
