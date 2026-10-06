#pragma once

#include <QColor>
#include <QFont>
#include <QHash>
#include <QString>

class ScintillaEdit;

namespace trowel {

class TerminalView;

struct StyleSpec {
    QColor fg;
    QColor bg;
    bool bold = false;
    bool italic = false;
};

struct Theme {
    QString name;
    QColor editorBg;
    QColor editorFg;
    QColor caret;
    QColor selectionBg;
    QColor currentLineBg;
    QColor lineNumberFg;
    QColor lineNumberBg;
    QColor activeLineNumberFg;
    QColor matchedBraceFg;
    QColor matchedBraceBg;
    QColor indentGuide;

    // Diagnostic squiggles and gutter markers.
    QColor diagnosticError;
    QColor diagnosticWarning;

    // Debugger gutter markers and the current-execution-line tint.
    QColor debugBreakpoint;
    QColor debugBreakpointDisabled;
    QColor debugCurrentLine;

    // Wash painted over every occurrence of the symbol at the caret. Falls back
    // to selectionBg at low alpha when a theme omits it, so existing user theme
    // files keep working.
    QColor occurrenceHighlight;

    QColor terminalBg;
    QColor terminalFg;
    QColor terminalCaret;
    // Standard 16-color ANSI palette: 0-7 normal, 8-15 bright.
    QColor ansi[16];

    // Minimap (phase 1). Each falls back when absent so a theme file
    // written before the minimap existed keeps working — see
    // LoadBuiltinDarkTheme for the fallbacks.
    QColor minimapBg;
    QColor minimapSliderBg;
    QColor minimapSliderHoverBg;
    QColor minimapSliderActiveBg;

    QHash<QString, StyleSpec> styles;
};

// The monospace font every preformatted surface renders in — the REPL
// terminal and the debugger's console and evaluate line.
//
// One function, because the two had drifted: the debugger hardcoded "Menlo"
// and inherited whatever point size the widget defaulted to (13 on macOS),
// while the REPL asked for Iosevka-or-Menlo at 12. The same program output was
// a point larger in one pane than the other, in a different face.
QFont MonospaceUiFont();

// Load the built-in "Turmeric Dark" theme from the Qt resource bundle.
Theme LoadBuiltinDarkTheme();

// Apply the given theme to a Scintilla editor. Only the style slots defined
// in the theme are touched; the caller is responsible for the base font.
void ApplyThemeToEditor(ScintillaEdit* sci, const Theme& theme);

void ApplyThemeToTerminal(TerminalView* terminal, const Theme& theme);

// Style id -> foreground QColor, for consumers that paint text themselves
// (the minimap) rather than handing colors to Scintilla. Built over the
// existing StyleKeyMap; the table is a copy, so mutating it does not affect
// the theme.
QHash<int, QColor> StyleForegroundTable(const Theme& theme);

}
