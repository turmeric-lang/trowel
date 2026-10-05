#pragma once

#include <QList>
#include <QKeySequence>

namespace trowel {

// One entry per menu command, including commands added by later phases
// (Find, Folding) so every key is chosen in one place.  The enum is the
// single source of truth for what shortcut a command gets on each platform;
// setupMenus() calls ShortcutsFor() rather than writing key strings inline.
enum class Command {
    // --- File ---
    New,
    NewWindow,
    Open,
    OpenDirectory,
    CloseTab,
    CloseWindow,
    Save,
    SaveAs,
    Quit,

    // --- Edit ---
    Undo,
    Redo,
    Cut,
    Copy,
    Paste,
    SelectAll,
    ToggleComment,
    Indent,
    Outdent,
    FormatFile,
    CompleteSymbol,
    ShowDocumentation,
    ShowSignatureHelp,
    RenameSymbol,
    Settings,
    TurmericSettings,

    // --- Edit > Find (Phase 3) ---
    Find,
    FindReplace,
    FindNext,
    FindPrevious,
    UseSelectionForFind,
    SelectAllOccurrences,

    // --- View ---
    ShowRepl,
    ToggleSplit,
    WordWrap,        // Phase 4
    ZoomIn,
    ZoomOut,
    ActualSize,
    NextTab,
    PreviousTab,
    FullScreen,

    // --- View > Folding (Phase 5) ---
    Fold,
    Unfold,
    ToggleFold,
    FoldAll,
    UnfoldAll,
    FoldTopLevel,

    // --- Go ---
    GoToDefinition,
    FindReferences,
    ShowSymbols,
    FindSymbolInProject,
    GoToLine,
    GoToDiagnosticSource,
    Back,
    Forward,

    // --- Run ---
    RunBuffer,
    RunSelection,
    TraceBuffer,
    DebugBuffer,
    TimeTravelDebug,
    RestartDebug,
    ToggleBreakpoint,
    RestartRepl,
    RestartReplIn,
    ClearRepl,
    RestartLanguageServer,

    // --- Window ---
    Minimize,
    Zoom,
    FocusEditor,
    FocusRepl,
    ToggleReplEditorFocus,
    BringAllToFront,

    // --- Help ---
    TrowelHelp,
    KeyboardShortcuts,
    TurmericDocumentation,
    ReportIssue,
    About,

    // Terminal-only (not in the menu bar).
    TerminalCopy,
    TerminalPaste,
};

// The platform-specific shortcut list for `cmd`.
//
// Returns an empty list when the command has no shortcut on this platform.
// This is the only #ifdef Q_OS_MACOS / Q_OS_WIN in the menu change.
QList<QKeySequence> ShortcutsFor(Command cmd);

// A human-readable name for the command, used by the control API (menu.list)
// and the docs drift check.  Stable across releases.
const char* CommandName(Command cmd);

}  // namespace trowel
