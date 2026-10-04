#include "editor/dialect.h"
#include "lsp/lsp_manager.h"

#include "editor/editor_view.h"
#include "editor/lexers.h"
#include "lsp/lsp_client.h"
#include "lsp/lsp_position.h"
#include "repl/repl_session.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QSettings>
#include <QTimer>
#include <QUrl>

namespace trowel {

namespace {

LspRange RangeFromJson(const QJsonObject& range) {
    const QJsonObject start = range.value("start").toObject();
    const QJsonObject end = range.value("end").toObject();
    return LspRange{start.value("line").toInt(), start.value("character").toInt(),
                    end.value("line").toInt(), end.value("character").toInt()};
}


// The server recompiles the whole buffer on every didChange, inline on its
// single thread. Coalesce keystrokes so a fast typist doesn't queue a compile
// per character.
constexpr int kDidChangeDebounceMs = 250;

// Analysis can take a while on a large file; completion and hover are
// interactive and should give up fast rather than pop up over stale text.
constexpr int kInteractiveTimeoutMs = 1500;
// Rename and its prepare step compile every importing file for its own binding
// table before editing it, so they are the two requests in the protocol that
// legitimately take seconds. Everything else queues behind them on the server's
// single thread, which is why the UI has to show it is busy (§8 of the plan).
constexpr int kRenameTimeoutMs = 10000;
constexpr int kInitializeTimeoutMs = 5000;

constexpr const char* kLanguageId = "turmeric";

} // namespace

const char* LspManager::kSkipUnsavedReason =
    "unsaved buffers get no language support — save the file to enable it";

LspManager* LspManager::instance() {
    static LspManager* self = new LspManager(qApp);
    return self;
}

LspManager::LspManager(QObject* parent)
    : QObject(parent)
{
    if (!enabledInSettings()) state_ = State::Disabled;
}

bool LspManager::enabledInSettings() {
    return QSettings().value("lsp/enabled", true).toBool();
}

QString LspManager::serverPath() const {
    const QString override = QSettings().value("lsp/serverPath").toString();
    if (!override.isEmpty()) {
        const QFileInfo fi(override);
        if (fi.exists() && fi.isFile() && fi.isExecutable()) return fi.absoluteFilePath();
    }
    // Same binary the REPL uses — settings override, then the copy bundled in
    // Trowel.app, then PATH.
    return ResolveTurBinary();
}

QString LspManager::UriForPath(const QString& path) {
    if (path.isEmpty()) return {};
    return QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()).toString();
}

QString LspManager::PathForUri(const QString& uri) {
    return QUrl(uri).toLocalFile();
}

QString LspManager::UriFor(EditorView* view) {
    if (!view) return {};
    // Every `#lang` base is served by the same language server, so eligibility
    // is "is this Turmeric-family source" -- which is the dialect table's
    // question, not a list of highlighting languages.
    //
    // It WAS such a list (`Turmeric || TurmericSweet`), and that made this the
    // third place the set of Turmeric file types lived. Adding `.scm` to the
    // other two therefore gave Scheme files highlighting and Run Buffer but no
    // language server at all -- silently, because an ineligible view just gets
    // an empty URI.
    Dialect ignored = Dialect::Turmeric;
    if (!DialectForFileName(view->filePath(), ignored)) return {};
    // No path means no stable URI, and the server resolves diagnostics against
    // a real file path. Untitled buffers are out of scope for v1.
    return UriForPath(view->filePath());
}

LspManager::DocState* LspManager::docFor(EditorView* view) {
    const QString uri = UriFor(view);
    if (uri.isEmpty()) return nullptr;
    const auto it = docs_.find(uri);
    return it == docs_.end() ? nullptr : &it.value();
}

EditorView* LspManager::primaryView(const QString& uri) {
    const auto it = docs_.find(uri);
    if (it == docs_.end()) return nullptr;
    for (const QPointer<EditorView>& v : it->views) {
        if (v) return v.data();
    }
    return nullptr;
}

void LspManager::setState(State s, const QString& error) {
    if (!error.isEmpty()) lastError_ = error;
    if (state_ == s) return;
    state_ = s;
    emit stateChanged();
}

bool LspManager::ensureStarted() {
    if (state_ == State::Disabled || state_ == State::Failed) return false;
    if (state_ == State::Starting || state_ == State::Ready) return true;

    const QString binary = serverPath();
    if (binary.isEmpty()) {
        setState(State::Failed, QStringLiteral("could not locate the `tur` binary"));
        return false;
    }

    if (!client_) {
        client_ = new LspClient(this);
        connect(client_, &LspClient::notificationReceived, this, &LspManager::onNotification);
        connect(client_, &LspClient::finished, this, &LspManager::onServerFinished);
        connect(client_, &LspClient::startFailed, this, [this](const QString& why) {
            setState(State::Failed, why);
        });
    }

    // Pin the stdlib belonging to the resolved binary, so an ambient
    // TUR_STDLIB_DIR can't pair a new `tur` with an old stdlib. See
    // TurStdlibDirFor -- this call site had the flat-layout-only probe, and a
    // prefix-layout bundle therefore inherited the ambient stdlib and reported
    // its diagnostics against every open buffer.
    QStringList extraEnv;
    const QString siblingStdlib = TurStdlibDirFor(binary);
    if (!siblingStdlib.isEmpty()) {
        extraEnv << QStringLiteral("TUR_STDLIB_DIR=") + siblingStdlib;
        // Remembered so stdlibDir() can answer "did this definition land inside
        // the bundle?" without re-deriving the path from the binary.
        stdlibDir_ = siblingStdlib;
    } else {
        stdlibDir_.clear();
    }

    if (!client_->start(binary, {"lsp"}, QString(), extraEnv)) {
        setState(State::Failed, QStringLiteral("failed to start `%1 lsp`").arg(binary));
        return false;
    }
    setState(State::Starting);

    QJsonObject params{
        {"processId", QCoreApplication::applicationPid()},
        {"rootUri", QJsonValue::Null},
        {"clientInfo", QJsonObject{{"name", "Trowel"}}},
        {"capabilities", QJsonObject{
            // LSP 3.17 position-encoding negotiation. Trowel's `character` is a
            // BYTE offset (see lsp/lsp_position.h), which used to be a
            // deliberate mismatch with the spec's UTF-16 default -- the pinned
            // server now declares `"positionEncoding":"utf-8"` itself, so
            // asking for it makes bytes the negotiated truth rather than a
            // shared assumption. utf-16 is listed second so a server that will
            // not speak utf-8 still gets a legal answer.
            {"general", QJsonObject{
                {"positionEncodings", QJsonArray{"utf-8", "utf-16"}},
            }},
            {"textDocument", QJsonObject{
                {"synchronization", QJsonObject{{"dynamicRegistration", false}}},
                {"hover", QJsonObject{{"contentFormat", QJsonArray{"markdown", "plaintext"}}}},
                {"completion", QJsonObject{{"dynamicRegistration", false}}},
                // relatedInformation: true, because a dependency error's only
                // actionable part arrives in it. Declaring false -- which this
                // did -- tells the server the client cannot use the note, and
                // Trowel then had no way to offer a jump to the real error
                // site inside a `load`ed file.
                {"publishDiagnostics", QJsonObject{{"relatedInformation", true}}},
            }},
        }},
    };
    client_->request("initialize", params,
                     [this](const QJsonValue&, const LspError* err) {
                         if (err) {
                             setState(State::Failed, err->message);
                             return;
                         }
                         onInitializeReply();
                     },
                     kInitializeTimeoutMs);
    return true;
}

void LspManager::onInitializeReply() {
    client_->notify("initialized", QJsonObject{});
    setState(State::Ready);
    flushPendingOpens();
}

void LspManager::flushPendingOpens() {
    for (auto it = docs_.begin(); it != docs_.end(); ++it) {
        if (!it->openOnServer) sendDidOpen(it.key());
    }
}

void LspManager::onServerFinished(int exitCode) {
    for (auto it = docs_.begin(); it != docs_.end(); ++it) it->openOnServer = false;

    if (state_ == State::Disabled) return;
    // A restart we asked for isn't a crash. QProcess can deliver `finished`
    // synchronously from inside terminate(), so this reentrancy is the norm,
    // not an edge case.
    if (restarting_) return;

    if (restartsRemaining_ <= 0) {
        setState(State::Failed,
                 QStringLiteral("language server keeps exiting (last code %1)").arg(exitCode));
        return;
    }
    restartsRemaining_--;
    setState(State::Stopped, QStringLiteral("language server exited (%1), restarting")
                                 .arg(exitCode));
    // Re-establish only if something is still open to serve.
    if (!docs_.isEmpty() && ensureStarted()) return;
    setState(State::Stopped);
}

void LspManager::openDocument(EditorView* view) {
    const QString uri = UriFor(view);
    if (uri.isEmpty()) return;
    if (state_ == State::Disabled || state_ == State::Failed) return;

    DocState& doc = docs_[uri];
    if (!doc.views.contains(view)) doc.views.append(view);

    if (!doc.debounce) {
        doc.debounce = new QTimer(this);
        doc.debounce->setSingleShot(true);
        doc.debounce->setInterval(kDidChangeDebounceMs);
        connect(doc.debounce, &QTimer::timeout, this, [this, uri] { sendDidChange(uri); });
    }

    if (!ensureStarted()) return;
    if (state_ == State::Ready && !doc.openOnServer) sendDidOpen(uri);
}

void LspManager::closeDocument(EditorView* view) {
    // The view's path may already have changed, so search by identity rather
    // than trusting UriFor() to still point at the right entry.
    for (auto it = docs_.begin(); it != docs_.end();) {
        it->views.removeAll(QPointer<EditorView>(view));
        it->views.removeAll(QPointer<EditorView>(nullptr));
        if (!it->views.isEmpty()) {
            ++it;
            continue;
        }
        const QString uri = it.key();
        if (it->openOnServer && client_ && state_ == State::Ready) {
            client_->notify("textDocument/didClose",
                            QJsonObject{{"textDocument", QJsonObject{{"uri", uri}}}});
        }
        if (it->debounce) it->debounce->deleteLater();
        it = docs_.erase(it);
        diagnostics_.remove(uri);
        emit diagnosticsUpdated(uri);
    }
}

void LspManager::documentChanged(EditorView* view) {
    DocState* doc = docFor(view);
    if (!doc) return;
    doc->version++;
    doc->generation++;
    if (doc->debounce) doc->debounce->start();  // restarts, coalescing bursts
}

QJsonObject LspManager::positionJson(EditorView* view, int pos) const {
    return LspPositionToJson(LspPositionFromPos(view->sciWidget(), pos));
}

void LspManager::sendDidOpen(const QString& uri) {
    if (!client_ || state_ != State::Ready) return;
    EditorView* view = primaryView(uri);
    if (!view) return;

    DocState& doc = docs_[uri];
    doc.openOnServer = true;
    client_->notify("textDocument/didOpen", QJsonObject{
        {"textDocument", QJsonObject{
            {"uri", uri},
            {"languageId", kLanguageId},
            {"version", doc.version},
            {"text", QString::fromUtf8(view->text())},
        }},
    });
}

void LspManager::sendDidChange(const QString& uri) {
    if (!client_ || state_ != State::Ready) return;
    const auto it = docs_.find(uri);
    if (it == docs_.end()) return;
    if (!it->openOnServer) {
        sendDidOpen(uri);
        return;
    }
    EditorView* view = primaryView(uri);
    if (!view) return;

    // Full-document sync: the server advertises textDocumentSync 1 and reads
    // only contentChanges[0].text, so a range change would be silently
    // mishandled.
    //
    // Recomputed per change rather than reused from didOpen: the user can type
    // a `#lang` line into a `.scm` buffer at any point, and from then on the
    // synthesized one must stop being added -- otherwise the document gains a
    // second header and every line is off by one.
    client_->notify("textDocument/didChange", QJsonObject{
        {"textDocument", QJsonObject{{"uri", uri}, {"version", it->version}}},
        {"contentChanges", QJsonArray{
            QJsonObject{{"text", QString::fromUtf8(view->text())}},
        }},
    });
}

void LspManager::onNotification(const QString& method, const QJsonObject& params) {
    if (method != QLatin1String("textDocument/publishDiagnostics")) return;

    const QString uri = params.value("uri").toString();
    if (uri.isEmpty()) return;

    QVector<LspDiagnostic> out;
    const QJsonArray items = params.value("diagnostics").toArray();
    out.reserve(items.size());
    for (const QJsonValue& v : items) {
        const QJsonObject o = v.toObject();
        const QJsonObject range = o.value("range").toObject();
        const QJsonObject start = range.value("start").toObject();
        const QJsonObject end = range.value("end").toObject();

        LspDiagnostic d;
        d.severity = o.value("severity").toInt(1);
        if (d.severity < 1 || d.severity > 4) d.severity = 1;
        d.startLine = start.value("line").toInt();
        d.startChar = start.value("character").toInt();
        d.endLine = end.value("line").toInt();
        d.endChar = end.value("character").toInt();
        d.message = o.value("message").toString();
        d.source = o.value("source").toString();

        for (const QJsonValue& rv : o.value("relatedInformation").toArray()) {
            const QJsonObject ro = rv.toObject();
            const QJsonObject loc = ro.value("location").toObject();
            const QString relUri = loc.value("uri").toString();
            if (relUri.isEmpty()) continue;
            d.related.append(LspSpan{relUri, RangeFromJson(loc.value("range").toObject())});
            d.relatedMessages.append(ro.value("message").toString());
        }
        out.append(d);
    }

    diagnostics_.insert(uri, out);
    emit diagnosticsUpdated(uri);
}

QVector<LspDiagnostic> LspManager::diagnosticsFor(const QString& uri) const {
    return diagnostics_.value(uri);
}

bool LspManager::hasPublishedFor(const QString& uri) const {
    return diagnostics_.contains(uri);
}

void LspManager::requestCompletion(EditorView* view, int pos, CompletionCallback cb) {
    DocState* doc = docFor(view);
    if (!doc || !client_ || state_ != State::Ready) return;

    const QString uri = UriFor(view);
    const int generation = doc->generation;

    // Send any buffered edit first — completing against text the server hasn't
    // seen yet returns symbols from the wrong buffer.
    if (doc->debounce && doc->debounce->isActive()) {
        doc->debounce->stop();
        sendDidChange(uri);
    }

    client_->request("textDocument/completion", QJsonObject{
        {"textDocument", QJsonObject{{"uri", uri}}},
        {"position", positionJson(view, pos)},
    },
    [this, cb = std::move(cb), uri, generation](const QJsonValue& result, const LspError* err) {
        if (err || !cb) return;
        const auto it = docs_.constFind(uri);
        if (it == docs_.constEnd() || it->generation != generation) return;  // stale

        // The server returns a bare CompletionItem[]; the spec also allows a
        // CompletionList. Accept both.
        QJsonArray items = result.isArray() ? result.toArray()
                                            : result.toObject().value("items").toArray();
        QStringList labels;
        labels.reserve(items.size());
        for (const QJsonValue& v : items) {
            const QString label = v.toObject().value("label").toString();
            if (!label.isEmpty()) labels.append(label);
        }
        cb(labels);
    },
    kInteractiveTimeoutMs);
}

void LspManager::requestHover(EditorView* view, int pos, HoverCallback cb) {
    DocState* doc = docFor(view);
    if (!doc || !client_ || state_ != State::Ready) return;

    const QString uri = UriFor(view);
    const int generation = doc->generation;

    client_->request("textDocument/hover", QJsonObject{
        {"textDocument", QJsonObject{{"uri", uri}}},
        {"position", positionJson(view, pos)},
    },
    [this, cb = std::move(cb), uri, generation](const QJsonValue& result, const LspError* err) {
        if (err || !cb) return;
        const auto it = docs_.constFind(uri);
        if (it == docs_.constEnd() || it->generation != generation) return;  // stale

        // `contents` is a MarkedString, a MarkedString[], or a MarkupContent.
        const QJsonValue contents = result.toObject().value("contents");
        QString text;
        if (contents.isString()) {
            text = contents.toString();
        } else if (contents.isObject()) {
            text = contents.toObject().value("value").toString();
        } else if (contents.isArray()) {
            QStringList parts;
            for (const QJsonValue& v : contents.toArray()) {
                parts << (v.isString() ? v.toString() : v.toObject().value("value").toString());
            }
            text = parts.join('\n');
        }
        if (!text.trimmed().isEmpty()) cb(text);
    },
    kInteractiveTimeoutMs);
}

void LspManager::requestDefinition(EditorView* view, int pos, DefinitionCallback cb) {
    DocState* doc = docFor(view);
    if (!doc || !client_ || state_ != State::Ready) {
        if (cb) cb(LspLocation{});
        return;
    }

    const QString uri = UriFor(view);
    const int generation = doc->generation;

    // Same reason completion flushes: resolving a position against text the
    // server has not seen yet answers about the wrong buffer, and here that
    // means jumping the user to the wrong line rather than merely offering a
    // stale list.
    if (doc->debounce && doc->debounce->isActive()) {
        doc->debounce->stop();
        sendDidChange(uri);
    }

    client_->request("textDocument/definition", QJsonObject{
        {"textDocument", QJsonObject{{"uri", uri}}},
        {"position", positionJson(view, pos)},
    },
    [this, cb = std::move(cb), uri, generation](const QJsonValue& result, const LspError* err) {
        if (!cb) return;
        const auto it = docs_.constFind(uri);
        // Stale: the user kept typing. Staying silent is right here — landing a
        // jump against text that has moved is worse than not jumping.
        if (it == docs_.constEnd() || it->generation != generation) return;
        if (err) { cb(LspLocation{}); return; }

        // The server writes a bare Location (lsp.c:940-957), but the spec also
        // allows Location[] and LocationLink[]. Accept all three: the server is
        // under active development and a conforming change should not break
        // navigation.
        QJsonObject loc;
        if (result.isObject()) {
            loc = result.toObject();
        } else if (result.isArray()) {
            const QJsonArray arr = result.toArray();
            if (!arr.isEmpty()) loc = arr.first().toObject();
        }

        // LocationLink spells its fields targetUri/targetSelectionRange; a
        // plain Location uses uri/range.
        const QString targetUri = loc.contains("targetUri")
                                      ? loc.value("targetUri").toString()
                                      : loc.value("uri").toString();
        const QJsonObject range = loc.contains("targetSelectionRange")
                                      ? loc.value("targetSelectionRange").toObject()
                                      : loc.value("range").toObject();
        if (targetUri.isEmpty()) { cb(LspLocation{}); return; }

        const QJsonObject start = range.value("start").toObject();
        cb(LspLocation{targetUri, start.value("line").toInt(),
                       start.value("character").toInt()});
    },
    kInteractiveTimeoutMs);
}

namespace {

// Flatten one documentSymbol entry and its children into `out`.
//
// The pinned server returns a flat DocumentSymbol[] with no children, but the
// spec permits nesting and also permits the older SymbolInformation shape
// (which spells its span `location.range` and has no selectionRange). Handling
// all three keeps a conforming server change from emptying the outline.
void CollectSymbols(const QJsonArray& items, QVector<LspSymbol>& out) {
    for (const QJsonValue& v : items) {
        const QJsonObject o = v.toObject();
        const QString name = o.value("name").toString();
        if (name.isEmpty()) continue;

        LspSymbol sym;
        sym.name = name;
        sym.kind = o.value("kind").toInt();
        if (o.contains("location")) {  // SymbolInformation
            sym.range = RangeFromJson(o.value("location").toObject()
                                          .value("range").toObject());
            sym.selection = sym.range;
        } else {  // DocumentSymbol
            sym.range = RangeFromJson(o.value("range").toObject());
            sym.selection = o.contains("selectionRange")
                                ? RangeFromJson(o.value("selectionRange").toObject())
                                : sym.range;
        }
        out.append(sym);

        // Depth-first, so a nested symbol still lands after its parent and the
        // list stays in document order.
        if (o.contains("children")) {
            CollectSymbols(o.value("children").toArray(), out);
        }
    }
}

}  // namespace

void LspManager::requestSignatureHelp(EditorView* view, int pos,
                                      SignatureHelpCallback cb) {
    DocState* doc = docFor(view);
    if (!doc || !client_ || state_ != State::Ready) {
        if (cb) cb(QString(), -1);
        return;
    }

    const QString uri = UriFor(view);
    const int generation = doc->generation;

    // Flush the pending edit first, as completion does.
    //
    // This is NOT the flush upstream removed, and the two are easy to confuse.
    // Upstream took signature help out of the SERVER's re-analysis group, so a
    // request no longer forces a recompile. What this flushes is the CLIENT's
    // debounced didChange -- and the server finds the enclosing call by reading
    // its own copy of the document up to the cursor (`lsp_enclosing_call`), so
    // without it the server has never seen the call the user is typing and
    // answers null every time.
    //
    // Removing it was tried, on a misreading of "left the flush group", and the
    // space-trigger test caught it: the tip came back empty on a freshly typed
    // call. The per-space cost is now a text transfer and an index lookup
    // rather than a full analysis, which is what makes the auto-trigger
    // affordable at all.
    if (doc->debounce && doc->debounce->isActive()) {
        doc->debounce->stop();
        sendDidChange(uri);
    }

    client_->request("textDocument/signatureHelp", QJsonObject{
        {"textDocument", QJsonObject{{"uri", uri}}},
        {"position", positionJson(view, pos)},
    },
    [this, cb = std::move(cb), uri, generation](const QJsonValue& result, const LspError* err) {
        if (!cb) return;
        if (err) { cb(QString(), -1); return; }
        const auto it = docs_.constFind(uri);
        if (it == docs_.constEnd() || it->generation != generation) return;  // stale

        const QJsonObject help = result.toObject();
        const QJsonArray sigs = help.value("signatures").toArray();
        // "No signature here" is REPORTED, not dropped -- same reason
        // requestDefinition reports an invalid location: a caller cannot tell a
        // silent return from a request still in flight, and the control API
        // then has nothing to reply with but a timeout.
        if (sigs.isEmpty()) { cb(QString(), -1); return; }

        // `activeSignature` is optional and may be out of range on a server
        // that counts differently; clamp rather than index blindly.
        int active = help.value("activeSignature").toInt(0);
        if (active < 0 || active >= sigs.size()) active = 0;
        const QJsonObject sig = sigs.at(active).toObject();

        QString label = sig.value("label").toString();
        if (label.trimmed().isEmpty()) { cb(QString(), -1); return; }

        // A docstring, when the server sends one, goes under the signature --
        // the same shape showHover renders, so the two look alike on screen.
        const QJsonValue doc = sig.value("documentation");
        QString docText = doc.isString() ? doc.toString()
                                         : doc.toObject().value("value").toString();
        if (!docText.trimmed().isEmpty()) label += "\n" + docText.trimmed();

        const QJsonValue ap = sig.contains("activeParameter")
                                  ? sig.value("activeParameter")
                                  : help.value("activeParameter");
        cb(label, ap.isDouble() ? ap.toInt() : -1);
    },
    kInteractiveTimeoutMs);
}

void LspManager::requestWorkspaceSymbols(const QString& query,
                                         WorkspaceSymbolsCallback cb) {
    // No document and no generation guard: this is a workspace query, so there
    // is no buffer underneath it to go stale. It is also the one request here
    // that can be issued with no editor open at all.
    if (!client_ || state_ != State::Ready) {
        if (cb) cb({}, {});
        return;
    }

    client_->request("workspace/symbol", QJsonObject{{"query", query}},
    [cb = std::move(cb)](const QJsonValue& result, const LspError* err) {
        if (!cb) return;
        if (err) { cb({}, {}); return; }

        QVector<LspSpan> spans;
        QStringList names;
        for (const QJsonValue& v : result.toArray()) {
            const QJsonObject o = v.toObject();
            const QString name = o.value("name").toString();
            if (name.isEmpty()) continue;

            // SymbolInformation puts the span in `location`; the newer
            // WorkspaceSymbol may send `location` as a bare `{uri}` with the
            // range resolved later. Take the range when it is there and fall
            // back to the file's start, so a row still jumps to the right file.
            const QJsonObject loc = o.value("location").toObject();
            const QString uri = loc.value("uri").toString();
            if (uri.isEmpty()) continue;
            const QJsonObject range = loc.value("range").toObject();
            const LspRange r = range.isEmpty()
                                   ? LspRange{0, 0, 0, 0}
                                   : RangeFromJson(range);
            spans.append(LspSpan{uri, r});
            names.append(name);
        }
        cb(spans, names);
    },
    kInteractiveTimeoutMs);
}

void LspManager::requestDocumentSymbols(EditorView* view, SymbolsCallback cb) {
    DocState* doc = docFor(view);
    if (!doc || !client_ || state_ != State::Ready) {
        if (cb) cb({});
        return;
    }

    const QString uri = UriFor(view);
    const int generation = doc->generation;

    // An outline of text the server has not seen is an outline of the wrong
    // file. Same flush the other requests do.
    if (doc->debounce && doc->debounce->isActive()) {
        doc->debounce->stop();
        sendDidChange(uri);
    }

    client_->request("textDocument/documentSymbol", QJsonObject{
        {"textDocument", QJsonObject{{"uri", uri}}},
    },
    [this, cb = std::move(cb), uri, generation](const QJsonValue& result, const LspError* err) {
        if (!cb) return;
        const auto it = docs_.constFind(uri);
        if (it == docs_.constEnd() || it->generation != generation) return;  // stale
        if (err) { cb({}); return; }

        QVector<LspSymbol> symbols;
        CollectSymbols(result.toArray(), symbols);
        cb(symbols);
    },
    kInteractiveTimeoutMs);
}

void LspManager::requestDocumentHighlights(EditorView* view, int pos,
                                           HighlightsCallback cb) {
    DocState* doc = docFor(view);
    if (!doc || !client_ || state_ != State::Ready) {
        if (cb) cb({});
        return;
    }

    const QString uri = UriFor(view);
    const int generation = doc->generation;

    if (doc->debounce && doc->debounce->isActive()) {
        doc->debounce->stop();
        sendDidChange(uri);
    }

    client_->request("textDocument/documentHighlight", QJsonObject{
        {"textDocument", QJsonObject{{"uri", uri}}},
        {"position", positionJson(view, pos)},
    },
    [this, cb = std::move(cb), uri, generation](const QJsonValue& result, const LspError* err) {
        if (!cb) return;
        const auto it = docs_.constFind(uri);
        if (it == docs_.constEnd() || it->generation != generation) return;  // stale
        if (err) { cb({}); return; }

        QVector<LspRange> ranges;
        for (const QJsonValue& v : result.toArray()) {
            // `kind` (Text/Read/Write) is deliberately dropped: an occurrence
            // is an occurrence, and painting reads and writes differently is a
            // second decoration nobody asked for.
            ranges.append(RangeFromJson(v.toObject().value("range").toObject()));
        }
        cb(ranges);
    },
    kInteractiveTimeoutMs);
}

void LspManager::requestReferences(EditorView* view, int pos, bool includeDeclaration,
                                   ReferencesCallback cb) {
    DocState* doc = docFor(view);
    if (!doc || !client_ || state_ != State::Ready) {
        if (cb) cb({});
        return;
    }

    const QString uri = UriFor(view);
    const int generation = doc->generation;

    if (doc->debounce && doc->debounce->isActive()) {
        doc->debounce->stop();
        sendDidChange(uri);
    }

    client_->request("textDocument/references", QJsonObject{
        {"textDocument", QJsonObject{{"uri", uri}}},
        {"position", positionJson(view, pos)},
        {"context", QJsonObject{{"includeDeclaration", includeDeclaration}}},
    },
    [this, cb = std::move(cb), uri, generation](const QJsonValue& result, const LspError* err) {
        if (!cb) return;
        const auto it = docs_.constFind(uri);
        if (it == docs_.constEnd() || it->generation != generation) return;  // stale
        if (err) { cb({}); return; }

        QVector<LspSpan> spans;
        for (const QJsonValue& v : result.toArray()) {
            const QJsonObject o = v.toObject();
            spans.append(LspSpan{o.value("uri").toString(),
                                 RangeFromJson(o.value("range").toObject())});
        }
        cb(spans);
    },
    kInteractiveTimeoutMs);
}

void LspManager::requestPrepareRename(EditorView* view, int pos, PrepareRenameCallback cb) {
    DocState* doc = docFor(view);
    if (!doc || !client_ || state_ != State::Ready) {
        if (cb) cb(PrepareRename{});
        return;
    }

    const QString uri = UriFor(view);
    const int generation = doc->generation;

    if (doc->debounce && doc->debounce->isActive()) {
        doc->debounce->stop();
        sendDidChange(uri);
    }

    client_->request("textDocument/prepareRename", QJsonObject{
        {"textDocument", QJsonObject{{"uri", uri}}},
        {"position", positionJson(view, pos)},
    },
    [this, cb = std::move(cb), uri, generation](const QJsonValue& result, const LspError* err) {
        if (!cb) return;
        const auto it = docs_.constFind(uri);
        if (it == docs_.constEnd() || it->generation != generation) return;  // stale

        PrepareRename out;
        if (err) {
            // Seven of the server's eight refusals land here, each with a
            // message written to be read by a person. Carried through verbatim:
            // paraphrasing "cannot rename a macro-introduced binding" loses the
            // only thing that tells the user what to do instead.
            out.refusal = err->message.isEmpty()
                              ? QStringLiteral("rename was refused")
                              : err->message;
            cb(out);
            return;
        }
        // A null result is the eighth case: nothing at this position at all.
        if (!result.isObject()) { cb(out); return; }

        const QJsonObject o = result.toObject();
        // The spec also allows {range, placeholder} to be a bare Range, and
        // allows {defaultBehavior: true}. The pinned server sends the first
        // form; accept a bare range too rather than breaking on a conforming
        // change.
        if (o.contains("range")) {
            out.range = RangeFromJson(o.value("range").toObject());
            out.placeholder = o.value("placeholder").toString();
        } else if (o.contains("start") && o.contains("end")) {
            out.range = RangeFromJson(o);
        } else {
            cb(out);
            return;
        }
        out.renameable = true;
        cb(out);
    },
    kRenameTimeoutMs);
}

void LspManager::requestRename(EditorView* view, int pos, const QString& newName,
                               RenameCallback cb) {
    DocState* doc = docFor(view);
    if (!doc || !client_ || state_ != State::Ready) {
        if (cb) cb({}, QStringLiteral("language server is not ready"));
        return;
    }

    const QString uri = UriFor(view);
    const int generation = doc->generation;

    if (doc->debounce && doc->debounce->isActive()) {
        doc->debounce->stop();
        sendDidChange(uri);
    }

    client_->request("textDocument/rename", QJsonObject{
        {"textDocument", QJsonObject{{"uri", uri}}},
        {"position", positionJson(view, pos)},
        {"newName", newName},
    },
    [this, cb = std::move(cb), uri, generation](const QJsonValue& result, const LspError* err) {
        if (!cb) return;
        const auto it = docs_.constFind(uri);
        // Staleness matters more here than anywhere else: applying an edit
        // computed against text the user has since changed corrupts the file.
        if (it == docs_.constEnd() || it->generation != generation) {
            cb({}, QStringLiteral("the document changed while renaming; nothing was applied"));
            return;
        }
        if (err) {
            cb({}, err->message.isEmpty() ? QStringLiteral("rename was refused")
                                          : err->message);
            return;
        }

        // The pinned server always sends `changes`. `documentChanges` is the
        // spec's richer form and is parsed too, so a conforming server change
        // does not silently produce an empty edit — which would read as
        // "nothing to rename" rather than as a client that stopped working.
        WorkspaceEdit edit;
        const QJsonObject root = result.toObject();
        const QJsonObject changes = root.value("changes").toObject();
        for (auto it2 = changes.constBegin(); it2 != changes.constEnd(); ++it2) {
            QVector<LspTextEdit> edits;
            for (const QJsonValue& v : it2.value().toArray()) {
                const QJsonObject e = v.toObject();
                edits.append(LspTextEdit{RangeFromJson(e.value("range").toObject()),
                                         e.value("newText").toString()});
            }
            if (!edits.isEmpty()) edit.insert(it2.key(), edits);
        }
        for (const QJsonValue& v : root.value("documentChanges").toArray()) {
            const QJsonObject entry = v.toObject();
            const QString docUri =
                entry.value("textDocument").toObject().value("uri").toString();
            if (docUri.isEmpty()) continue;  // a create/rename/delete op, not an edit
            QVector<LspTextEdit> edits = edit.value(docUri);
            for (const QJsonValue& ev : entry.value("edits").toArray()) {
                const QJsonObject e = ev.toObject();
                edits.append(LspTextEdit{RangeFromJson(e.value("range").toObject()),
                                         e.value("newText").toString()});
            }
            if (!edits.isEmpty()) edit.insert(docUri, edits);
        }
        cb(edit, QString());
    },
    kRenameTimeoutMs);
}

bool LspManager::isStdlibPath(const QString& path) const {
    // stdlibDir_ is only set once a server has been started; without one there
    // is no bundle to be inside of, so nothing is read-only.
    if (stdlibDir_.isEmpty() || path.isEmpty()) return false;
    const QString root = QDir(stdlibDir_).absolutePath() + QLatin1Char('/');
    return QFileInfo(path).absoluteFilePath().startsWith(root);
}

void LspManager::restart() {
    if (state_ == State::Disabled) return;
    restartsRemaining_ = 3;
    lastError_.clear();
    for (auto it = docs_.begin(); it != docs_.end(); ++it) it->openOnServer = false;
    // Diagnostics from the old process describe a buffer the new one hasn't
    // seen; drop them so nothing stale is left painted.
    const QList<QString> staleUris = diagnostics_.keys();
    diagnostics_.clear();

    restarting_ = true;
    if (client_) client_->stop();
    restarting_ = false;

    state_ = State::Stopped;
    ensureStarted();
    emit stateChanged();
    for (const QString& uri : staleUris) emit diagnosticsUpdated(uri);
}

void LspManager::shutdown() {
    if (!client_) return;
    if (state_ == State::Ready) {
        client_->request("shutdown", QJsonObject{}, nullptr, 200);
        client_->notify("exit", QJsonObject{});
    }
    client_->stop();
    setState(State::Stopped);
}

}
