#include "editor/theme_loader.h"

#include "editor/editor_view.h"
#include "editor/lexers.h"
#include "repl/terminal_view.h"

#include <ScintillaEdit.h>

#include <QFile>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QPalette>
#include <QWidget>

namespace trowel {

namespace {

QColor parseColor(const QJsonValue& v, const QColor& fallback = {}) {
    if (!v.isString()) return fallback;
    QColor c(v.toString());
    return c.isValid() ? c : fallback;
}

StyleSpec parseStyle(const QJsonValue& v) {
    StyleSpec s;
    if (!v.isObject()) return s;
    const QJsonObject o = v.toObject();
    s.fg = parseColor(o.value("fg"));
    s.bg = parseColor(o.value("bg"));
    s.bold = o.value("bold").toBool(false);
    s.italic = o.value("italic").toBool(false);
    return s;
}

int bgra(const QColor& c) {
    // Scintilla accepts 0x00BBGGRR for opaque colors.
    return (c.blue() << 16) | (c.green() << 8) | c.red();
}

void setStyle(ScintillaEdit* sci, int id, const StyleSpec& spec,
              const QColor& defaultBg) {
    if (spec.fg.isValid()) sci->styleSetFore(id, bgra(spec.fg));
    sci->styleSetBack(id, bgra(spec.bg.isValid() ? spec.bg : defaultBg));
    sci->styleSetBold(id, spec.bold);
    sci->styleSetItalic(id, spec.italic);
}

// Theme keys to Scintilla style slots. Turmeric's keys are unprefixed for
// backwards compatibility with existing theme files; every other language
// namespaces its own.
const QHash<QString, int>& StyleKeyMap() {
    auto id = [](auto style) { return static_cast<int>(style); };
    static const QHash<QString, int> m = {
        {"default",      id(TurStyle::Default)},
        {"lineComment",  id(TurStyle::LineComment)},
        {"docComment",   id(TurStyle::DocComment)},
        {"blockComment", id(TurStyle::BlockComment)},
        {"string",       id(TurStyle::String)},
        {"stringEscape", id(TurStyle::StringEscape)},
        {"number",       id(TurStyle::Number)},
        {"boolean",      id(TurStyle::Boolean)},
        {"nil",          id(TurStyle::Nil)},
        {"keywordLit",   id(TurStyle::KeywordLit)},
        {"charLit",      id(TurStyle::CharLit)},
        {"metadata",     id(TurStyle::Metadata)},
        {"quote",        id(TurStyle::Quote)},
        {"operator",     id(TurStyle::Operator)},
        {"define",       id(TurStyle::Define)},
        {"control",      id(TurStyle::Control)},
        {"type",         id(TurStyle::Type)},
        {"effect",       id(TurStyle::Effect)},
        {"except",       id(TurStyle::Except)},
        {"special",      id(TurStyle::Special)},
        {"builtin",      id(TurStyle::Builtin)},
        {"cblock",       id(TurStyle::CBlock)},
        {"langDir",      id(TurStyle::LangDir)},
        {"delim",        id(TurStyle::Delim)},
        {"curlyInfix",   id(TurStyle::CurlyInfix)},
        {"neotericCall", id(TurStyle::NeotericCall)},
        {"identifier",   id(TurStyle::Identifier)},
        {"invalid",      id(TurStyle::Invalid)},
        {"sweetMarker",  id(TurStyle::SweetMarker)},
        {"schemeVector", id(TurStyle::SchemeVector)},
        {"barSymbol",    id(TurStyle::BarSymbol)},
        {"rainbow0",     id(TurStyle::Rainbow0)},
        {"rainbow1",     id(TurStyle::Rainbow1)},
        {"rainbow2",     id(TurStyle::Rainbow2)},
        {"rainbow3",     id(TurStyle::Rainbow3)},
        {"rainbow4",     id(TurStyle::Rainbow4)},
        {"rainbow5",     id(TurStyle::Rainbow5)},
        {"rainbow6",     id(TurStyle::Rainbow6)},
        {"bracketError", id(TurStyle::BracketError)},

        {"c.default",      id(CStyle::Default)},
        {"c.comment",      id(CStyle::Comment)},
        {"c.docComment",   id(CStyle::DocComment)},
        {"c.preproc",      id(CStyle::Preproc)},
        {"c.keyword",      id(CStyle::Keyword)},
        {"c.type",         id(CStyle::Type)},
        {"c.string",       id(CStyle::String)},
        {"c.stringEscape", id(CStyle::StringEscape)},
        {"c.char",         id(CStyle::Char)},
        {"c.number",       id(CStyle::Number)},
        {"c.operator",     id(CStyle::Operator)},
        {"c.identifier",   id(CStyle::Identifier)},

        {"md.default",    id(MdStyle::Default)},
        {"md.heading",    id(MdStyle::Heading)},
        {"md.emphasis",   id(MdStyle::Emphasis)},
        {"md.strong",     id(MdStyle::Strong)},
        {"md.codeSpan",   id(MdStyle::CodeSpan)},
        {"md.fence",      id(MdStyle::Fence)},
        {"md.codeBlock",  id(MdStyle::CodeBlock)},
        {"md.linkText",   id(MdStyle::LinkText)},
        {"md.linkUrl",    id(MdStyle::LinkUrl)},
        {"md.blockquote", id(MdStyle::Blockquote)},
        {"md.listMarker", id(MdStyle::ListMarker)},
        {"md.rule",       id(MdStyle::Rule)},
        {"md.html",       id(MdStyle::Html)},
        {"md.escape",     id(MdStyle::Escape)},

        {"json.default",      id(JsonStyle::Default)},
        {"json.key",          id(JsonStyle::Key)},
        {"json.string",       id(JsonStyle::String)},
        {"json.stringEscape", id(JsonStyle::StringEscape)},
        {"json.number",       id(JsonStyle::Number)},
        {"json.literal",      id(JsonStyle::Literal)},
        {"json.operator",     id(JsonStyle::Operator)},
        {"json.error",        id(JsonStyle::Error)},

        {"just.default",       id(JustStyle::Default)},
        {"just.comment",       id(JustStyle::Comment)},
        {"just.recipeName",    id(JustStyle::RecipeName)},
        {"just.dependency",    id(JustStyle::Dependency)},
        {"just.parameter",     id(JustStyle::Parameter)},
        {"just.assign",        id(JustStyle::Assign)},
        {"just.interpolation", id(JustStyle::Interpolation)},
        {"just.backtick",      id(JustStyle::Backtick)},
        {"just.keyword",       id(JustStyle::Keyword)},
        {"just.string",        id(JustStyle::String)},
        {"just.number",        id(JustStyle::Number)},
        {"just.body",          id(JustStyle::Body)},
        {"just.attribute",     id(JustStyle::Attribute)},
        {"just.operator",      id(JustStyle::Operator)},

        {"cmake.default",      id(CMakeStyle::Default)},
        {"cmake.comment",      id(CMakeStyle::Comment)},
        {"cmake.command",      id(CMakeStyle::Command)},
        {"cmake.keyword",      id(CMakeStyle::Keyword)},
        {"cmake.variable",     id(CMakeStyle::Variable)},
        {"cmake.string",       id(CMakeStyle::String)},
        {"cmake.stringEscape", id(CMakeStyle::StringEscape)},
        {"cmake.number",       id(CMakeStyle::Number)},
        {"cmake.operator",     id(CMakeStyle::Operator)},
        {"cmake.identifier",   id(CMakeStyle::Identifier)},

        {"toml.default",      id(TomlStyle::Default)},
        {"toml.comment",      id(TomlStyle::Comment)},
        {"toml.table",        id(TomlStyle::Table)},
        {"toml.key",          id(TomlStyle::Key)},
        {"toml.string",       id(TomlStyle::String)},
        {"toml.stringEscape", id(TomlStyle::StringEscape)},
        {"toml.number",       id(TomlStyle::Number)},
        {"toml.boolean",      id(TomlStyle::Boolean)},
        {"toml.dateTime",     id(TomlStyle::DateTime)},
        {"toml.operator",     id(TomlStyle::Operator)},
        {"toml.error",        id(TomlStyle::Error)},

        {"sh.default",      id(ShStyle::Default)},
        {"sh.comment",      id(ShStyle::Comment)},
        {"sh.keyword",      id(ShStyle::Keyword)},
        {"sh.builtin",      id(ShStyle::Builtin)},
        {"sh.function",     id(ShStyle::Function)},
        {"sh.string",       id(ShStyle::String)},
        {"sh.stringEscape", id(ShStyle::StringEscape)},
        {"sh.variable",     id(ShStyle::Variable)},
        {"sh.number",       id(ShStyle::Number)},
        {"sh.operator",     id(ShStyle::Operator)},
        {"sh.backtick",     id(ShStyle::Backtick)},
        {"sh.identifier",   id(ShStyle::Identifier)},

        {"py.default",      id(PyStyle::Default)},
        {"py.comment",      id(PyStyle::Comment)},
        {"py.keyword",      id(PyStyle::Keyword)},
        {"py.builtin",      id(PyStyle::Builtin)},
        {"py.decorator",    id(PyStyle::Decorator)},
        {"py.className",    id(PyStyle::ClassName)},
        {"py.funcName",     id(PyStyle::FuncName)},
        {"py.string",       id(PyStyle::String)},
        {"py.stringEscape", id(PyStyle::StringEscape)},
        {"py.tripleString", id(PyStyle::TripleString)},
        {"py.number",       id(PyStyle::Number)},
        {"py.operator",     id(PyStyle::Operator)},
        {"py.identifier",   id(PyStyle::Identifier)},
    };
    return m;
}

}

QFont MonospaceUiFont() {
    // Iosevka when the user has it, Menlo otherwise: Menlo ships with macOS,
    // so it is the floor rather than a preference.
    const QString family =
        QFontDatabase::hasFamily(QStringLiteral("Iosevka")) ? QStringLiteral("Iosevka")
                                                            : QStringLiteral("Menlo");
    QFont f(family, 12);
    f.setFixedPitch(true);
    // Both set, and they do different jobs: fixedPitch is a request, styleHint
    // is the fallback to use if neither family resolves.
    f.setStyleHint(QFont::Monospace);
    return f;
}

Theme LoadBuiltinDarkTheme() {
    Theme t;
    QFile f(":/themes/turmeric-dark.theme.json");
    if (!f.open(QIODevice::ReadOnly)) return t;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    const QJsonObject root = doc.object();

    t.name = root.value("name").toString("Turmeric Dark");

    const QJsonObject ed = root.value("editor").toObject();
    t.editorBg            = parseColor(ed.value("background"),           QColor("#0C0A08"));
    t.editorFg            = parseColor(ed.value("foreground"),           QColor("#EAE0D2"));
    t.caret               = parseColor(ed.value("caret"),                QColor("#D48B1C"));
    t.selectionBg         = parseColor(ed.value("selectionBackground"),  QColor("#D48B1C2E"));
    t.currentLineBg       = parseColor(ed.value("currentLineBackground"),QColor("#111009"));
    t.lineNumberFg        = parseColor(ed.value("lineNumberForeground"), QColor("#453F39"));
    t.lineNumberBg        = parseColor(ed.value("lineNumberBackground"), QColor("#0C0A08"));
    t.activeLineNumberFg  = parseColor(ed.value("activeLineNumberForeground"), QColor("#88796C"));
    t.matchedBraceFg      = parseColor(ed.value("matchedBraceForeground"), QColor("#EFA030"));
    t.matchedBraceBg      = parseColor(ed.value("matchedBraceBackground"), QColor("#D48B1C1F"));
    t.indentGuide         = parseColor(ed.value("indentGuide"),          QColor("#252119"));

    const QJsonObject diagnostics = root.value("diagnostics").toObject();
    t.diagnosticError   = parseColor(diagnostics.value("error"),   QColor("#D9735A"));
    t.diagnosticWarning = parseColor(diagnostics.value("warning"), QColor("#EFA030"));

    const QJsonObject dbg = root.value("debugger").toObject();
    t.debugBreakpoint         = parseColor(dbg.value("breakpoint"),          QColor("#D48B1C"));
    t.debugBreakpointDisabled = parseColor(dbg.value("breakpointDisabled"), QColor("#7A6A55"));
    t.debugCurrentLine        = parseColor(dbg.value("currentLine"),         QColor("#E5484D"));

    // Falls back to the selection colour at a lower alpha, so a theme file
    // written before occurrence highlighting existed still gets a sane wash
    // instead of an invisible or garish one. A wash rather than a fill: the
    // token colours underneath have to stay legible, and a mark that hides the
    // code it marks defeats the purpose.
    QColor occurrenceFallback = t.selectionBg;
    occurrenceFallback.setAlpha(qMin(t.selectionBg.alpha(), 40));
    t.occurrenceHighlight =
        parseColor(ed.value("occurrenceHighlight"), occurrenceFallback);

    const QJsonObject term = root.value("terminal").toObject();
    t.terminalBg     = parseColor(term.value("background"), t.editorBg);
    t.terminalFg     = parseColor(term.value("foreground"), t.editorFg);
    t.terminalCaret  = parseColor(term.value("caret"),      t.caret);

    // Fallback ANSI palette (VGA-ish) used when the theme JSON omits colors.
    static const char* kFallbackAnsi[16] = {
        "#000000","#800000","#008000","#808000","#000080","#800080","#008080","#C0C0C0",
        "#808080","#FF0000","#00FF00","#FFFF00","#0000FF","#FF00FF","#00FFFF","#FFFFFF",
    };
    const QJsonArray ansi = term.value("ansi").toArray();
    for (int i = 0; i < 16; ++i) {
        t.ansi[i] = (i < ansi.size())
            ? parseColor(ansi.at(i), QColor(kFallbackAnsi[i]))
            : QColor(kFallbackAnsi[i]);
    }

    const QJsonObject styles = root.value("styles").toObject();
    for (auto it = styles.begin(); it != styles.end(); ++it) {
        t.styles.insert(it.key(), parseStyle(it.value()));
    }
    return t;
}

void ApplyThemeToEditor(ScintillaEdit* sci, const Theme& theme) {
    // Force the widget palette (Active + Inactive + Disabled) to match the
    // theme background. Without this, macOS swaps a system Base color under
    // the viewport when focus changes and any transparent Scintilla styling
    // lets it bleed through — that's what caused "black on black when focused,
    // fine when unfocused".
    QPalette p = sci->palette();
    for (auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        p.setColor(group, QPalette::Base,       theme.editorBg);
        p.setColor(group, QPalette::Window,     theme.editorBg);
        p.setColor(group, QPalette::Text,       theme.editorFg);
        p.setColor(group, QPalette::WindowText, theme.editorFg);
    }
    sci->setPalette(p);
    if (auto* vp = sci->viewport()) vp->setPalette(p);
    if (auto* parent = sci->parentWidget()) {
        parent->setAutoFillBackground(true);
        parent->setPalette(p);
    }

    // Base default style — set fg/bg, then styleClearAll so unset styles inherit.
    sci->styleSetFore(STYLE_DEFAULT, bgra(theme.editorFg));
    sci->styleSetBack(STYLE_DEFAULT, bgra(theme.editorBg));
    sci->styleClearAll();

    for (auto it = theme.styles.begin(); it != theme.styles.end(); ++it) {
        const auto styleIt = StyleKeyMap().find(it.key());
        if (styleIt == StyleKeyMap().end()) continue;
        setStyle(sci, styleIt.value(), it.value(), theme.editorBg);
    }

    // Line-number margin.
    sci->styleSetFore(STYLE_LINENUMBER, bgra(theme.lineNumberFg));
    sci->styleSetBack(STYLE_LINENUMBER, bgra(theme.lineNumberBg));

    // Caret + selection + caret-line highlight.
    sci->setCaretFore(bgra(theme.caret));
    sci->setSelBack(true, bgra(theme.selectionBg));
    sci->setSelAlpha(theme.selectionBg.alpha());
    sci->setCaretLineBack(bgra(theme.currentLineBg));
    sci->setCaretLineBackAlpha(48);  // translucent so text on the line stays readable

    // Diagnostic squiggles and their gutter markers. The marker gets a hollow
    // look (fore = fill color, back = editor background) so it reads as a dot
    // in the margin rather than a solid block.
    sci->indicSetFore(diag::kErrorIndicator, bgra(theme.diagnosticError));
    sci->indicSetFore(diag::kWarningIndicator, bgra(theme.diagnosticWarning));
    sci->markerSetFore(diag::kErrorMarker, bgra(theme.diagnosticError));
    sci->markerSetBack(diag::kErrorMarker, bgra(theme.diagnosticError));
    sci->markerSetFore(diag::kWarningMarker, bgra(theme.diagnosticWarning));
    sci->markerSetBack(diag::kWarningMarker, bgra(theme.diagnosticWarning));
    sci->setMarginBackN(margins::kGutter, bgra(theme.editorBg));

    // Fold markers: filled triangles in the line-number colour, with the
    // editor background behind them so the margin reads as transparent.
    // fore = triangle fill, back = editor bg (no visible stroke).
    const int foldFore = bgra(theme.lineNumberFg);
    const int foldBack = bgra(theme.editorBg);
    for (int m : {SC_MARKNUM_FOLDER, SC_MARKNUM_FOLDEROPEN,
                  SC_MARKNUM_FOLDEREND, SC_MARKNUM_FOLDEROPENMID}) {
        sci->markerSetFore(m, foldFore);
        sci->markerSetBack(m, foldBack);
    }
    // Scintilla draws the fold margin with a dithered checkerboard pattern
    // by default (selbar/selbarlight).  Override both the fill and the
    // stripe colour with the editor background so the margin is solid and
    // matches the body.
    sci->setFoldMarginColour(true, foldBack);
    sci->setFoldMarginHiColour(true, foldBack);
    sci->setMarginBackN(2, foldBack);

    // Debugger gutter markers. The breakpoint marker is a filled circle
    // (fore = fill, back = editor bg so it reads as a dot, not a block); the
    // disabled/pending marker is hollow (back = editor bg, fore = the dim
    // outline). The current-line marker tints the whole line as a background
    // and draws a short arrow in the breakpoint margin.
    sci->markerSetFore(dbg::kBreakpointMarker, bgra(theme.debugBreakpoint));
    sci->markerSetBack(dbg::kBreakpointMarker, bgra(theme.debugBreakpoint));
    sci->markerSetFore(dbg::kBreakpointDisabledMarker, bgra(theme.debugBreakpointDisabled));
    sci->markerSetBack(dbg::kBreakpointDisabledMarker, bgra(theme.editorBg));
    // A filled dot where the program is stopped; a hollow one for a frame the
    // user has merely selected. Distinct in hue from the amber breakpoint so
    // the two read apart at a glance even though they sit in adjacent margins.
    sci->markerSetFore(dbg::kCurrentLineMarker, bgra(theme.debugCurrentLine));
    sci->markerSetBack(dbg::kCurrentLineMarker, bgra(theme.debugCurrentLine));
    sci->markerSetFore(dbg::kSelectedFrameMarker, bgra(theme.debugCurrentLine));
    sci->markerSetBack(dbg::kSelectedFrameMarker, bgra(theme.editorBg));
    // Stopped on a breakpoint: one dot carrying both states, since they share
    // a margin. `fore` is the ring — the breakpoint's own colour, dim when the
    // breakpoint is disabled or pending — and `back` is the fill, the
    // execution marker's red. Reading outward: where the program is, sitting
    // inside the thing that stopped it.
    sci->markerSetFore(dbg::kBreakpointStoppedMarker, bgra(theme.debugBreakpoint));
    sci->markerSetBack(dbg::kBreakpointStoppedMarker, bgra(theme.debugCurrentLine));
    sci->markerSetFore(dbg::kBreakpointDisabledStoppedMarker,
                       bgra(theme.debugBreakpointDisabled));
    sci->markerSetBack(dbg::kBreakpointDisabledStoppedMarker,
                       bgra(theme.debugCurrentLine));

    // Occurrences: a translucent rounded box under the text, not a squiggle.
    // Painted under the text so the syntax colours stay on top and readable.
    sci->indicSetStyle(occurrence::kIndicator, INDIC_ROUNDBOX);
    sci->indicSetFore(occurrence::kIndicator, bgra(theme.occurrenceHighlight));
    sci->indicSetAlpha(occurrence::kIndicator, theme.occurrenceHighlight.alpha());
    sci->indicSetOutlineAlpha(occurrence::kIndicator,
                              qMin(theme.occurrenceHighlight.alpha() * 2, 255));
    sci->indicSetUnder(occurrence::kIndicator, true);

    // Find/replace match highlighting.
    sci->indicSetStyle(find::kMatchIndicator, INDIC_ROUNDBOX);
    sci->indicSetFore(find::kMatchIndicator, 0x888800);  // yellow-ish
    sci->indicSetAlpha(find::kMatchIndicator, 60);
    sci->indicSetOutlineAlpha(find::kMatchIndicator, 120);
    sci->indicSetUnder(find::kMatchIndicator, true);

    sci->indicSetStyle(find::kCurrentIndicator, INDIC_ROUNDBOX);
    sci->indicSetFore(find::kCurrentIndicator, 0xCCCC00);  // brighter yellow
    sci->indicSetAlpha(find::kCurrentIndicator, 120);
    sci->indicSetOutlineAlpha(find::kCurrentIndicator, 200);
    sci->indicSetUnder(find::kCurrentIndicator, true);

    // Matched brace.
    sci->styleSetFore(STYLE_BRACELIGHT, bgra(theme.matchedBraceFg));
    sci->styleSetBack(STYLE_BRACELIGHT, bgra(theme.matchedBraceBg));
    sci->styleSetFore(STYLE_BRACEBAD, bgra(theme.matchedBraceFg));
}

void ApplyThemeToTerminal(TerminalView* terminal, const Theme& theme) {
    if (!terminal) return;
    QPalette p = terminal->palette();
    p.setColor(QPalette::Base, theme.terminalBg);
    p.setColor(QPalette::Text, theme.terminalFg);
    terminal->setPalette(p);
    terminal->setStyleSheet(QString(
        "QPlainTextEdit {"
        "  background-color: %1;"
        "  color: %2;"
        "  border: none;"
        "  padding: 4px;"
        "}"
    ).arg(theme.terminalBg.name(), theme.terminalFg.name()));
    terminal->setCursorWidth(2);

    std::array<QColor, 16> ansi;
    for (int i = 0; i < 16; ++i) ansi[i] = theme.ansi[i];
    terminal->setTerminalPalette(theme.terminalFg, theme.terminalBg, ansi);
}

}
