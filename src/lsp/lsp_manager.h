#pragma once

#include "lsp/lsp_diagnostic.h"
#include "lsp/lsp_location.h"
#include "lsp/lsp_symbol.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

class QTimer;

namespace trowel {

class EditorView;
class LspClient;

// Owns the single `tur lsp` child shared by the whole application.
//
// One server, not one per window. The Turmeric server indexes only the
// documents it has been sent (workspace/symbol iterates the open-doc store),
// so a per-window rootUri buys nothing — while a second process would mean a
// second blocking compiler competing for the same CPU and a duplicate copy of
// every shared document.
//
// Documents are keyed by URI, not by EditorView: the same file can be open in
// two windows, and LSP has exactly one document per URI. didOpen fires when the
// first view attaches, didClose when the last one detaches.
class LspManager : public QObject {
    Q_OBJECT
public:
    enum class State {
        Disabled,  // turned off in settings — no child is ever spawned
        Stopped,   // enabled, nothing running yet (nothing has asked for it)
        Starting,  // spawned, initialize in flight
        Ready,
        Failed,    // gave up after repeated crashes; see lastError()
    };

    // Application-wide instance, parented to qApp. Created on first use so a
    // Trowel session that never opens a Turmeric file never spawns a server.
    static LspManager* instance();

    State state() const { return state_; }
    QString lastError() const { return lastError_; }
    // Resolved path of the server binary, or empty if none could be found.
    QString serverPath() const;
    static bool enabledInSettings();

    // Attach/detach a buffer. Non-Turmeric buffers and buffers with no path are
    // ignored — see kSkipUnsavedReason.
    void openDocument(EditorView* view);
    void closeDocument(EditorView* view);
    // Buffer edited; schedules a debounced didChange.
    void documentChanged(EditorView* view);

    using CompletionCallback = std::function<void(const QStringList& labels)>;
    using HoverCallback = std::function<void(const QString& text)>;
    using DefinitionCallback = std::function<void(const LspLocation&)>;

    // All three drop their reply if the document changed underneath them, so a
    // stale popup can never appear over newer text. The callback simply isn't
    // invoked in that case.
    void requestCompletion(EditorView* view, int pos, CompletionCallback cb);
    void requestHover(EditorView* view, int pos, HoverCallback cb);
    // Unlike the other two, this one reports "no answer" rather than staying
    // silent: it fires `cb` with an invalid LspLocation when the server returns
    // null or an empty array. A jump that quietly does nothing is
    // indistinguishable from a jump that is still in flight.
    void requestDefinition(EditorView* view, int pos, DefinitionCallback cb);

    using SignatureHelpCallback = std::function<void(const QString& text,
                                                    int activeParameter)>;
    // The parameter list of the call the cursor is inside.
    //
    // The capability Trowel was missing that costs the most in a lisp: by the
    // time you have typed `(vec-get ` the name is behind you and the arity and
    // argument types are exactly what you cannot see. The server advertises `(`
    // as the trigger and space as the retrigger.
    //
    // Fires with an EMPTY string when there is no signature at `pos` -- which
    // is the common case, since the server answers null anywhere but argument
    // position. Reported rather than dropped, so a caller can tell "no
    // signature" from "still in flight".
    //
    // `activeParameter` is the index the server believes the cursor is on, or
    // -1 when it does not say. Reported rather than used to re-render the
    // label, because the call tip surface this ends up on is plain text and
    // cannot highlight a range.
    void requestSignatureHelp(EditorView* view, int pos, SignatureHelpCallback cb);

    using SymbolsCallback = std::function<void(const QVector<LspSymbol>&)>;
    // Document outline, in the order the server returns it — which is document
    // order, and is the one thing an outline is for. Never sorted here.
    //
    // Like requestDefinition, an empty result is reported rather than dropped:
    // §4.2.1 measured that a file with any analysis error yields `[]`, and the
    // caller has to be able to say so.
    void requestDocumentSymbols(EditorView* view, SymbolsCallback cb);

    using WorkspaceSymbolsCallback = std::function<void(const QVector<LspSpan>&,
                                                       const QStringList& names)>;
    // Symbols matching `query` across the whole workspace, not just this file.
    //
    // Parallel vectors rather than a struct: the spans are what a jump needs
    // and the names are what a picker shows, and LspSpan has nowhere to put a
    // name. An empty `query` is sent as-is -- the server decides whether that
    // means "everything" or "nothing", and guessing here would be a second
    // policy.
    void requestWorkspaceSymbols(const QString& query, WorkspaceSymbolsCallback cb);

    using HighlightsCallback = std::function<void(const QVector<LspRange>&)>;
    // Every occurrence of the symbol at `pos`, from the server's index.
    //
    // Token-based, not textual: the same spelling inside a comment or a string
    // is not a use and does not come back. That is the whole reason this goes
    // through the server rather than through Scintilla's word matching.
    void requestDocumentHighlights(EditorView* view, int pos, HighlightsCallback cb);

    using ReferencesCallback = std::function<void(const QVector<LspSpan>&)>;
    // Every real use of the symbol at `pos`, across the workspace.
    //
    // An oversized workspace answers with a *shorter list*, not an error, so a
    // caller can never prove the list is complete — label the UI accordingly.
    void requestReferences(EditorView* view, int pos, bool includeDeclaration,
                           ReferencesCallback cb);

    // The answer to "may I rename what is at this position, and what is it
    // called now".
    //
    // Three states, because the server has three (§3.1.1 of the editor
    // intelligence plan): a range means yes; `refusal` carries the server's own
    // words for the seven documented no-cases, which arrive as JSON-RPC errors;
    // both empty means there was simply nothing at that position.
    struct PrepareRename {
        LspRange range;
        QString placeholder;
        QString refusal;
        bool renameable = false;
    };
    using PrepareRenameCallback = std::function<void(const PrepareRename&)>;
    void requestPrepareRename(EditorView* view, int pos, PrepareRenameCallback cb);

    // Empty edit with a non-empty `error` when the server refused; empty with
    // an empty error means it had nothing to change.
    using WorkspaceEdit = LspWorkspaceEdit;
    using RenameCallback =
        std::function<void(const WorkspaceEdit& edit, const QString& error)>;
    // Rename is the most expensive request in the protocol — the workspace half
    // compiles every importing file for its own binding table before editing it
    // — so this one gets its own budget rather than the 2 s default.
    void requestRename(EditorView* view, int pos, const QString& newName,
                       RenameCallback cb);

    // Directory the bundled stdlib was pinned to, or empty when none was found
    // next to the resolved binary.
    //
    // The single source of truth for "is this buffer part of the read-only
    // stdlib". Deriving that path a second time somewhere else is how the two
    // copies drift after a TROWEL_TURMERIC_VERSION bump.
    QString stdlibDir() const { return stdlibDir_; }

    // True for a file inside that directory. Buffers holding one open
    // read-only, stay out of recent files, and stay out of the persisted
    // session — see the navigation plan §5.3.
    bool isStdlibPath(const QString& path) const;

    QVector<LspDiagnostic> diagnosticsFor(const QString& uri) const;
    // True once the server has published at least one batch for this URI.
    // Distinguishes "analyzed, clean" from "not analyzed yet" — diagnosticsFor
    // returns an empty vector for both.
    bool hasPublishedFor(const QString& uri) const;

    void restart();
    void shutdown();

    static QString UriForPath(const QString& path);
    static QString PathForUri(const QString& uri);

    // Why an unsaved buffer gets no language support, surfaced in the UI rather
    // than failing silently.
    static const char* kSkipUnsavedReason;

signals:
    void diagnosticsUpdated(const QString& uri);
    void stateChanged();

private:
    explicit LspManager(QObject* parent = nullptr);

    struct DocState {
        QVector<QPointer<EditorView>> views;
        int version = 0;
        // Bumped on every edit. A request captures it and discards its reply if
        // it no longer matches — the staleness guard.
        int generation = 0;
        QTimer* debounce = nullptr;
        bool openOnServer = false;
    };

    // A buffer position as the server's line/character.
    QJsonObject positionJson(EditorView* view, int pos) const;

    bool ensureStarted();
    void setState(State s, const QString& error = {});
    void onInitializeReply();
    void onNotification(const QString& method, const QJsonObject& params);
    void onServerFinished(int exitCode);

    void sendDidOpen(const QString& uri);
    void sendDidChange(const QString& uri);
    void flushPendingOpens();

    // The URI a view maps to, or empty when the view isn't eligible.
    static QString UriFor(EditorView* view);
    DocState* docFor(EditorView* view);
    EditorView* primaryView(const QString& uri);

    LspClient* client_ = nullptr;
    State state_ = State::Stopped;
    // Set while restart() is tearing the child down, so the resulting `finished`
    // signal isn't mistaken for a crash and counted against restartsRemaining_.
    bool restarting_ = false;
    QString lastError_;
    int restartsRemaining_ = 3;
    QString stdlibDir_;

    QHash<QString, DocState> docs_;
    QHash<QString, QVector<LspDiagnostic>> diagnostics_;
};

}
