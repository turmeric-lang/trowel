#include "editor/dialect.h"
#include "control/control_handlers.h"

#include "control/control_connection.h"
#include "app/main_window.h"
#include "app/window_manager.h"
#include "debug/breakpoint_model.h"
#include "debug/debug_session.h"
#include "editor/editor_view.h"
#include "lsp/lsp_manager.h"
#include "repl/repl_session.h"
#include "repl/pty_session.h"
#include "repl/terminal_view.h"
#include "repl/run_buffer.h"

#include <ScintillaEdit.h>

#include <QAction>
#include <QApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QHash>
#include <QMimeData>
#include <QUrl>
#include <QJsonArray>
#include <QKeyEvent>
#include <QMenu>
#include <QMenuBar>
#include <QMetaObject>
#include <QPixmap>
#include <QPointer>
#include <QRegularExpression>
#include <QSplitter>
#include <QTimer>

#include <memory>

namespace trowel::control {

namespace {

// Convenience: reply with a simple {"ok": true} object when there's nothing
// meaningful to return.
inline QJsonObject Ok() { QJsonObject o; o["ok"] = true; return o; }

inline void ReplyErr(const Reply& reply, const QString& code, const QString& msg) {
    ControlError e{code, msg};
    reply({}, &e);
}

// Resolve the active editor, or reply with an error and return null.
//
// MainWindow::editorView() is null whenever the active tab is not an editor —
// a directory browser or the preferences pane. Editor commands must check;
// dereferencing it crashes the app.
EditorView* RequireEditor(MainWindow* w, const Reply& reply) {
    EditorView* e = w->editorView();
    if (!e) ReplyErr(reply, "no_editor", "the active tab is not an editor");
    return e;
}

Qt::KeyboardModifiers ParseMods(const QJsonArray& mods) {
    Qt::KeyboardModifiers m = Qt::NoModifier;
    for (const QJsonValue& v : mods) {
        const QString s = v.toString().toLower();
        if (s == "ctrl" || s == "control") m |= Qt::ControlModifier;
        else if (s == "shift") m |= Qt::ShiftModifier;
        else if (s == "alt") m |= Qt::AltModifier;
        else if (s == "meta" || s == "cmd") m |= Qt::MetaModifier;
    }
    return m;
}

// Very small subset of Qt::Key names. Extend as smoke tests need.
static const QHash<QString, int>& KeyTable() {
    static const QHash<QString, int> t = {
        {"return", Qt::Key_Return}, {"enter", Qt::Key_Return},
        {"backspace", Qt::Key_Backspace}, {"tab", Qt::Key_Tab},
        {"escape", Qt::Key_Escape}, {"esc", Qt::Key_Escape},
        {"space", Qt::Key_Space},
        {"up", Qt::Key_Up}, {"down", Qt::Key_Down},
        {"left", Qt::Key_Left}, {"right", Qt::Key_Right},
        {"home", Qt::Key_Home}, {"end", Qt::Key_End},
        {"delete", Qt::Key_Delete}, {"del", Qt::Key_Delete},
        {"pageup", Qt::Key_PageUp}, {"pagedown", Qt::Key_PageDown},
        {"insert", Qt::Key_Insert},
    };
    return t;
}

bool ParseKey(const QString& name, int& outKey, QString& outText) {
    const QString lower = name.toLower();
    auto it = KeyTable().find(lower);
    if (it != KeyTable().end()) {
        outKey = it.value();
        // Provide sensible text for keys that produce characters.
        switch (outKey) {
            case Qt::Key_Return: outText = "\r"; break;
            case Qt::Key_Tab: outText = "\t"; break;
            case Qt::Key_Space: outText = " "; break;
            default: outText.clear();
        }
        return true;
    }
    if (name.size() == 1) {
        const QChar c = name.at(0);
        outText = QString(c);
        outKey = c.toUpper().unicode();
        return true;
    }
    return false;
}

void SendKeyToWidget(QWidget* target, int key, Qt::KeyboardModifiers mods, const QString& text) {
    QKeyEvent press(QEvent::KeyPress, key, mods, text);
    QApplication::sendEvent(target, &press);
    QKeyEvent release(QEvent::KeyRelease, key, mods, text);
    QApplication::sendEvent(target, &release);
}

QAction* FindMenuAction(QMenuBar* bar, const QStringList& path) {
    if (!bar || path.isEmpty()) return nullptr;
    auto stripAmp = [](QString s) { return s.remove(QChar('&')); };

    QList<QAction*> current = bar->actions();
    QAction* found = nullptr;
    for (int i = 0; i < path.size(); ++i) {
        const QString target = path[i];
        found = nullptr;
        for (QAction* a : current) {
            if (stripAmp(a->text()).compare(target, Qt::CaseInsensitive) == 0) {
                found = a;
                break;
            }
        }
        if (!found) return nullptr;
        if (i + 1 < path.size()) {
            if (!found->menu()) return nullptr;
            current = found->menu()->actions();
        }
    }
    return found;
}

// Handler wiring ---------------------------------------------------------

struct WaitCtx {
    bool done = false;
    QMetaObject::Connection conn;
    QMetaObject::Connection conn2;
    QTimer* timer = nullptr;
};

void ArmTimeout(std::shared_ptr<WaitCtx> ctx, QObject* parent, int ms, Reply reply) {
    ctx->timer = new QTimer(parent);
    ctx->timer->setSingleShot(true);
    QObject::connect(ctx->timer, &QTimer::timeout, parent, [ctx, reply]() {
        if (ctx->done) return;
        ctx->done = true;
        QObject::disconnect(ctx->conn);
        QObject::disconnect(ctx->conn2);
        ControlError e{"timeout", "wait timed out"};
        reply({}, &e);
    });
    ctx->timer->start(ms);
}

void HandleWindowFocus(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    const QString pane = args.value("pane").toString();
    if (pane == "editor") {
        EditorView* e = RequireEditor(w, reply);
        if (!e) return;
        e->setFocus();
        reply(Ok(), nullptr);
        return;
    }
    if (pane == "terminal" || pane == "repl") { w->terminalView()->setFocus(); reply(Ok(), nullptr); return; }
    ReplyErr(reply, "bad_pane", QString("unknown pane: %1").arg(pane));
}

void HandleWindowActivate(MainWindow* w, const QJsonObject&, const Reply& reply) {
    // Mirror TrowelApplication::openFile()'s raise sequence so a forwarded
    // single-instance request brings the existing window to the front.
    w->show();
    w->raise();
    w->activateWindow();
    reply(Ok(), nullptr);
}

void HandleWindowNew(WindowManager* wm, const QJsonObject&, const Reply& reply) {
    wm->newWindow();
    QJsonObject o;
    o["count"] = wm->count();
    reply(o, nullptr);
}

void HandleWindowList(WindowManager* wm, const QJsonObject&, const Reply& reply) {
    QJsonArray arr;
    for (MainWindow* win : wm->windows()) {
        QJsonObject o;
        o["title"] = win->windowTitle();
        o["active"] = (win == wm->activeWindow());
        EditorView* e = win->editorView();
        o["file_path"] = e ? e->filePath() : QString();
        QJsonArray tabs;
        for (const QString& p : win->tabPaths()) tabs.append(p);
        o["tabs"] = tabs;
        o["tab_count"] = tabs.size();
        arr.append(o);
    }
    QJsonObject o;
    o["count"] = arr.size();
    o["windows"] = arr;
    reply(o, nullptr);
}

void HandleWindowDrop(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    QList<QUrl> urls;
    for (const QJsonValue& v : args.value("paths").toArray()) {
        const QString p = v.toString();
        if (!p.isEmpty()) urls << QUrl::fromLocalFile(p);
    }
    if (urls.isEmpty()) { ReplyErr(reply, "bad_args", "missing `paths`"); return; }

    // Synthesize a real drag/drop rather than calling openDropped() directly,
    // so the window's dragEnterEvent/dropEvent are on the tested path too.
    QMimeData mime;
    mime.setUrls(urls);
    const QPoint pos(10, 10);
    QDragEnterEvent enter(pos, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w, &enter);
    if (!enter.isAccepted()) { ReplyErr(reply, "drop_rejected", "window refused the drag"); return; }
    QDropEvent drop(QPointF(pos), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w, &drop);

    QJsonObject o;
    o["accepted"] = drop.isAccepted();
    reply(o, nullptr);
}

void HandleWindowGeometry(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    // Optionally *set* first, then report. Read-only until now, which quietly
    // ignored a `{"width": ...}` and made it impossible to test that a resize
    // survives a restart — the exact thing that turned out to be broken.
    if (args.contains("width") || args.contains("height")) {
        w->resize(args.value("width").toInt(w->width()),
                  args.value("height").toInt(w->height()));
    }
    if (args.contains("x") || args.contains("y")) {
        w->move(args.value("x").toInt(w->x()), args.value("y").toInt(w->y()));
    }
    QJsonObject o;
    const QRect g = w->geometry();
    o["x"] = g.x(); o["y"] = g.y(); o["w"] = g.width(); o["h"] = g.height();
    const auto sizes = w->splitter()->sizes();
    QJsonArray a; for (int s : sizes) a.append(s);
    o["splitter"] = a;
    o["title"] = w->windowTitle();
    // Not an error when the active tab is a directory/preferences pane —
    // geometry is still meaningful, so report an empty path.
    EditorView* ge = w->editorView();
    o["file_path"] = ge ? ge->filePath() : QString();
    reply(o, nullptr);
}

void HandleWindowSetSplitter(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    if (args.contains("sizes")) {
        QList<int> sizes;
        for (const QJsonValue& v : args.value("sizes").toArray()) sizes.append(v.toInt());
        w->splitter()->setSizes(sizes);
        reply(Ok(), nullptr);
        return;
    }
    if (args.contains("pos")) {
        const int pos = args.value("pos").toInt();
        const int total = w->splitter()->orientation() == Qt::Horizontal
                          ? w->splitter()->width() : w->splitter()->height();
        w->splitter()->setSizes({pos, qMax(0, total - pos)});
        reply(Ok(), nullptr);
        return;
    }
    ReplyErr(reply, "bad_args", "expected `pos` or `sizes`");
}

void HandleMenuInvoke(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    QStringList path;
    for (const QJsonValue& v : args.value("path").toArray()) path.append(v.toString());
    QAction* a = FindMenuAction(w->menuBar(), path);
    if (!a) { ReplyErr(reply, "no_action", "menu action not found"); return; }
    if (!a->isEnabled()) { ReplyErr(reply, "action_disabled", "menu action is disabled"); return; }
    a->trigger();
    reply(Ok(), nullptr);
}

void HandleWindowScreenshot(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    // Render the window to a PNG. Qt's own `grab()` rather than a platform
    // screen capture: it needs no screen-recording permission, it captures the
    // window even when it is not frontmost, and it works under the offscreen
    // platform plugin the smoke tests run with — so a UI change can be looked
    // at rather than only asserted about. Shipping UI without ever seeing it
    // is how it comes to look the way it does.
    const QString path = args.value("path").toString();
    if (path.isEmpty()) { ReplyErr(reply, "bad_args", "missing `path`"); return; }
    const QPixmap shot = w->grab();
    if (shot.isNull() || !shot.save(path, "PNG")) {
        ReplyErr(reply, "capture_failed", QString("could not write %1").arg(path));
        return;
    }
    QJsonObject o;
    o["path"] = path;
    o["width"] = shot.width();
    o["height"] = shot.height();
    reply(o, nullptr);
}

void HandleEditorOpen(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    const QString path = args.value("path").toString();
    if (path.isEmpty()) { ReplyErr(reply, "bad_args", "missing `path`"); return; }
    if (!w->openPath(path)) { ReplyErr(reply, "open_failed", QString("could not open %1").arg(path)); return; }
    reply(Ok(), nullptr);
}

void HandleEditorSave(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    EditorView* e = RequireEditor(w, reply);
    if (!e) return;
    const QString path = args.value("path").toString();
    const bool ok = path.isEmpty() ? e->saveCurrent() : e->saveFile(path);
    if (!ok) { ReplyErr(reply, "save_failed", "save failed"); return; }
    reply(Ok(), nullptr);
}

void HandleEditorSetText(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    EditorView* e = RequireEditor(w, reply);
    if (!e) return;
    e->setText(args.value("text").toString().toUtf8());
    reply(Ok(), nullptr);
}

void HandleEditorType(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    const QString text = args.value("text").toString();
    EditorView* e = RequireEditor(w, reply);
    if (!e) return;
    QWidget* target = e->sciWidget();
    if (!target) { ReplyErr(reply, "no_target", "editor unavailable"); return; }
    for (QChar c : text) {
        int key;
        if (c == QChar('\n') || c == QChar('\r')) key = Qt::Key_Return;
        else if (c == QChar('\t')) key = Qt::Key_Tab;
        else key = c.toUpper().unicode();
        SendKeyToWidget(target, key, Qt::NoModifier, QString(c));
    }
    reply(Ok(), nullptr);
}

void HandleEditorPress(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    int key = 0; QString text;
    if (!ParseKey(args.value("key").toString(), key, text)) {
        ReplyErr(reply, "bad_key", "unknown key name"); return;
    }
    const Qt::KeyboardModifiers mods = ParseMods(args.value("mods").toArray());
    // Modified keys typically don't produce text.
    if (mods && mods != Qt::ShiftModifier) text.clear();
    EditorView* pe = RequireEditor(w, reply);
    if (!pe) return;
    SendKeyToWidget(pe->sciWidget(), key, mods, text);
    reply(Ok(), nullptr);
}

void HandleEditorGetText(MainWindow* w, const QJsonObject&, const Reply& reply) {
    EditorView* e = RequireEditor(w, reply);
    if (!e) return;
    const QByteArray raw = e->text();
    QJsonObject o;
    if (raw.size() > 4 * 1024 * 1024) {
        o["text_truncated"] = true;
        o["size"] = raw.size();
    } else {
        o["text"] = QString::fromUtf8(raw);
    }
    o["modified"] = e->isModified();
    o["path"] = e->filePath();
    reply(o, nullptr);
}

void HandleEditorGetCursor(MainWindow* w, const QJsonObject&, const Reply& reply) {
    EditorView* e = RequireEditor(w, reply);
    if (!e) return;
    const int pos = e->cursorPos();
    const auto [line, col] = e->lineColFromPos(pos);
    QJsonObject o;
    o["pos"] = pos;
    o["line"] = line;
    o["col"] = col;
    o["anchor"] = e->anchorPos();
    const auto [s, en] = e->selectionRange();
    QJsonArray sel; sel.append(s); sel.append(en);
    o["selection"] = sel;
    reply(o, nullptr);
}

void HandleEditorSetCursor(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    EditorView* e = RequireEditor(w, reply);
    if (!e) return;
    int pos = -1;
    if (args.contains("pos")) pos = args.value("pos").toInt();
    else if (args.contains("line")) {
        const int line = args.value("line").toInt();
        const int col = args.value("col").toInt(0);
        pos = e->posFromLineCol(line, col);
    }
    if (pos < 0) { ReplyErr(reply, "bad_args", "expected `pos` or `line`"); return; }
    e->setCursorPos(pos);
    reply(Ok(), nullptr);
}

void HandleEditorGetSelection(MainWindow* w, const QJsonObject&, const Reply& reply) {
    EditorView* e = RequireEditor(w, reply);
    if (!e) return;
    const auto [s, en] = e->selectionRange();
    QJsonObject o;
    o["start"] = s; o["end"] = en;
    o["text"] = QString::fromUtf8(e->textInRange(s, en));
    reply(o, nullptr);
}

void HandleEditorSetSelection(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    const int start = args.value("start").toInt();
    const int end = args.value("end").toInt();
    EditorView* e = RequireEditor(w, reply);
    if (!e) return;
    e->setSelection(start, end);
    reply(Ok(), nullptr);
}

void HandleEditorGetStyleAt(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    const int pos = args.value("pos").toInt();
    // Styling is otherwise lazy (driven by painting), so force the lexer to run
    // through the requested position before reading the stored style byte.
    EditorView* e = RequireEditor(w, reply);
    if (!e) return;
    if (ScintillaEdit* sci = e->sciWidget()) {
        sci->colourise(0, pos + 1);
    }
    QJsonObject o;
    o["style"] = e->styleAt(pos);
    reply(o, nullptr);
}

// --- Language server -----------------------------------------------------

QJsonObject DiagnosticsToJson(const QVector<LspDiagnostic>& diagnostics) {
    QJsonArray arr;
    for (const LspDiagnostic& d : diagnostics) {
        arr.append(QJsonObject{
            {"severity", d.severity},
            {"start_line", d.startLine},
            {"start_char", d.startChar},
            {"end_line", d.endLine},
            {"end_char", d.endChar},
            {"message", d.message},
            {"source", d.source},
        });
    }
    QJsonObject o;
    o["diagnostics"] = arr;
    o["count"] = arr.size();
    return o;
}

const char* LspStateName(LspManager::State s) {
    switch (s) {
        case LspManager::State::Disabled: return "disabled";
        case LspManager::State::Stopped:  return "stopped";
        case LspManager::State::Starting: return "starting";
        case LspManager::State::Ready:    return "ready";
        case LspManager::State::Failed:   return "failed";
    }
    return "unknown";
}

void HandleLspStatus(MainWindow*, const QJsonObject&, const Reply& reply) {
    LspManager* lsp = LspManager::instance();
    QJsonObject o;
    o["state"] = LspStateName(lsp->state());
    o["error"] = lsp->lastError();
    o["server_path"] = lsp->serverPath();
    o["enabled"] = LspManager::enabledInSettings();
    reply(o, nullptr);
}

void HandleLspDiagnostics(MainWindow* w, const QJsonObject&, const Reply& reply) {
    EditorView* e = RequireEditor(w, reply);
    if (!e) return;
    reply(DiagnosticsToJson(e->diagnostics()), nullptr);
}

// Read decorations back out of Scintilla rather than out of our own model.
// This is what proves setDiagnostics actually painted: the manager holding a
// diagnostic and the editor showing a squiggle are different claims.
void HandleLspDecorations(MainWindow* w, const QJsonObject&, const Reply& reply) {
    EditorView* e = RequireEditor(w, reply);
    if (!e) return;
    ScintillaEdit* sci = e->sciWidget();
    if (!sci) { ReplyErr(reply, "no_editor", "editor has no Scintilla widget"); return; }

    const int docEnd = static_cast<int>(sci->textLength());

    auto rangesFor = [&](int indicator) {
        QJsonArray out;
        int pos = 0;
        while (pos < docEnd) {
            const int end = static_cast<int>(sci->indicatorEnd(indicator, pos));
            if (end <= pos) break;  // no further boundaries; also guards progress
            if (sci->indicatorValueAt(indicator, pos)) {
                out.append(QJsonObject{{"start", pos}, {"end", end}});
            }
            pos = end;
        }
        return out;
    };

    QJsonArray errorMarkerLines;
    QJsonArray warningMarkerLines;
    const int lines = e->lineCount();
    for (int line = 0; line < lines; ++line) {
        const int mask = static_cast<int>(sci->markerGet(line));
        if (mask & (1 << diag::kErrorMarker)) errorMarkerLines.append(line);
        if (mask & (1 << diag::kWarningMarker)) warningMarkerLines.append(line);
    }

    QJsonObject o;
    o["error_ranges"] = rangesFor(diag::kErrorIndicator);
    o["warning_ranges"] = rangesFor(diag::kWarningIndicator);
    o["occurrence_ranges"] = rangesFor(occurrence::kIndicator);
    o["bracket_guide_ranges"] = rangesFor(bracketguide::kIndicator);
    // Asserting on a computed pair without asserting on the painted range
    // proves nothing, so the colour is read back off the widget too.
    o["bracket_guide_color"] =
        static_cast<int>(sci->indicFore(bracketguide::kIndicator));
    const auto [guideStart, guideEnd] = e->bracketGuideSpan();
    o["bracket_guide_span"] = QJsonObject{{"start", guideStart}, {"end", guideEnd}};
    const EditorView::GuideLine line = e->bracketGuideLine();
    o["bracket_guide_vertical"] = QJsonObject{
        {"visible", line.visible}, {"x", line.x},
        {"top", line.top}, {"bottom", line.bottom}};
    const EditorView::GuideLine bar = e->bracketGutterBar();
    o["bracket_guide_gutter"] = QJsonObject{
        {"visible", bar.visible}, {"x", bar.x},
        {"top", bar.top}, {"bottom", bar.bottom}};
    o["error_marker_lines"] = errorMarkerLines;
    o["warning_marker_lines"] = warningMarkerLines;
    reply(o, nullptr);
}

void HandleLspRestart(MainWindow*, const QJsonObject&, const Reply& reply) {
    LspManager::instance()->restart();
    reply(Ok(), nullptr);
}

void HandleLspCompletions(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    EditorView* e = RequireEditor(w, reply);
    if (!e) return;
    const int timeout = args.value("timeout_ms").toInt(5000);
    const int pos = args.contains("pos") ? args.value("pos").toInt() : e->cursorPos();

    // The manager drops its callback entirely on error or staleness, so the
    // timeout is what guarantees the control connection gets an answer.
    auto ctx = std::make_shared<WaitCtx>();
    LspManager::instance()->requestCompletion(e, pos, [ctx, reply](const QStringList& labels) {
        if (ctx->done) return;
        ctx->done = true;
        if (ctx->timer) ctx->timer->stop();
        QJsonArray arr;
        for (const QString& label : labels) arr.append(label);
        QJsonObject o;
        o["labels"] = arr;
        o["count"] = arr.size();
        reply(o, nullptr);
    });
    ArmTimeout(ctx, w, timeout, reply);
}

void HandleLspHover(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    EditorView* e = RequireEditor(w, reply);
    if (!e) return;
    const int timeout = args.value("timeout_ms").toInt(5000);
    const int pos = args.contains("pos") ? args.value("pos").toInt() : e->cursorPos();

    auto ctx = std::make_shared<WaitCtx>();
    LspManager::instance()->requestHover(e, pos, [ctx, reply](const QString& text) {
        if (ctx->done) return;
        ctx->done = true;
        if (ctx->timer) ctx->timer->stop();
        QJsonObject o;
        o["text"] = text;
        reply(o, nullptr);
    });
    ArmTimeout(ctx, w, timeout, reply);
}

void HandleLspDefinition(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    EditorView* e = RequireEditor(w, reply);
    if (!e) return;
    const int timeout = args.value("timeout_ms").toInt(5000);
    const int pos = args.contains("pos") ? args.value("pos").toInt() : e->cursorPos();

    // Reports the raw server answer rather than performing the jump, so a test
    // can tell "the server had no definition" apart from "the window failed to
    // open the tab". Driving the actual navigation is menu.invoke's job.
    auto ctx = std::make_shared<WaitCtx>();
    LspManager::instance()->requestDefinition(e, pos, [ctx, reply](const LspLocation& loc) {
        if (ctx->done) return;
        ctx->done = true;
        if (ctx->timer) ctx->timer->stop();
        QJsonObject o;
        if (!loc.isValid()) {
            o["location"] = QJsonValue::Null;
        } else {
            o["location"] = QJsonObject{
                {"uri", loc.uri},
                {"path", LspManager::PathForUri(loc.uri)},
                {"line", loc.line},
                {"character", loc.character},
            };
        }
        reply(o, nullptr);
    });
    ArmTimeout(ctx, w, timeout, reply);
}

void HandleLspHighlights(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    EditorView* e = RequireEditor(w, reply);
    if (!e) return;
    const int timeout = args.value("timeout_ms").toInt(5000);
    const int pos = args.contains("pos") ? args.value("pos").toInt() : e->cursorPos();

    auto ctx = std::make_shared<WaitCtx>();
    LspManager::instance()->requestDocumentHighlights(
        e, pos, [ctx, reply, e](const QVector<LspRange>& ranges) {
            if (ctx->done) return;
            ctx->done = true;
            if (ctx->timer) ctx->timer->stop();

            // Paint here as well as report. The debounce would get there on its
            // own, but a test that asserts on indicator 10 immediately after
            // this call must not race it.
            e->setOccurrences(ranges);

            QJsonArray arr;
            for (const LspRange& r : ranges) {
                arr.append(QJsonObject{
                    {"start", e->posFromLineCol(r.startLine, r.startCharacter)},
                    {"end", e->posFromLineCol(r.endLine, r.endCharacter)},
                    {"start_line", r.startLine},
                    {"start_character", r.startCharacter},
                });
            }
            QJsonObject o;
            o["ranges"] = arr;
            o["count"] = arr.size();
            reply(o, nullptr);
        });
    ArmTimeout(ctx, w, timeout, reply);
}

void HandleLspSymbols(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    const int timeout = args.value("timeout_ms").toInt(5000);
    EditorView* e = RequireEditor(w, reply);
    if (!e) return;

    // Connected before the request, same as nav.goto_definition: some of the
    // outline's states resolve synchronously (unsaved buffer, server not
    // ready) and would otherwise be missed entirely.
    auto ctx = std::make_shared<WaitCtx>();
    ctx->conn = QObject::connect(w, &MainWindow::outlineReady, w,
        [ctx, reply, e](const QVector<LspSymbol>& symbols, const QString& reason) {
            if (ctx->done) return;
            ctx->done = true;
            if (ctx->timer) ctx->timer->stop();
            QObject::disconnect(ctx->conn);

            const int current = e->symbolIndexAtCaret(symbols);
            QJsonArray arr;
            for (int i = 0; i < symbols.size(); ++i) {
                const LspSymbol& s = symbols.at(i);
                arr.append(QJsonObject{
                    {"name", s.name},
                    {"kind", s.kind},
                    {"kind_label", LspSymbolKindLabel(s.kind)},
                    {"line", s.selection.startLine},
                    {"character", s.selection.startCharacter},
                    {"current", i == current},
                });
            }
            QJsonObject o;
            o["symbols"] = arr;
            o["count"] = arr.size();
            // Empty on success. Lets a test tell "this file defines nothing"
            // apart from "this file did not compile" and from "no server".
            o["reason"] = reason;
            reply(o, nullptr);
        });
    w->showOutline();
    ArmTimeout(ctx, w, timeout, reply);
}

void HandleLspReferences(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    const int timeout = args.value("timeout_ms").toInt(5000);
    EditorView* e = RequireEditor(w, reply);
    if (!e) return;
    if (args.contains("pos")) e->setCursorPos(args.value("pos").toInt());

    auto ctx = std::make_shared<WaitCtx>();
    ctx->conn = QObject::connect(w, &MainWindow::referencesReady, w,
        [ctx, reply](const QVector<LspSpan>& spans, const QString& reason) {
            if (ctx->done) return;
            ctx->done = true;
            if (ctx->timer) ctx->timer->stop();
            QObject::disconnect(ctx->conn);
            QJsonArray arr;
            for (const LspSpan& s : spans) {
                arr.append(QJsonObject{
                    {"uri", s.uri},
                    {"path", LspManager::PathForUri(s.uri)},
                    {"line", s.range.startLine},
                    {"character", s.range.startCharacter},
                    {"end_line", s.range.endLine},
                    {"end_character", s.range.endCharacter},
                });
            }
            QJsonObject o;
            o["references"] = arr;
            o["count"] = arr.size();
            o["reason"] = reason;
            reply(o, nullptr);
        });
    w->findReferences();
    ArmTimeout(ctx, w, timeout, reply);
}

void HandleLspWorkspaceSymbols(MainWindow* w, const QJsonObject& args,
                               const Reply& reply) {
    const int timeout = args.value("timeout_ms").toInt(8000);
    const QString query = args.value("query").toString();

    auto ctx = std::make_shared<WaitCtx>();
    ctx->conn = QObject::connect(w, &MainWindow::workspaceSymbolsReady, w,
        [ctx, reply](const QVector<LspSpan>& spans, const QString& reason) {
            if (ctx->done) return;
            ctx->done = true;
            if (ctx->timer) ctx->timer->stop();
            QObject::disconnect(ctx->conn);
            QJsonArray arr;
            for (const LspSpan& s : spans) {
                arr.append(QJsonObject{
                    {"uri", s.uri},
                    {"path", LspManager::PathForUri(s.uri)},
                    {"line", s.range.startLine},
                    {"character", s.range.startCharacter},
                });
            }
            QJsonObject o;
            o["symbols"] = arr;
            o["count"] = arr.size();
            o["reason"] = reason;
            reply(o, nullptr);
        });
    // The no-dialog half: a modal would hang the control socket.
    w->findSymbolInProjectFor(query);
    ArmTimeout(ctx, w, timeout, reply);
}

// Signature help.
//
// Goes to the manager directly rather than through EditorView's signal, the way
// lsp.hover does, so the reply carries the server's answer. `tip_active` is
// reported alongside it because the two are different claims -- the manager
// having a signature and the editor showing a call tip -- and under offscreen
// Qt the tip is a window that may never activate, so a test that asserted only
// the tip would be asserting the platform.
void HandleLspSignatureHelp(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    EditorView* e = RequireEditor(w, reply);
    if (!e) return;
    const int timeout = args.value("timeout_ms").toInt(8000);
    const int pos = args.contains("pos") ? args.value("pos").toInt() : e->cursorPos();

    auto ctx = std::make_shared<WaitCtx>();
    LspManager::instance()->requestSignatureHelp(e, pos,
        [ctx, reply, e, pos](const QString& text, int activeParameter) {
            if (ctx->done) return;
            ctx->done = true;
            if (ctx->timer) ctx->timer->stop();
            // Render it the way the editor would, so the reply describes what
            // the user would see rather than only what arrived.
            // At the position asked about, not at the caret: a caller that
            // passed `pos` without moving the cursor would otherwise get a tip
            // anchored somewhere else.
            e->showSignatureHelp(pos, text);
            reply(QJsonObject{
                      {"text", text},
                      {"active_parameter", activeParameter},
                      {"tip_active", !e->callTipText().isEmpty()},
                  },
                  nullptr);
        });
    ArmTimeout(ctx, w, timeout, reply);
}

void HandleLspPrepareRename(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    EditorView* e = RequireEditor(w, reply);
    if (!e) return;
    const int timeout = args.value("timeout_ms").toInt(12000);
    const int pos = args.contains("pos") ? args.value("pos").toInt() : e->cursorPos();

    // Goes straight to the manager rather than through renameSymbol(), so a
    // test can read the three outcomes without the inline input opening.
    auto ctx = std::make_shared<WaitCtx>();
    LspManager::instance()->requestPrepareRename(
        e, pos, [ctx, reply](const LspManager::PrepareRename& prep) {
            if (ctx->done) return;
            ctx->done = true;
            if (ctx->timer) ctx->timer->stop();
            QJsonObject o;
            o["renameable"] = prep.renameable;
            o["placeholder"] = prep.placeholder;
            // Empty unless the server refused; carries its words verbatim.
            o["refusal"] = prep.refusal;
            o["line"] = prep.range.startLine;
            o["character"] = prep.range.startCharacter;
            reply(o, nullptr);
        });
    ArmTimeout(ctx, w, timeout, reply);
}

void HandleLspRename(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    EditorView* e = RequireEditor(w, reply);
    if (!e) return;
    const QString newName = args.value("new_name").toString();
    if (newName.isEmpty()) { ReplyErr(reply, "bad_args", "missing `new_name`"); return; }
    const int timeout = args.value("timeout_ms").toInt(12000);
    const int pos = args.contains("pos") ? args.value("pos").toInt() : e->cursorPos();
    // Default false so a test has to opt in to touching buffers; the common
    // case is asserting on the edit the server proposed.
    const bool apply = args.value("apply").toBool(false);

    auto ctx = std::make_shared<WaitCtx>();
    LspManager::instance()->requestRename(
        e, pos, newName,
        [ctx, reply, w, apply](const LspWorkspaceEdit& edit, const QString& error) {
            if (ctx->done) return;
            ctx->done = true;
            if (ctx->timer) ctx->timer->stop();

            QJsonObject documents;
            for (auto it = edit.constBegin(); it != edit.constEnd(); ++it) {
                QJsonArray edits;
                for (const LspTextEdit& te : it.value()) {
                    edits.append(QJsonObject{
                        {"line", te.range.startLine},
                        {"character", te.range.startCharacter},
                        {"end_line", te.range.endLine},
                        {"end_character", te.range.endCharacter},
                        {"new_text", te.newText},
                    });
                }
                documents.insert(LspManager::PathForUri(it.key()), edits);
            }

            QJsonObject o;
            o["documents"] = documents;
            o["document_count"] = documents.size();
            o["error"] = error;
            if (apply && error.isEmpty() && !edit.isEmpty()) {
                QString applyError;
                const int changed = w->applyWorkspaceEdit(edit, &applyError);
                o["applied"] = changed;
                o["apply_error"] = applyError;
            } else {
                o["applied"] = 0;
                o["apply_error"] = QString();
            }
            reply(o, nullptr);
        });
    ArmTimeout(ctx, w, timeout, reply);
}

void HandleEditorBeginRename(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    const int timeout = args.value("timeout_ms").toInt(12000);
    EditorView* e = RequireEditor(w, reply);
    if (!e) return;
    if (args.contains("pos")) e->setCursorPos(args.value("pos").toInt());

    // Drives the real F2 path — prepareRename included — and settles on
    // whichever arm it takes. Connected before triggering, because the refusal
    // arm can resolve inside the same event-loop turn.
    auto ctx = std::make_shared<WaitCtx>();
    auto opened = std::make_shared<QMetaObject::Connection>();
    auto finish = [ctx, opened, reply](bool wasOpened, const QString& refusal) {
        if (ctx->done) return;
        ctx->done = true;
        if (ctx->timer) ctx->timer->stop();
        QObject::disconnect(ctx->conn);
        QObject::disconnect(*opened);
        QJsonObject o;
        o["opened"] = wasOpened;
        o["refusal"] = refusal;
        reply(o, nullptr);
    };
    *opened = QObject::connect(w, &MainWindow::renameInputOpened, w,
                               [finish] { finish(true, QString()); });
    ctx->conn = QObject::connect(w, &MainWindow::renameFinished, w,
        [finish](int, const QString& message) { finish(false, message); });
    w->renameSymbol();
    ArmTimeout(ctx, w, timeout, reply);
}

void HandleEditorRenameInput(MainWindow* w, const QJsonObject&, const Reply& reply) {
    EditorView* e = RequireEditor(w, reply);
    if (!e) return;
    QJsonObject o;
    o["visible"] = e->renameInputVisible();
    o["text"] = e->renameInputText();
    reply(o, nullptr);
}

void HandleTraceRun(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    // Recording is a real subprocess running a real program, so this gets a
    // longer default than the LSP requests do.
    const int timeout = args.value("timeout_ms").toInt(20000);
    EditorView* e = RequireEditor(w, reply);
    if (!e) return;

    static const QHash<TraceOutcome, QString> kNames = {
        {TraceOutcome::Failed, QStringLiteral("failed")},
        {TraceOutcome::CompileError, QStringLiteral("compile_error")},
        {TraceOutcome::NoMain, QStringLiteral("no_main")},
        {TraceOutcome::ShortRecording, QStringLiteral("short_recording")},
        {TraceOutcome::Recorded, QStringLiteral("recorded")},
    };

    auto ctx = std::make_shared<WaitCtx>();
    ctx->conn = QObject::connect(w, &MainWindow::traceFinished, w,
        [ctx, reply](TraceOutcome outcome, const TraceSummary& summary,
                     const QString& explanation) {
            if (ctx->done) return;
            ctx->done = true;
            if (ctx->timer) ctx->timer->stop();
            QObject::disconnect(ctx->conn);
            QJsonObject o;
            o["outcome"] = kNames.value(outcome, QStringLiteral("unknown"));
            o["explanation"] = explanation;
            o["parsed"] = summary.parsed;
            o["steps"] = summary.steps;
            o["enters"] = summary.enters;
            o["peak_depth"] = summary.peakDepth;
            o["output_bytes"] = summary.outputBytes;
            o["truncated"] = summary.truncated;
            o["granularity"] = summary.granularity;
            reply(o, nullptr);
        });
    w->traceBuffer();
    ArmTimeout(ctx, w, timeout, reply);
}

void HandleNavGotoDefinition(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    const int timeout = args.value("timeout_ms").toInt(5000);
    EditorView* e = RequireEditor(w, reply);
    if (!e) return;

    // Connect *before* triggering: the round trip is asynchronous, and a
    // caller that requested first and waited second could miss the reply. This
    // is what keeps the navigation tests free of sleeps.
    auto ctx = std::make_shared<WaitCtx>();
    ctx->conn = QObject::connect(w, &MainWindow::definitionJumpFinished, w,
        [ctx, reply](bool jumped) {
            if (ctx->done) return;
            ctx->done = true;
            if (ctx->timer) ctx->timer->stop();
            QObject::disconnect(ctx->conn);
            QJsonObject o;
            o["jumped"] = jumped;
            reply(o, nullptr);
        });
    w->goToDefinition();
    ArmTimeout(ctx, w, timeout, reply);
}

void HandleNavHistory(MainWindow* w, const QJsonObject&, const Reply& reply) {
    auto encode = [](const QVector<MainWindow::NavEntry>& stack) {
        QJsonArray out;
        for (const MainWindow::NavEntry& e : stack) {
            out.append(QJsonObject{{"path", e.path}, {"pos", e.pos}});
        }
        return out;
    };
    QJsonObject o;
    o["back"] = encode(w->navBackStack());
    o["forward"] = encode(w->navForwardStack());
    reply(o, nullptr);
}

void HandleEditorIsReadOnly(MainWindow* w, const QJsonObject&, const Reply& reply) {
    EditorView* e = RequireEditor(w, reply);
    if (!e) return;
    QJsonObject o;
    o["read_only"] = e->isReadOnly();
    reply(o, nullptr);
}

void HandleWaitDiagnostics(MainWindow* w, QPointer<ControlConnection> conn,
                           const QJsonObject& args, const Reply& reply) {
    EditorView* e = RequireEditor(w, reply);
    if (!e) return;
    const int timeout = args.value("timeout_ms").toInt(10000);
    const int minCount = args.value("min_count").toInt(1);
    // `max_count` is what lets a caller wait for diagnostics to *clear*: with
    // min 0 / max 0 the already-published check fails while errors remain, so
    // the wait blocks until the repaired buffer publishes an empty batch.
    const int maxCount = args.value("max_count").toInt(INT_MAX);
    const QString uri = LspManager::UriForPath(e->filePath());
    if (uri.isEmpty()) {
        ReplyErr(reply, "no_uri", "buffer has no file path");
        return;
    }

    auto satisfied = [minCount, maxCount](int n) { return n >= minCount && n <= maxCount; };

    // The batch may already have landed before the caller started waiting.
    // hasPublishedFor distinguishes "analyzed and clean" from "not analyzed
    // yet", which a count alone cannot.
    LspManager* lsp = LspManager::instance();
    if (lsp->hasPublishedFor(uri) && satisfied(int(lsp->diagnosticsFor(uri).size()))) {
        reply(DiagnosticsToJson(lsp->diagnosticsFor(uri)), nullptr);
        return;
    }

    auto ctx = std::make_shared<WaitCtx>();
    ctx->conn = QObject::connect(lsp, &LspManager::diagnosticsUpdated, w,
        [ctx, reply, uri, satisfied, lsp](const QString& changed) {
            if (ctx->done || changed != uri) return;
            if (!lsp->hasPublishedFor(uri)) return;
            const QVector<LspDiagnostic> d = lsp->diagnosticsFor(uri);
            if (!satisfied(int(d.size()))) return;
            ctx->done = true;
            if (ctx->timer) ctx->timer->stop();
            QObject::disconnect(ctx->conn);
            reply(DiagnosticsToJson(d), nullptr);
        });
    ArmTimeout(ctx, w, timeout, reply);
    (void)conn;
}

// --- REPL ---------------------------------------------------------------

void HandleReplSend(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    ReplSession* r = w->replSession();
    if (!r || !r->isRunning()) { ReplyErr(reply, "no_repl", "REPL not running"); return; }
    QByteArray bytes = args.value("text").toString().toUtf8();
    const bool newline = args.value("newline").toBool(true);
    if (newline && !bytes.endsWith('\r') && !bytes.endsWith('\n')) bytes.append('\r');
    r->pty()->write(bytes);
    reply(Ok(), nullptr);
}

void HandleReplPress(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    ReplSession* r = w->replSession();
    if (!r || !r->isRunning()) { ReplyErr(reply, "no_repl", "REPL not running"); return; }
    const QString name = args.value("key").toString().toLower();
    const Qt::KeyboardModifiers mods = ParseMods(args.value("mods").toArray());

    QByteArray seq;
    if (name == "return" || name == "enter") seq = "\r";
    else if (name == "backspace") seq = "\x7f";
    else if (name == "tab") seq = "\t";
    else if (name == "escape" || name == "esc") seq = "\x1b";
    else if (name == "up") seq = "\x1b[A";
    else if (name == "down") seq = "\x1b[B";
    else if (name == "right") seq = "\x1b[C";
    else if (name == "left") seq = "\x1b[D";
    else if (name == "home") seq = "\x1b[H";
    else if (name == "end") seq = "\x1b[F";
    else if (name == "delete" || name == "del") seq = "\x1b[3~";
    else if (name.size() == 1) {
        const char c = name.at(0).toLatin1();
        if (mods & Qt::ControlModifier) {
            if (c >= 'a' && c <= 'z') seq.append(char(c - 'a' + 1));
            else { ReplyErr(reply, "bad_key", "unsupported Ctrl-<key>"); return; }
        } else {
            seq.append(c);
        }
    } else {
        ReplyErr(reply, "bad_key", "unknown key name"); return;
    }
    r->pty()->write(seq);
    reply(Ok(), nullptr);
}

void HandleReplGetScreen(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    const int lines = args.contains("lines") ? args.value("lines").toInt() : 40;
    const int capped = qMin(qMax(lines, 1), 500);
    QJsonObject o;
    o["text"] = w->terminalView()->screenText(capped);
    reply(o, nullptr);
}

void HandleReplGetCursor(MainWindow* w, const QJsonObject&, const Reply& reply) {
    const auto [line, col] = w->terminalView()->screenCursor();
    QJsonObject o;
    o["line"] = line; o["col"] = col;
    reply(o, nullptr);
}

void HandleReplGetCwd(MainWindow* w, const QJsonObject&, const Reply& reply) {
    ReplSession* r = w->replSession();
    if (!r) { ReplyErr(reply, "no_repl", "REPL not available"); return; }
    QJsonObject o;
    o["cwd"] = r->workingDir();
    o["running"] = r->isRunning();
    reply(o, nullptr);
}

void HandleReplSetCwd(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    ReplSession* r = w->replSession();
    if (!r) { ReplyErr(reply, "no_repl", "REPL not available"); return; }
    const QString path = args.value("path").toString();
    if (path.isEmpty()) { ReplyErr(reply, "bad_args", "missing `path`"); return; }
    const QFileInfo info(path);
    if (!info.exists() || !info.isDir()) {
        ReplyErr(reply, "bad_path", QString("not a directory: %1").arg(path));
        return;
    }
    // Restarts the REPL: a running process cannot be moved. Callers lose REPL
    // state, which is why this is spelled "restart" in the UI.
    r->restart(info.absoluteFilePath());
    QJsonObject o;
    o["cwd"] = r->workingDir();
    reply(o, nullptr);
}

void HandleReplRestart(MainWindow* w, const QJsonObject&, const Reply& reply) {
    QMetaObject::invokeMethod(w, "restartRepl", Qt::QueuedConnection);
    reply(Ok(), nullptr);
}

void HandleReplIsRunning(MainWindow* w, const QJsonObject&, const Reply& reply) {
    ReplSession* r = w->replSession();
    QJsonObject o;
    o["running"] = r && r->isRunning();
    reply(o, nullptr);
}

// --- dialects -------------------------------------------------------------
//
// Three reads over one table, so the smoke tests can assert the dialect axis
// without inferring it from style ids: what the ten bases are, what dialect the
// active buffer is in, and what dialect the live REPL session is in. The last
// is the one that cannot be derived from anything else -- it is session state.

void HandleLangBases(MainWindow*, const QJsonObject&, const Reply& reply) {
    QJsonArray bases;
    for (int i = 0; i < static_cast<int>(Dialect::Count); ++i) {
        const Dialect d = static_cast<Dialect>(i);
        bases.append(QJsonObject{
            {"base", QString::fromLatin1(DialectBaseToken(d))},
            {"language", QString::fromLatin1(DialectLanguageName(d))},
            {"reader", QString::fromLatin1(DialectReaderName(d))},
            {"sweet", DialectIsSweet(d)},
            {"formattable", DialectIsFormattable(d)},
        });
    }
    reply(QJsonObject{{"bases", bases}}, nullptr);
}

void HandleLangGet(MainWindow* w, const QJsonObject&, const Reply& reply) {
    EditorView* e = RequireEditor(w, reply);
    if (!e) return;
    const Dialect d = e->dialect();
    QJsonObject o{
        {"base", QString::fromLatin1(DialectBaseToken(d))},
        {"language", QString::fromLatin1(DialectLanguageName(d))},
        {"reader", QString::fromLatin1(DialectReaderName(d))},
    };
    ReplSession* r = w->replSession();
    if (r) {
        o["session_base"] = QString::fromLatin1(DialectBaseToken(r->dialect()));
        o["session_dirtied"] = r->dirtiedSinceReset();
        o["needs_switch"] = DialectNeedsSessionSwitch(r->dialect(), d);
    }
    reply(o, nullptr);
}

// Write the active buffer's `#lang` line, which is what the picker does.
// Deliberately NOT a UI-state setter: the reply reports whether the text
// changed, and `lang.get` reads the answer back off the buffer.
void HandleLangSet(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    EditorView* e = RequireEditor(w, reply);
    if (!e) return;
    const QByteArray base = args.value("base").toString().toUtf8();
    Dialect d = Dialect::Turmeric;
    if (!DialectFromBaseToken(base, d)) {
        ReplyErr(reply, "bad_base",
                 QString("not a `#lang` base: '%1'").arg(QString::fromUtf8(base)));
        return;
    }
    const bool changed = e->setLangDirective(d);
    reply(QJsonObject{{"changed", changed},
                      {"base", QString::fromLatin1(DialectBaseToken(e->dialect()))}},
          nullptr);
}

// The rows the Dialect menu would show for the active buffer: the headings, the
// offered readers, and which one is checked. Mirrors rebuildDialectMenu rather
// than reading QMenu back, because a QAction's text is display chrome and
// asserting on it would pin the wording rather than the behaviour.
void HandleLangMenu(MainWindow* w, const QJsonObject&, const Reply& reply) {
    EditorView* e = w ? w->editorView() : nullptr;
    const Dialect current = e ? e->dialect() : Dialect::Turmeric;

    const QVector<Dialect> offered{
        Dialect::Turmeric, Dialect::TurmericSweet,
        Dialect::Saffron,  Dialect::SaffronSweet,
        Dialect::R7rs,     Dialect::R7rsSweet,
    };

    QJsonArray rows;
    DialectLanguage heading = DialectLanguage::Turmeric;
    bool first = true;
    for (const Dialect d : offered) {
        if (first || LanguageOf(d) != heading) {
            heading = LanguageOf(d);
            first = false;
            rows.append(QJsonObject{
                {"heading", QString::fromLatin1(DialectLanguageName(d))}});
        }
        rows.append(QJsonObject{
            {"base", QString::fromLatin1(DialectBaseToken(d))},
            {"reader", QString::fromLatin1(DialectReaderName(d))},
            {"checked", d == current},
        });
    }
    if (!offered.contains(current)) {
        rows.append(QJsonObject{
            {"base", QString::fromLatin1(DialectBaseToken(current))},
            {"reader", QString::fromLatin1(DialectReaderName(current))},
            {"checked", true},
        });
    }
    reply(QJsonObject{{"rows", rows}}, nullptr);
}

void HandleLangSetSession(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    ReplSession* r = w->replSession();
    if (!r) { ReplyErr(reply, "no_repl", "no REPL session"); return; }
    const QByteArray base = args.value("base").toString().toUtf8();
    Dialect d = Dialect::Turmeric;
    if (!DialectFromBaseToken(base, d)) {
        ReplyErr(reply, "bad_base",
                 QString("not a `#lang` base: '%1'").arg(QString::fromUtf8(base)));
        return;
    }
    if (!r->switchDialect(d)) {
        ReplyErr(reply, "no_repl", "REPL is not running");
        return;
    }
    reply(Ok(), nullptr);
}

void HandleRunBuffer(MainWindow* w, const QJsonObject&, const Reply& reply) {
    QMetaObject::invokeMethod(w, "runBuffer", Qt::QueuedConnection);
    reply(Ok(), nullptr);
}

void HandleRunSelection(MainWindow* w, const QJsonObject&, const Reply& reply) {
    QMetaObject::invokeMethod(w, "runSelection", Qt::QueuedConnection);
    reply(Ok(), nullptr);
}

// --- Debug --------------------------------------------------------------

void HandleDebugStart(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    // `stop_on_entry` defaults false: phase 1 is "run with captured output",
    // not a paused stepping session. The smoke test sets it true to exercise
    // the paused path without depending on breakpoints (phase 3).
    const bool stopOnEntry = args.value("stop_on_entry").toBool(false);
    EditorView* e = RequireEditor(w, reply);
    if (!e) return;
    if (EvalModeForPath(e->filePath()) != EvalMode::Buffer) {
        ReplyErr(reply, "not_debuggable", "the active buffer is not a Turmeric script");
        return;
    }
    // debugBuffer() reads this member to decide whether to stop at entry.
    // Set it before the queued invocation so the session launches with it.
    w->setDebugStopOnEntry(stopOnEntry);
    // `replay` records the run first and then serves it backwards as well as
    // forwards. replayBuffer() forces stop_on_entry for its own reasons, so
    // the flag above is moot on that path.
    if (args.value("replay").toBool(false)) {
        QMetaObject::invokeMethod(w, "replayBuffer", Qt::QueuedConnection);
    } else {
        QMetaObject::invokeMethod(w, "debugBuffer", Qt::QueuedConnection);
    }
    reply(Ok(), nullptr);
}

void HandleDebugStop(MainWindow* w, const QJsonObject&, const Reply& reply) {
    DebugSession* d = w->debugSession();
    if (!d) { ReplyErr(reply, "no_debug", "no debug session is running"); return; }
    d->stop();
    reply(Ok(), nullptr);
}

void HandleDebugStatus(MainWindow* w, const QJsonObject&, const Reply& reply) {
    DebugSession* d = w->debugSession();
    QJsonObject o;
    if (!d) {
        o["running"] = false;
        o["state"] = "idle";
        reply(o, nullptr);
        return;
    }
    o["running"] = d->isRunning();
    static const char* kStates[] = {"idle","starting","configuring","running","paused","terminated"};
    const int idx = static_cast<int>(d->state());
    o["state"] = (idx >= 0 && idx <= 5) ? QString(kStates[idx]) : QStringLiteral("unknown");
    o["exit_code"] = d->exitCode();
    o["output"] = d->output();
    o["replay"] = d->isReplay();
    // Stepping is Paused → Paused, so `state` cannot tell a caller whether a
    // step has landed. This can.
    o["stop_count"] = d->stopCount();
    reply(o, nullptr);
}

void HandleDebugContinue(MainWindow* w, const QJsonObject&, const Reply& reply) {
    DebugSession* d = w->debugSession();
    if (!d) { ReplyErr(reply, "no_debug", "no debug session is running"); return; }
    d->resume();
    reply(Ok(), nullptr);
}

void HandleDebugStep(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    DebugSession* d = w->debugSession();
    if (!d) { ReplyErr(reply, "no_debug", "no debug session is running"); return; }
    const QString kind = args.value("kind").toString("over");
    if (kind == "in") d->stepIn();
    else if (kind == "out") d->stepOut();
    // Reverse kinds are no-ops outside a recording — the session guards them,
    // rather than the adapter answering "not supported while paused".
    else if (kind == "back") d->stepBack();
    else if (kind == "reverse_over") d->reverseStepOver();
    else d->stepOver();
    reply(Ok(), nullptr);
}

void HandleDebugTimeline(MainWindow* w, const QJsonObject&, const Reply& reply) {
    DebugSession* d = w->debugSession();
    if (!d) { ReplyErr(reply, "no_debug", "no debug session is running"); return; }
    const DebugSession::Timeline& t = d->timeline();
    QJsonObject o;
    // Two different questions. `supported` is about the adapter: `tur dap`
    // advertises the capability on `initialize`, before it knows whether this
    // launch is a replay. `available` is about *this session* — only a
    // recording has an axis to scrub.
    o["supported"] = d->hasTimeline();
    o["available"] = d->hasTimeline() && d->isReplay();
    o["steps"] = t.steps;
    o["index"] = t.index;
    o["depth"] = t.depth;
    o["output_length"] = t.outputLength;
    reply(o, nullptr);
}

void HandleDebugSeek(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    DebugSession* d = w->debugSession();
    if (!d) { ReplyErr(reply, "no_debug", "no debug session is running"); return; }
    if (!d->hasTimeline()) {
        ReplyErr(reply, "no_timeline",
                 "this `tur` has no replay timeline (needs v0.42.2 or newer)");
        return;
    }
    // The capability is advertised unconditionally by the adapter, so a live
    // session reaches here with `hasTimeline()` true and nothing to scrub.
    // `DebugSession::seek` would no-op silently; refusing with the reason is
    // the whole difference between a limitation and a bug.
    if (!d->isReplay()) {
        ReplyErr(reply, "not_a_recording",
                 "this session is live, not a recording -- relaunch with `replay`");
        return;
    }
    if (!args.contains("index")) { ReplyErr(reply, "bad_args", "need `index`"); return; }
    d->seek(args.value("index").toInt(0));
    // The cursor lands asynchronously — a `stopped` follows. Callers poll
    // `debug.timeline` or wait on `stop_count`.
    reply(Ok(), nullptr);
}

void HandleDebugSites(MainWindow* w, QPointer<ControlConnection> conn,
                      const QJsonObject& args, const Reply& reply) {
    DebugSession* d = w->debugSession();
    if (!d) { ReplyErr(reply, "no_debug", "no debug session is running"); return; }
    if (!d->hasTimeline()) {
        ReplyErr(reply, "no_timeline",
                 "this `tur` has no replay timeline (needs v0.42.2 or newer)");
        return;
    }
    if (!d->isReplay()) {
        ReplyErr(reply, "not_a_recording",
                 "this session is live, not a recording -- relaunch with `replay`");
        return;
    }
    const int buckets = args.value("buckets").toInt(64);
    d->requestSites(buckets, [reply, conn](const QVector<DebugSession::Site>& sites) {
        if (!conn) return;
        QJsonArray arr;
        for (const DebugSession::Site& s : sites) {
            arr.append(QJsonObject{{"index", s.index}, {"line", s.line},
                                   {"depth", s.depth}, {"file", s.filePath}});
        }
        QJsonObject o;
        o["sites"] = arr;
        reply(o, nullptr);
    });
}

void HandleDebugReverseContinue(MainWindow* w, const QJsonObject&, const Reply& reply) {
    DebugSession* d = w->debugSession();
    if (!d) { ReplyErr(reply, "no_debug", "no debug session is running"); return; }
    d->reverseContinue();
    reply(Ok(), nullptr);
}

void HandleDebugBreakpointToggle(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    BreakpointModel* m = w->breakpointModel();
    if (!m) { ReplyErr(reply, "no_model", "breakpoint model not initialized"); return; }
    const QString path = args.value("path").toString();
    const int line = args.value("line").toInt(0);
    if (path.isEmpty() || line < 1) {
        ReplyErr(reply, "bad_args", "need `path` and 1-based `line`");
        return;
    }
    m->toggle(path, line);
    // Report whether the breakpoint now exists (added) rather than whether
    // the set changed — toggle returns true for both add and remove.
    bool nowExists = false;
    for (const auto& bp : m->forFile(path)) {
        if (bp.line == line) { nowExists = true; break; }
    }
    QJsonObject o; o["added"] = nowExists; o["line"] = line;
    reply(o, nullptr);
}

void HandleDebugBreakpointSet(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    BreakpointModel* m = w->breakpointModel();
    if (!m) { ReplyErr(reply, "no_model", "breakpoint model not initialized"); return; }
    const QString path = args.value("path").toString();
    const int line = args.value("line").toInt(0);
    if (path.isEmpty() || line < 1) {
        ReplyErr(reply, "bad_args", "need `path` and 1-based `line`");
        return;
    }
    // Both fields are optional and each is edited independently, so an absent
    // one keeps whatever the breakpoint already had rather than resetting it.
    if (args.contains("enabled")) {
        m->setEnabled(path, line, args.value("enabled").toBool(true));
    }
    if (args.contains("condition")) {
        m->setCondition(path, line, args.value("condition").toString());
    }
    reply(Ok(), nullptr);
}

void HandleDebugBreakpoints(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    BreakpointModel* m = w->breakpointModel();
    if (!m) { ReplyErr(reply, "no_model", "breakpoint model not initialized"); return; }
    const QString path = args.value("path").toString();
    QJsonArray arr;
    for (const auto& bp : (path.isEmpty() ? m->breakpoints() : m->forFile(path))) {
        QJsonObject o;
        o["path"] = bp.path;
        o["line"] = bp.line;
        o["enabled"] = bp.enabled;
        o["condition"] = bp.condition;
        arr.append(o);
    }
    QJsonObject out; out["breakpoints"] = arr;
    reply(out, nullptr);
}

void HandleDebugFrames(MainWindow* w, const QJsonObject&, const Reply& reply) {
    DebugSession* d = w->debugSession();
    if (!d) { ReplyErr(reply, "no_debug", "no debug session is running"); return; }
    QJsonArray arr;
    for (const DebugSession::Frame& f : d->frames()) {
        arr.append(QJsonObject{
            {"id", f.id},
            {"name", f.name},
            {"path", f.filePath},
            {"line", f.line},
            {"column", f.column},
        });
    }
    QJsonObject o;
    o["frames"] = arr;
    o["selected"] = d->selectedFrameId();
    reply(o, nullptr);
}

void HandleDebugVariables(MainWindow* w, const QJsonObject&, const Reply& reply) {
    DebugSession* d = w->debugSession();
    if (!d) { ReplyErr(reply, "no_debug", "no debug session is running"); return; }
    QJsonArray arr;
    for (const DebugSession::Variable& v : d->variables()) {
        arr.append(QJsonObject{
            {"name", v.name}, {"value", v.value}, {"type", v.type},
        });
    }
    QJsonObject o;
    o["variables"] = arr;
    reply(o, nullptr);
}

void HandleDebugSelectFrame(MainWindow* w, const QJsonObject& args, const Reply& reply) {
    DebugSession* d = w->debugSession();
    if (!d) { ReplyErr(reply, "no_debug", "no debug session is running"); return; }
    if (!args.contains("frame_id")) {
        ReplyErr(reply, "bad_args", "need `frame_id`");
        return;
    }
    d->selectFrame(args.value("frame_id").toInt(0));
    // The variables arrive asynchronously; the caller polls `debug.variables`.
    reply(Ok(), nullptr);
}

void HandleDebugEvaluate(MainWindow* w, QPointer<ControlConnection> conn,
                         const QJsonObject& args, const Reply& reply) {
    DebugSession* d = w->debugSession();
    if (!d) { ReplyErr(reply, "no_debug", "no debug session is running"); return; }
    const QString expr = args.value("expression").toString();
    if (expr.isEmpty()) { ReplyErr(reply, "bad_args", "missing `expression`"); return; }
    // Held open until the adapter answers: an `evaluate` that replied Ok and
    // made the caller poll for the result would be untestable, since there is
    // nowhere for the result to be polled *from*.
    d->evaluate(expr, [reply, conn](bool ok, const QString& text) {
        if (!conn) return;
        QJsonObject o;
        o["ok"] = ok;
        o["result"] = text;
        reply(o, nullptr);
    });
}

// --- Waiters ------------------------------------------------------------

void HandleWaitReplOutput(MainWindow* w, QPointer<ControlConnection> conn,
                          const QJsonObject& args, const Reply& reply) {
    const QString pattern = args.value("pattern").toString();
    const bool regex = args.value("regex").toBool(false);
    const int timeout = args.value("timeout_ms").toInt(3000);
    if (pattern.isEmpty()) { ReplyErr(reply, "bad_args", "missing pattern"); return; }

    auto matcher = [pattern, regex](const QString& hay) -> QString {
        if (regex) {
            QRegularExpression re(pattern);
            if (!re.isValid()) return {};
            auto m = re.match(hay);
            return m.hasMatch() ? m.captured(0) : QString();
        }
        return hay.contains(pattern) ? pattern : QString();
    };

    // Check what's already on-screen first.
    const QString existing = w->terminalView()->screenText(500);
    const QString hit = matcher(existing);
    if (!hit.isEmpty()) {
        QJsonObject o; o["matched"] = hit; o["already_present"] = true;
        reply(o, nullptr);
        return;
    }

    ReplSession* r = w->replSession();
    if (!r) { ReplyErr(reply, "no_repl", "REPL not running"); return; }
    auto ctx = std::make_shared<WaitCtx>();
    auto accum = std::make_shared<QByteArray>();

    ctx->conn = QObject::connect(r, &ReplSession::dataReceived, w,
        [ctx, accum, matcher, reply](const QByteArray& bytes) {
            if (ctx->done) return;
            accum->append(bytes);
            const QString hit = matcher(QString::fromUtf8(*accum));
            if (!hit.isEmpty()) {
                ctx->done = true;
                if (ctx->timer) ctx->timer->stop();
                QObject::disconnect(ctx->conn);
                QJsonObject o; o["matched"] = hit;
                reply(o, nullptr);
            }
        });
    ArmTimeout(ctx, w, timeout, reply);
    (void)conn;
}

void HandleWaitReplIdle(MainWindow* w, QPointer<ControlConnection> conn,
                        const QJsonObject& args, const Reply& reply) {
    const int quiet = args.value("quiet_ms").toInt(200);
    const int timeout = args.value("timeout_ms").toInt(5000);
    ReplSession* r = w->replSession();
    if (!r) { ReplyErr(reply, "no_repl", "REPL not running"); return; }

    auto ctx = std::make_shared<WaitCtx>();
    auto quietTimer = std::make_shared<QPointer<QTimer>>(new QTimer(w));
    (*quietTimer)->setSingleShot(true);

    QObject::connect((*quietTimer).data(), &QTimer::timeout, w, [ctx, reply]() {
        if (ctx->done) return;
        ctx->done = true;
        if (ctx->timer) ctx->timer->stop();
        QObject::disconnect(ctx->conn);
        reply(Ok(), nullptr);
    });

    ctx->conn = QObject::connect(r, &ReplSession::dataReceived, w,
        [quietTimer, quiet](const QByteArray&) {
            if (*quietTimer) (*quietTimer)->start(quiet);
        });

    (*quietTimer)->start(quiet);
    ArmTimeout(ctx, w, timeout, reply);
    (void)conn;
}

void HandleWaitEditorSignal(MainWindow* w, QPointer<ControlConnection> conn,
                            const QJsonObject& args, const Reply& reply) {
    const QString sig = args.value("signal").toString();
    const int timeout = args.value("timeout_ms").toInt(3000);
    // Connecting to a null sender would not crash, but the wait could never
    // fire — it would fail as a timeout, hiding the real reason.
    EditorView* ed = RequireEditor(w, reply);
    if (!ed) return;
    auto ctx = std::make_shared<WaitCtx>();

    if (sig == "modifiedChanged") {
        ctx->conn = QObject::connect(ed, &EditorView::modifiedChanged, w,
            [ctx, reply](bool modified) {
                if (ctx->done) return;
                ctx->done = true;
                if (ctx->timer) ctx->timer->stop();
                QObject::disconnect(ctx->conn);
                QJsonObject o; o["modified"] = modified;
                reply(o, nullptr);
            });
    } else if (sig == "filePathChanged") {
        ctx->conn = QObject::connect(ed, &EditorView::filePathChanged, w,
            [ctx, reply](const QString& path) {
                if (ctx->done) return;
                ctx->done = true;
                if (ctx->timer) ctx->timer->stop();
                QObject::disconnect(ctx->conn);
                QJsonObject o; o["path"] = path;
                reply(o, nullptr);
            });
    } else {
        ReplyErr(reply, "bad_signal", QString("unknown signal: %1").arg(sig));
        return;
    }
    ArmTimeout(ctx, w, timeout, reply);
    (void)conn;
}

void HandleWaitProcessExit(MainWindow* w, QPointer<ControlConnection> conn,
                           const QJsonObject& args, const Reply& reply) {
    const int timeout = args.value("timeout_ms").toInt(3000);
    ReplSession* r = w->replSession();
    if (!r) { ReplyErr(reply, "no_repl", "REPL not running"); return; }
    if (!r->isRunning()) { QJsonObject o; o["exit_code"] = -1; o["already_exited"] = true; reply(o, nullptr); return; }
    auto ctx = std::make_shared<WaitCtx>();
    ctx->conn = QObject::connect(r, &ReplSession::stopped, w, [ctx, reply](int code) {
        if (ctx->done) return;
        ctx->done = true;
        if (ctx->timer) ctx->timer->stop();
        QObject::disconnect(ctx->conn);
        QJsonObject o; o["exit_code"] = code;
        reply(o, nullptr);
    });
    ArmTimeout(ctx, w, timeout, reply);
    (void)conn;
}

}  // namespace

void Dispatch(WindowManager* windows, QPointer<ControlConnection> conn,
              const QString& cmd, const QJsonObject& args, Reply reply) {
    if (!windows) { ReplyErr(reply, "no_registry", "window registry not available"); return; }

    // Registry-level commands: these must work with zero windows open, so they
    // never resolve a target window (asking "how many windows?" must not create
    // one to answer).
    if (cmd == "ping") { QJsonObject o; o["pong"] = true; reply(o, nullptr); return; }
    if (cmd == "window.new")  { HandleWindowNew(windows, args, reply); return; }
    if (cmd == "window.list") { HandleWindowList(windows, args, reply); return; }

    // Opening a document creates a window when none is open — requirement (a).
    // Every other command operates on the window that already has focus, and
    // reports `no_window` rather than conjuring a blank one to answer a query.
    const bool createsWindow = (cmd == "editor.open" || cmd == "window.activate");
    MainWindow* w = createsWindow ? windows->activeOrNewWindow() : windows->activeWindow();
    if (!w) { ReplyErr(reply, "no_window", "no editor window is open"); return; }

    if (cmd == "window.activate")      { HandleWindowActivate(w, args, reply); return; }
    if (cmd == "window.drop")          { HandleWindowDrop(w, args, reply); return; }
    if (cmd == "window.focus")         { HandleWindowFocus(w, args, reply); return; }
    if (cmd == "window.geometry")      { HandleWindowGeometry(w, args, reply); return; }
    if (cmd == "window.set_splitter")  { HandleWindowSetSplitter(w, args, reply); return; }
    if (cmd == "menu.invoke")          { HandleMenuInvoke(w, args, reply); return; }
    if (cmd == "window.screenshot")    { HandleWindowScreenshot(w, args, reply); return; }

    if (cmd == "editor.open")          { HandleEditorOpen(w, args, reply); return; }
    if (cmd == "editor.save")          { HandleEditorSave(w, args, reply); return; }
    if (cmd == "editor.save_as")       { HandleEditorSave(w, args, reply); return; }
    if (cmd == "editor.set_text")      { HandleEditorSetText(w, args, reply); return; }
    if (cmd == "editor.type")          { HandleEditorType(w, args, reply); return; }
    if (cmd == "editor.press")         { HandleEditorPress(w, args, reply); return; }
    if (cmd == "editor.get_text")      { HandleEditorGetText(w, args, reply); return; }
    if (cmd == "editor.get_cursor")    { HandleEditorGetCursor(w, args, reply); return; }
    if (cmd == "editor.set_cursor")    { HandleEditorSetCursor(w, args, reply); return; }
    if (cmd == "editor.get_selection") { HandleEditorGetSelection(w, args, reply); return; }
    if (cmd == "editor.set_selection") { HandleEditorSetSelection(w, args, reply); return; }
    if (cmd == "editor.get_style_at")  { HandleEditorGetStyleAt(w, args, reply); return; }
    if (cmd == "editor.is_read_only")  { HandleEditorIsReadOnly(w, args, reply); return; }
    if (cmd == "editor.rename_input")  { HandleEditorRenameInput(w, args, reply); return; }
    if (cmd == "editor.begin_rename")  { HandleEditorBeginRename(w, args, reply); return; }

    if (cmd == "lsp.status")           { HandleLspStatus(w, args, reply); return; }
    if (cmd == "lsp.diagnostics")      { HandleLspDiagnostics(w, args, reply); return; }
    if (cmd == "lsp.decorations")      { HandleLspDecorations(w, args, reply); return; }
    if (cmd == "lsp.completions")      { HandleLspCompletions(w, args, reply); return; }
    if (cmd == "lsp.hover")            { HandleLspHover(w, args, reply); return; }
    if (cmd == "lsp.definition")       { HandleLspDefinition(w, args, reply); return; }
    if (cmd == "lsp.symbols")          { HandleLspSymbols(w, args, reply); return; }
    if (cmd == "lsp.highlights")       { HandleLspHighlights(w, args, reply); return; }
    if (cmd == "lsp.references")       { HandleLspReferences(w, args, reply); return; }
    if (cmd == "lsp.workspace_symbols") { HandleLspWorkspaceSymbols(w, args, reply); return; }
    if (cmd == "lsp.signature_help")   { HandleLspSignatureHelp(w, args, reply); return; }
    if (cmd == "lsp.prepare_rename")   { HandleLspPrepareRename(w, args, reply); return; }
    if (cmd == "lsp.rename")           { HandleLspRename(w, args, reply); return; }
    if (cmd == "lsp.restart")          { HandleLspRestart(w, args, reply); return; }

    if (cmd == "trace.run")            { HandleTraceRun(w, args, reply); return; }

    if (cmd == "nav.history")          { HandleNavHistory(w, args, reply); return; }
    if (cmd == "nav.goto_definition")  { HandleNavGotoDefinition(w, args, reply); return; }

    if (cmd == "repl.send")            { HandleReplSend(w, args, reply); return; }
    if (cmd == "repl.press")           { HandleReplPress(w, args, reply); return; }
    if (cmd == "repl.get_screen")      { HandleReplGetScreen(w, args, reply); return; }
    if (cmd == "repl.get_cursor")      { HandleReplGetCursor(w, args, reply); return; }
    if (cmd == "repl.restart")         { HandleReplRestart(w, args, reply); return; }
    if (cmd == "repl.get_cwd")         { HandleReplGetCwd(w, args, reply); return; }
    if (cmd == "repl.set_cwd")         { HandleReplSetCwd(w, args, reply); return; }
    if (cmd == "repl.is_running")      { HandleReplIsRunning(w, args, reply); return; }
    if (cmd == "lang.bases")           { HandleLangBases(w, args, reply); return; }
    if (cmd == "lang.get")             { HandleLangGet(w, args, reply); return; }
    if (cmd == "lang.set")             { HandleLangSet(w, args, reply); return; }
    if (cmd == "lang.menu")            { HandleLangMenu(w, args, reply); return; }
    if (cmd == "lang.set_session")     { HandleLangSetSession(w, args, reply); return; }
    if (cmd == "run.buffer")           { HandleRunBuffer(w, args, reply); return; }
    if (cmd == "run.selection")        { HandleRunSelection(w, args, reply); return; }

    if (cmd == "debug.start")          { HandleDebugStart(w, args, reply); return; }
    if (cmd == "debug.stop")           { HandleDebugStop(w, args, reply); return; }
    if (cmd == "debug.status")         { HandleDebugStatus(w, args, reply); return; }
    if (cmd == "debug.continue")       { HandleDebugContinue(w, args, reply); return; }
    if (cmd == "debug.step")           { HandleDebugStep(w, args, reply); return; }
    if (cmd == "debug.breakpoint.toggle") { HandleDebugBreakpointToggle(w, args, reply); return; }
    if (cmd == "debug.breakpoint.set") { HandleDebugBreakpointSet(w, args, reply); return; }
    if (cmd == "debug.breakpoints")    { HandleDebugBreakpoints(w, args, reply); return; }
    if (cmd == "debug.frames")         { HandleDebugFrames(w, args, reply); return; }
    if (cmd == "debug.variables")      { HandleDebugVariables(w, args, reply); return; }
    if (cmd == "debug.select_frame")   { HandleDebugSelectFrame(w, args, reply); return; }
    if (cmd == "debug.evaluate")       { HandleDebugEvaluate(w, conn, args, reply); return; }
    if (cmd == "debug.reverse_continue") { HandleDebugReverseContinue(w, args, reply); return; }
    if (cmd == "debug.timeline")       { HandleDebugTimeline(w, args, reply); return; }
    if (cmd == "debug.seek")           { HandleDebugSeek(w, args, reply); return; }
    if (cmd == "debug.sites")          { HandleDebugSites(w, conn, args, reply); return; }

    if (cmd == "wait.repl_output")     { HandleWaitReplOutput(w, conn, args, reply); return; }
    if (cmd == "wait.repl_idle")       { HandleWaitReplIdle(w, conn, args, reply); return; }
    if (cmd == "wait.editor_signal")   { HandleWaitEditorSignal(w, conn, args, reply); return; }
    if (cmd == "wait.process_exit")    { HandleWaitProcessExit(w, conn, args, reply); return; }
    if (cmd == "wait.diagnostics")     { HandleWaitDiagnostics(w, conn, args, reply); return; }

    ReplyErr(reply, "unknown_cmd", QString("no such command: %1").arg(cmd));
}

}
