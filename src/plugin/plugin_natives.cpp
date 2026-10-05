#include "plugin_natives.h"

#include "command_registry.h"
#include "hook_bus.h"
#include "plugin_host.h"
#include "snippet_session.h"
#include "syntax_descriptor.h"

#include "app/icon_font.h"
#include "app/main_window.h"
#include "editor/editor_view.h"
#include "editor/theme_loader.h"

#include <ScintillaEdit.h>

#include <QAction>
#include <QKeyEvent>
#include <QStatusBar>

#include <cstdlib>

namespace trowel {

// Helper: get the PluginContext from a TuriNativeFn's ud pointer.
static PluginContext* ctx(void* ud) {
    return static_cast<PluginContext*>(ud);
}

// Helper: get the active EditorView from the context.
static EditorView* activeEditor(PluginContext* c) {
    return c->window ? c->window->editorView() : nullptr;
}

// Helper: convert a TuriValue to a C string (or null).
static const char* asCStr(TuriValue v) {
    return v.tag == TURI_CSTR ? v.as_cstr : nullptr;
}

// Helper: convert a TuriValue to an int (or 0).
static int64_t asInt(TuriValue v) {
    return v.tag == TURI_INT ? v.as_int : 0;
}

// ---- Registration natives ---------------------------------------------------

// (trowel:register-command :id "..." :title "..." :category "..." :key "..." :fn closure)
// The closure is a zero-argument function called when the command runs.
static TuriValue native_register_command(TuriEnv* /*env*/, TuriValue* args,
                                           uint32_t n, void* ud)
{
    auto* c = ctx(ud);
    if (!c || !c->registry) return turi_error("no command registry");
    if (n < 5) return turi_error("register-command needs 5 args");

    CommandEntry cmd;
    cmd.id = QString::fromUtf8(asCStr(args[0]) ? asCStr(args[0]) : "");
    cmd.title = QString::fromUtf8(asCStr(args[1]) ? asCStr(args[1]) : "");
    cmd.category = QString::fromUtf8(asCStr(args[2]) ? asCStr(args[2]) : "");
    const char* keyStr = asCStr(args[3]);
    if (keyStr && *keyStr)
        cmd.shortcut = QKeySequence(QString::fromUtf8(keyStr));

    // args[4] is the closure.  We store it in the handler via the host's
    // callClosure mechanism.  The closure value is valid for the env's
    // lifetime, which outlives the registry.
    TuriValue closure = args[4];
    PluginHost* host = c->host;
    cmd.handler = [host, closure] {
        host->callClosure(closure, {});
    };

    c->registry->add(cmd);
    return turi_nil();
}

// (trowel:register-button :icon "..." :tooltip "..." :command "...")
// The icon string is a hex Nerd Font codepoint, e.g. "0xF0411" for
// nf-md-playlist_play.  The glyph is rasterized via the same NerdIcon
// path the built-in side-bar buttons use.
static TuriValue native_register_button(TuriEnv* /*env*/, TuriValue* args,
                                         uint32_t n, void* ud)
{
    auto* c = ctx(ud);
    if (!c || !c->window) return turi_error("no main window");
    if (n < 3) return turi_error("register-button needs 3 args");

    const QString iconStr = QString::fromUtf8(asCStr(args[0]) ? asCStr(args[0]) : "");
    const QString tooltip = QString::fromUtf8(asCStr(args[1]) ? asCStr(args[1]) : "");
    const QString commandId = QString::fromUtf8(asCStr(args[2]) ? asCStr(args[2]) : "");

    auto* action = new QAction(c->window);
    action->setToolTip(tooltip);

    // Parse the icon string as a hex Nerd Font codepoint.
    bool ok = false;
    const char* raw = asCStr(args[0]);
    if (raw) {
        const char* p = raw;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        char32_t codepoint = static_cast<char32_t>(std::strtoul(p, nullptr, 16));
        ok = (codepoint != 0 || (p[0] == '0' && !p[1]));
        if (ok) {
            const QColor iconColor = LoadBuiltinDarkTheme().editorFg;
            action->setIcon(NerdIcon(codepoint, 18, iconColor));
        }
    }

    // Fall back to text if the codepoint did not parse.  The side bar uses
    // ToolButtonIconOnly, so a text-only action shows a blank button — but
    // at least the tooltip works.
    if (!ok)
        action->setText(iconStr);

    QObject::connect(action, &QAction::triggered, c->window, [c, commandId] {
        if (c->registry) c->registry->run(commandId);
    });
    c->window->addSideBarAction(action);
    return turi_nil();
}

// (trowel:register-hook :event "..." :fn closure)
static TuriValue native_register_hook(TuriEnv* /*env*/, TuriValue* args,
                                       uint32_t n, void* ud)
{
    auto* c = ctx(ud);
    if (!c || !c->hookBus) return turi_error("no hook bus");
    if (n < 2) return turi_error("register-hook needs 2 args");

    const QString event = QString::fromUtf8(asCStr(args[0]) ? asCStr(args[0]) : "");
    TuriValue closure = args[1];
    c->hookBus->subscribe(event, closure);
    return turi_nil();
}

// ---- State reading natives --------------------------------------------------

// (trowel:buffer-text) → cstr
static TuriValue native_buffer_text(TuriEnv* env, TuriValue* /*args*/,
                                      uint32_t /*n*/, void* ud)
{
    auto* e = activeEditor(ctx(ud));
    if (!e) return turi_nil();
    QByteArray text = e->text();
    char* copy = turi_val_strdup(env, text.constData());
    return turi_cstr(copy);
}

// (trowel:cursor-pos) → int
static TuriValue native_cursor_pos(TuriEnv* /*env*/, TuriValue* /*args*/,
                                     uint32_t /*n*/, void* ud)
{
    auto* e = activeEditor(ctx(ud));
    if (!e) return turi_int(0);
    return turi_int(e->cursorPos());
}

// (trowel:selection) → (start end) as two int args
static TuriValue native_selection(TuriEnv* /*env*/, TuriValue* /*args*/,
                                    uint32_t /*n*/, void* ud)
{
    auto* e = activeEditor(ctx(ud));
    if (!e) return turi_nil();
    auto [start, end] = e->selectionRange();
    // Return start as int; the caller can get end via a second call if needed.
    // For v1 simplicity, return start.
    return turi_int(start);
}

// (trowel:file-path) → cstr
static TuriValue native_file_path(TuriEnv* env, TuriValue* /*args*/,
                                    uint32_t /*n*/, void* ud)
{
    auto* e = activeEditor(ctx(ud));
    if (!e) return turi_nil();
    char* copy = turi_val_strdup(env, e->filePath().toUtf8().constData());
    return turi_cstr(copy);
}

// (trowel:word-at-cursor) → cstr
static TuriValue native_word_at_cursor(TuriEnv* env, TuriValue* /*args*/,
                                         uint32_t /*n*/, void* ud)
{
    auto* e = activeEditor(ctx(ud));
    if (!e || !e->sciWidget()) return turi_nil();
    ScintillaEdit* sci = e->sciWidget();
    int pos = e->cursorPos();
    // SCI_WORDSTARTPOSITION / SCI_WORDENDPOSITION find word boundaries.
    int start = sci->wordStartPosition(pos, true);
    int end = sci->wordEndPosition(pos, true);
    if (start >= end) return turi_nil();
    QByteArray word = e->textInRange(start, end);
    char* copy = turi_val_strdup(env, word.constData());
    return turi_cstr(copy);
}

// ---- Buffer manipulation natives -------------------------------------------

// (trowel:set-text :text "...")
static TuriValue native_set_text(TuriEnv* /*env*/, TuriValue* args,
                                    uint32_t n, void* ud)
{
    auto* e = activeEditor(ctx(ud));
    if (!e) return turi_nil();
    if (n < 1) return turi_error("set-text needs 1 arg");
    const char* text = asCStr(args[0]);
    e->setText(QByteArray(text ? text : ""));
    return turi_nil();
}

// (trowel:insert-text :text "..." :pos int)
static TuriValue native_insert_text(TuriEnv* /*env*/, TuriValue* args,
                                      uint32_t n, void* ud)
{
    auto* e = activeEditor(ctx(ud));
    if (!e || !e->sciWidget()) return turi_nil();
    if (n < 2) return turi_error("insert-text needs 2 args");
    const char* text = asCStr(args[0]);
    int pos = static_cast<int>(asInt(args[1]));
    e->sciWidget()->insertText(pos, text ? text : "");
    return turi_nil();
}

// (trowel:set-cursor :pos int)
static TuriValue native_set_cursor(TuriEnv* /*env*/, TuriValue* args,
                                      uint32_t n, void* ud)
{
    auto* e = activeEditor(ctx(ud));
    if (!e) return turi_nil();
    if (n < 1) return turi_error("set-cursor needs 1 arg");
    e->setCursorPos(static_cast<int>(asInt(args[0])));
    return turi_nil();
}

// (trowel:set-selection :anchor int :caret int)
static TuriValue native_set_selection(TuriEnv* /*env*/, TuriValue* args,
                                         uint32_t n, void* ud)
{
    auto* e = activeEditor(ctx(ud));
    if (!e) return turi_nil();
    if (n < 2) return turi_error("set-selection needs 2 args");
    e->setSelection(static_cast<int>(asInt(args[0])),
                    static_cast<int>(asInt(args[1])));
    return turi_nil();
}

// ---- UI natives -------------------------------------------------------------

// (trowel:status-message :msg "...")
static TuriValue native_status_message(TuriEnv* /*env*/, TuriValue* args,
                                          uint32_t n, void* ud)
{
    auto* c = ctx(ud);
    if (!c || !c->window) return turi_nil();
    if (n < 1) return turi_error("status-message needs 1 arg");
    const char* msg = asCStr(args[0]);
    c->window->statusBar()->show();
    c->window->statusBar()->showMessage(
        QString::fromUtf8(msg ? msg : ""), 3000);
    return turi_nil();
}

// (trowel:focus-editor)
static TuriValue native_focus_editor(TuriEnv* /*env*/, TuriValue* /*args*/,
                                         uint32_t /*n*/, void* ud)
{
    auto* c = ctx(ud);
    if (c && c->window) c->window->focusEditor();
    return turi_nil();
}

// (trowel:focus-repl)
static TuriValue native_focus_repl(TuriEnv* /*env*/, TuriValue* /*args*/,
                                      uint32_t /*n*/, void* ud)
{
    auto* c = ctx(ud);
    if (c && c->window) c->window->focusRepl();
    return turi_nil();
}

// (trowel-reload-plugin :name "...")
static TuriValue native_reload_plugin(TuriEnv* /*env*/, TuriValue* args,
                                         uint32_t n, void* ud)
{
    auto* c = ctx(ud);
    if (!c || !c->host) return turi_error("no plugin host");
    if (n < 1) return turi_error("reload-plugin needs 1 arg");
    const char* name = asCStr(args[0]);
    if (!name) return turi_error("reload-plugin: name is not a string");
    bool ok = c->host->reloadPlugin(QString::fromUtf8(name));
    return turi_bool(ok);
}

// ---- Snippet natives --------------------------------------------------------

// (trowel-insert-snippet :template "...")
// Parses the template for tab-stops, inserts the expanded text at the cursor,
// and selects the first tab-stop.  Starts snippet mode.
static TuriValue native_insert_snippet(TuriEnv* /*env*/, TuriValue* args,
                                         uint32_t n, void* ud)
{
    auto* c = ctx(ud);
    if (!c || !c->snippet) return turi_error("no snippet session");
    auto* e = activeEditor(c);
    if (!e || !e->sciWidget()) return turi_error("no active editor");
    if (n < 1) return turi_error("insert-snippet needs 1 arg");

    const char* tmpl = asCStr(args[0]);
    if (!tmpl) return turi_error("insert-snippet: template is not a string");

    ScintillaEdit* sci = e->sciWidget();
    int pos = e->cursorPos();

    // Parse the template into text + tab-stops.
    c->snippet->parse(QByteArray(tmpl), pos);

    // Insert the expanded text.
    sci->insertText(pos, c->snippet->text().constData());

    // Select the first tab-stop.
    SnippetStop stop = c->snippet->firstStop();
    if (stop.start >= 0) {
        e->setSelection(stop.start, stop.end);
    }

    return turi_nil();
}

// (trowel-snippet-active?) → bool
static TuriValue native_snippet_active(TuriEnv* /*env*/, TuriValue* /*args*/,
                                         uint32_t /*n*/, void* ud)
{
    auto* c = ctx(ud);
    if (!c || !c->snippet) return turi_bool(false);
    return turi_bool(c->snippet->active());
}

// (trowel-snippet-advance)
// Advances to the next tab-stop.  When the last stop is reached, exits
// snippet mode and places the cursor at ${0}.
static TuriValue native_snippet_advance(TuriEnv* /*env*/, TuriValue* /*args*/,
                                          uint32_t /*n*/, void* ud)
{
    auto* c = ctx(ud);
    if (!c || !c->snippet) return turi_nil();
    auto* e = activeEditor(c);
    if (!e) return turi_nil();

    SnippetStop stop = c->snippet->advance();
    if (stop.start >= 0) {
        e->setSelection(stop.start, stop.end);
    }
    return turi_nil();
}

// (trowel-set-keybinding :command "..." :key "...")
// Override the keyboard shortcut for a registered command.
static TuriValue native_set_keybinding(TuriEnv* /*env*/, TuriValue* args,
                                          uint32_t n, void* ud)
{
    auto* c = ctx(ud);
    if (!c || !c->registry) return turi_error("no command registry");
    if (n < 2) return turi_error("set-keybinding needs 2 args");
    const char* cmdId = asCStr(args[0]);
    const char* keyStr = asCStr(args[1]);
    if (!cmdId) return turi_error("set-keybinding: command id is not a string");
    if (!keyStr || !*keyStr) return turi_error("set-keybinding: key is empty");
    QKeySequence ks(QString::fromUtf8(keyStr));
    if (ks.isEmpty()) return turi_error("set-keybinding: invalid key sequence");
    if (!c->registry->setShortcut(QString::fromUtf8(cmdId), ks))
        return turi_error("set-keybinding: command not found");
    return turi_nil();
}

// (trowel-spawn :src "...")
// Spawn an async background task from Turmeric source.  Returns a TURI_FUTURE.
// The host pumps the scheduler on a QTimer (§4.5).
static TuriValue native_spawn(TuriEnv* env, TuriValue* args,
                                 uint32_t n, void* /*ud*/)
{
    if (n < 1) return turi_error("spawn needs 1 arg");
    const char* src = asCStr(args[0]);
    if (!src) return turi_error("spawn: source is not a string");
    return turi_task_spawn(env, src);
}

// (trowel-pump-events)
// Drain the async scheduler manually.  Normally the host's QTimer does this
// every ~16ms, but a plugin can call this to pump immediately.
static TuriValue native_pump_events(TuriEnv* env, TuriValue* /*args*/,
                                        uint32_t /*n*/, void* /*ud*/)
{
    turi_run_event_loop(env);
    return turi_nil();
}

// ---- Registration -----------------------------------------------------------

void registerPluginNatives(TuriEnv* env, PluginContext* ctxPtr)
{
    struct NativeDef {
        const char* name;
        TuriNativeFn fn;
        TurNativeRetType ret;
    };

    static const NativeDef natives[] = {
        {"trowel-register-command",  native_register_command,  TUR_NRT_VOID},
        {"trowel-register-button",   native_register_button,   TUR_NRT_VOID},
        {"trowel-register-hook",     native_register_hook,     TUR_NRT_VOID},
        {"trowel-buffer-text",       native_buffer_text,       TUR_NRT_CSTR},
        {"trowel-cursor-pos",        native_cursor_pos,        TUR_NRT_INT},
        {"trowel-selection",         native_selection,         TUR_NRT_INT},
        {"trowel-file-path",         native_file_path,         TUR_NRT_CSTR},
        {"trowel-word-at-cursor",    native_word_at_cursor,    TUR_NRT_CSTR},
        {"trowel-set-text",          native_set_text,          TUR_NRT_VOID},
        {"trowel-insert-text",       native_insert_text,       TUR_NRT_VOID},
        {"trowel-set-cursor",        native_set_cursor,        TUR_NRT_VOID},
        {"trowel-set-selection",     native_set_selection,     TUR_NRT_VOID},
        {"trowel-status-message",    native_status_message,    TUR_NRT_VOID},
        {"trowel-focus-editor",      native_focus_editor,      TUR_NRT_VOID},
        {"trowel-focus-repl",        native_focus_repl,        TUR_NRT_VOID},
        {"trowel-reload-plugin",     native_reload_plugin,     TUR_NRT_BOOL},
        {"trowel-insert-snippet",    native_insert_snippet,    TUR_NRT_VOID},
        {"trowel-snippet-active?",   native_snippet_active,    TUR_NRT_BOOL},
        {"trowel-snippet-advance",   native_snippet_advance,   TUR_NRT_VOID},
        {"trowel-set-keybinding",    native_set_keybinding,    TUR_NRT_VOID},
        {"trowel-spawn",             native_spawn,             TUR_NRT_VOID},
        {"trowel-pump-events",       native_pump_events,       TUR_NRT_VOID},
    };

    for (const auto& def : natives) {
        turi_env_register_native_typed(env, def.name, def.fn, ctxPtr, def.ret);
    }
}

// ---- Syntax plugin natives -------------------------------------------------

// (trowel-define-syntax :name "..." :extensions "ext1,ext2" ...)
// Registers a syntax descriptor from a syntax.tur file.
static TuriValue native_define_syntax(TuriEnv* /*env*/, TuriValue* args,
                                         uint32_t n, void* ud)
{
    auto* registry = static_cast<SyntaxRegistry*>(ud);
    if (!registry) return turi_error("no syntax registry");
    if (n < 2) return turi_error("define-syntax needs at least name + extensions");

    SyntaxDescriptor desc;
    desc.name = QString::fromUtf8(asCStr(args[0]) ? asCStr(args[0]) : "");

    // args[1] is a comma-separated extension list.
    const char* extStr = asCStr(args[1]);
    if (extStr) {
        desc.extensions = QString::fromUtf8(extStr).split(',',
            Qt::SkipEmptyParts);
        for (auto& ext : desc.extensions)
            ext = ext.trimmed();
    }

    // Optional named args as pairs: [key value key value ...]
    for (uint32_t i = 2; i + 1 < n; i += 2) {
        const char* key = asCStr(args[i]);
        if (!key) continue;
        const char* val = asCStr(args[i + 1]);

        QByteArray keyBa(key);
        if (keyBa == "keywords") {
            // Keywords as a space-separated string.
            if (val) desc.keywords = QString::fromUtf8(val).split(
                ' ', Qt::SkipEmptyParts);
        } else if (keyBa == "line-comment") {
            if (val) desc.lineComment = QString::fromUtf8(val);
        } else if (keyBa == "block-comment-open") {
            if (val) desc.blockCommentOpen = QString::fromUtf8(val);
        } else if (keyBa == "block-comment-close") {
            if (val) desc.blockCommentClose = QString::fromUtf8(val);
        } else if (keyBa == "string-delim") {
            if (val) desc.stringDelim = QString::fromUtf8(val);
        } else if (keyBa == "string-escape") {
            if (val) desc.stringEscape = QString::fromUtf8(val);
        } else if (keyBa == "operators") {
            if (val) desc.operators = QString::fromUtf8(val);
        } else if (keyBa == "fold-open") {
            if (val) desc.foldOpen = QString::fromUtf8(val);
        } else if (keyBa == "fold-close") {
            if (val) desc.foldClose = QString::fromUtf8(val);
        } else if (keyBa == "numbers") {
            desc.hasNumbers = (args[i + 1].tag == TURI_BOOL)
                ? args[i + 1].as_bool : true;
        }
    }

    registry->registerDescriptor(desc);
    return turi_nil();
}

void registerSyntaxNatives(TuriEnv* env, SyntaxRegistry* registry)
{
    turi_env_register_native_typed(env, "trowel-define-syntax",
                             native_define_syntax, registry, TUR_NRT_VOID);
}

}  // namespace trowel
