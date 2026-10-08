#include "platform/shortcuts.h"

#include <QtGlobal>

namespace trowel {

namespace {

// On macOS, "Ctrl" in a QKeySequence string maps to the Cmd (⌘) key, and
// "Meta" maps to the physical Control (⌃) key.  Everywhere else "Ctrl" is
// Control and "Meta" is the Windows/Super key.
//
// The plan uses these conventions:
//   macOS:  ⌘ = Cmd (Qt "Ctrl"), ⌃ = Control (Qt "Meta"), ⌥ = Alt, ⇧ = Shift
//   Linux/Win: Ctrl = Control, Alt = Alt, Shift = Shift

QList<QKeySequence> mac(Command cmd) {
    switch (cmd) {
    // --- File ---
    case Command::New:              return {QKeySequence("Ctrl+N")};
    case Command::NewWindow:        return {QKeySequence("Ctrl+Shift+N")};
    case Command::Open:              return {QKeySequence("Ctrl+O")};
    case Command::OpenDirectory:     return {QKeySequence("Ctrl+Shift+O")};
    case Command::CloseTab:          return {QKeySequence("Ctrl+W")};
    case Command::CloseWindow:      return {QKeySequence("Ctrl+Shift+W")};
    case Command::Save:              return {QKeySequence("Ctrl+S")};
    case Command::SaveAs:            return {QKeySequence("Ctrl+Shift+S")};
    case Command::Quit:              return {QKeySequence("Ctrl+Q")};

    // --- Edit ---
    case Command::Undo:              return {QKeySequence("Ctrl+Z")};
    case Command::Redo:              return {QKeySequence("Ctrl+Shift+Z")};
    case Command::Cut:               return {QKeySequence("Ctrl+X")};
    case Command::Copy:              return {QKeySequence("Ctrl+C")};
    case Command::Paste:             return {QKeySequence("Ctrl+V")};
    case Command::SelectAll:          return {QKeySequence("Ctrl+A")};
    case Command::ToggleComment:      return {QKeySequence("Ctrl+/")};
    case Command::Indent:             return {QKeySequence("Ctrl+]")};
    case Command::Outdent:            return {QKeySequence("Ctrl+[")};
    case Command::FormatFile:         return {QKeySequence("Ctrl+Shift+Alt+F")};
    case Command::CompleteSymbol:     return {QKeySequence("Meta+Space")};
    case Command::ShowDocumentation:   return {QKeySequence("Ctrl+Shift+D")};
    case Command::ShowSignatureHelp:   return {QKeySequence("Ctrl+Shift+P")};
    case Command::RenameSymbol:       return {QKeySequence("F2")};
    case Command::Settings:           return {QKeySequence("Ctrl+,")};
    case Command::TurmericSettings:    return {};
    case Command::ExperimentFlags:   return {};

    // --- Edit > Find (Phase 3) ---
    case Command::Find:               return {QKeySequence("Ctrl+F")};
    case Command::FindReplace:        return {QKeySequence("Ctrl+Alt+F")};
    case Command::FindNext:           return {QKeySequence("Ctrl+G")};
    case Command::FindPrevious:       return {QKeySequence("Ctrl+Shift+G")};
    case Command::UseSelectionForFind: return {QKeySequence("Ctrl+E")};
    case Command::SelectAllOccurrences: return {QKeySequence("Meta+Ctrl+G")};

    // --- View ---
    case Command::ShowRepl:           return {QKeySequence("Ctrl+Alt+R")};
    case Command::ToggleSplit:        return {};
    case Command::WordWrap:           return {QKeySequence("Alt+Z")};
    case Command::Minimap:            return {QKeySequence("Alt+M")};
    case Command::ZoomIn:             return {QKeySequence("Ctrl+=")};
    case Command::ZoomOut:            return {QKeySequence("Ctrl+-")};
    case Command::ActualSize:         return {QKeySequence("Ctrl+0")};
    case Command::NextTab:            return {QKeySequence("Ctrl+Shift+]"),
                                             QKeySequence("Meta+Tab")};
    case Command::PreviousTab:        return {QKeySequence("Ctrl+Shift+["),
                                             QKeySequence("Meta+Shift+Tab")};
    case Command::FullScreen:         return {QKeySequence("Meta+Ctrl+F")};

    // --- View > Folding (Phase 5) ---
    case Command::Fold:               return {QKeySequence("Alt+Ctrl+[")};
    case Command::Unfold:             return {QKeySequence("Alt+Ctrl+]")};
    case Command::ToggleFold:         return {QKeySequence("Ctrl+K, Ctrl+L")};
    case Command::FoldAll:            return {QKeySequence("Ctrl+K, Ctrl+0")};
    case Command::UnfoldAll:          return {QKeySequence("Ctrl+K, Ctrl+J")};
    case Command::FoldTopLevel:       return {QKeySequence("Ctrl+K, Ctrl+1")};

    // --- Go ---
    case Command::GoToDefinition:     return {QKeySequence("F12")};
    case Command::FindReferences:      return {QKeySequence("Shift+F12")};
    case Command::ShowSymbols:         return {QKeySequence("Ctrl+Shift+M")};
    case Command::FindSymbolInProject:  return {QKeySequence("Ctrl+Shift+J")};
    case Command::GoToLine:            return {QKeySequence("Ctrl+L")};
    case Command::GoToDiagnosticSource: return {};
    case Command::Back:                return {QKeySequence("Meta+-")};
    case Command::Forward:             return {QKeySequence("Meta+Shift+-")};

    // --- Run ---
    case Command::RunBuffer:           return {QKeySequence("Ctrl+R")};
    case Command::RunSelection:       return {QKeySequence("Ctrl+Shift+E")};
    case Command::TraceBuffer:         return {QKeySequence("Ctrl+Shift+T")};
    case Command::DebugBuffer:         return {QKeySequence("F5")};
    case Command::TimeTravelDebug:     return {QKeySequence("Ctrl+F5")};
    case Command::RestartDebug:        return {QKeySequence("Ctrl+Shift+F5")};
    case Command::ToggleBreakpoint:    return {QKeySequence("F9")};
    case Command::RestartRepl:        return {QKeySequence("Ctrl+Shift+R")};
    case Command::RestartReplIn:       return {};
    case Command::ClearRepl:           return {QKeySequence("Ctrl+Shift+K")};
    case Command::RestartLanguageServer: return {};

    // --- Window ---
    case Command::Minimize:            return {QKeySequence("Ctrl+M")};
    case Command::Zoom:                return {};
    case Command::FocusEditor:         return {QKeySequence("Ctrl+Alt+E")};
    case Command::FocusRepl:           return {QKeySequence("Ctrl+T")};
    case Command::ToggleReplEditorFocus: return {QKeySequence("Meta+`")};
    case Command::BringAllToFront:     return {};

    // --- Help ---
    case Command::TrowelHelp:          return {};
    case Command::KeyboardShortcuts:   return {};
    case Command::TurmericDocumentation: return {};
    case Command::ReportIssue:         return {};
    case Command::About:               return {};

    // Terminal-only.
    case Command::TerminalCopy:        return {};
    case Command::TerminalPaste:       return {};
    }
    return {};
}

[[maybe_unused]]
QList<QKeySequence> other(Command cmd) {
    // Linux and Windows share the same tree; the only differences are
    // handled by QKeySequence::standard shortcuts and the Quit/Exit label.
    switch (cmd) {
    // --- File ---
    case Command::New:              return {QKeySequence::New};
    case Command::NewWindow:        return {QKeySequence("Ctrl+Shift+N")};
    case Command::Open:              return {QKeySequence::Open};
    case Command::OpenDirectory:     return {QKeySequence("Ctrl+Shift+O")};
    case Command::CloseTab:          return {QKeySequence("Ctrl+W")};
    case Command::CloseWindow:      return {QKeySequence("Ctrl+Shift+W")};
    case Command::Save:              return {QKeySequence::Save};
    case Command::SaveAs:            return {QKeySequence::SaveAs};
    case Command::Quit:              return {QKeySequence("Ctrl+Q")};

    // --- Edit ---
    case Command::Undo:              return {QKeySequence::Undo};
    case Command::Redo:              return {QKeySequence::Redo};
    case Command::Cut:               return {QKeySequence::Cut};
    case Command::Copy:              return {QKeySequence::Copy};
    case Command::Paste:             return {QKeySequence::Paste};
    case Command::SelectAll:          return {QKeySequence::SelectAll};
    case Command::ToggleComment:      return {QKeySequence("Ctrl+/")};
    case Command::Indent:             return {QKeySequence("Ctrl+]")};
    case Command::Outdent:            return {QKeySequence("Ctrl+[")};
    case Command::FormatFile:         return {QKeySequence("Ctrl+Shift+Alt+F")};
    case Command::CompleteSymbol:     return {QKeySequence("Ctrl+Space")};
    case Command::ShowDocumentation:   return {QKeySequence("Ctrl+Shift+D")};
    case Command::ShowSignatureHelp:   return {QKeySequence("Ctrl+Shift+P")};
    case Command::RenameSymbol:       return {QKeySequence("F2")};
    case Command::Settings:           return {QKeySequence("Ctrl+,")};
    case Command::TurmericSettings:    return {};
    case Command::ExperimentFlags:   return {};

    // --- Edit > Find (Phase 3) ---
    case Command::Find:               return {QKeySequence("Ctrl+F")};
    case Command::FindReplace:        return {QKeySequence("Ctrl+H")};
    case Command::FindNext:           return {QKeySequence("F3"),
                                             QKeySequence("Ctrl+G")};
    case Command::FindPrevious:       return {QKeySequence("Shift+F3")};
    case Command::UseSelectionForFind: return {};
    case Command::SelectAllOccurrences: return {QKeySequence("Ctrl+Shift+L")};

    // --- View ---
    case Command::ShowRepl:           return {QKeySequence("Ctrl+Alt+R")};
    case Command::ToggleSplit:        return {};
    case Command::WordWrap:           return {QKeySequence("Alt+Z")};
    case Command::Minimap:            return {QKeySequence("Alt+M")};
    case Command::ZoomIn:             return {QKeySequence("Ctrl+=")};
    case Command::ZoomOut:            return {QKeySequence("Ctrl+-")};
    case Command::ActualSize:         return {QKeySequence("Ctrl+0")};
    case Command::NextTab:            return {QKeySequence("Ctrl+Tab"),
                                             QKeySequence("Ctrl+PgDown")};
    case Command::PreviousTab:        return {QKeySequence("Ctrl+Shift+Tab"),
                                             QKeySequence("Ctrl+PgUp")};
    case Command::FullScreen:         return {QKeySequence("F11")};

    // --- View > Folding (Phase 5) ---
    case Command::Fold:               return {QKeySequence("Ctrl+Shift+[")};
    case Command::Unfold:             return {QKeySequence("Ctrl+Shift+]")};
    case Command::ToggleFold:         return {QKeySequence("Ctrl+K, Ctrl+L")};
    case Command::FoldAll:            return {QKeySequence("Ctrl+K, Ctrl+0")};
    case Command::UnfoldAll:          return {QKeySequence("Ctrl+K, Ctrl+J")};
    case Command::FoldTopLevel:       return {QKeySequence("Ctrl+K, Ctrl+1")};

    // --- Go ---
    case Command::GoToDefinition:     return {QKeySequence("F12")};
    case Command::FindReferences:      return {QKeySequence("Shift+F12")};
    case Command::ShowSymbols:         return {QKeySequence("Ctrl+Shift+M")};
    case Command::FindSymbolInProject:  return {QKeySequence("Ctrl+Shift+J")};
    case Command::GoToLine:            return {QKeySequence("Ctrl+L")};
    case Command::GoToDiagnosticSource: return {};
    case Command::Back:                return {QKeySequence("Ctrl+Alt+-")};
    case Command::Forward:             return {QKeySequence("Ctrl+Alt+Shift+-")};

    // --- Run ---
    case Command::RunBuffer:           return {QKeySequence("Ctrl+R")};
    case Command::RunSelection:       return {QKeySequence("Ctrl+Shift+E")};
    case Command::TraceBuffer:         return {QKeySequence("Ctrl+Shift+T")};
    case Command::DebugBuffer:         return {QKeySequence("F5")};
    case Command::TimeTravelDebug:     return {QKeySequence("Ctrl+F5")};
    case Command::RestartDebug:        return {QKeySequence("Ctrl+Shift+F5")};
    case Command::ToggleBreakpoint:    return {QKeySequence("F9")};
    case Command::RestartRepl:        return {QKeySequence("Ctrl+Shift+R")};
    case Command::RestartReplIn:       return {};
    case Command::ClearRepl:           return {QKeySequence("Ctrl+Shift+K")};
    case Command::RestartLanguageServer: return {};

    // --- Window ---
    case Command::Minimize:            return {};
    case Command::Zoom:                return {};
    case Command::FocusEditor:         return {QKeySequence("Ctrl+E")};
    case Command::FocusRepl:           return {QKeySequence("Ctrl+T")};
    case Command::ToggleReplEditorFocus: return {QKeySequence("Ctrl+`")};
    case Command::BringAllToFront:     return {};

    // --- Help ---
    case Command::TrowelHelp:          return {};
    case Command::KeyboardShortcuts:   return {};
    case Command::TurmericDocumentation: return {};
    case Command::ReportIssue:         return {};
    case Command::About:               return {};

    // Terminal-only.
    case Command::TerminalCopy:        return {QKeySequence("Ctrl+Shift+C")};
    case Command::TerminalPaste:       return {QKeySequence("Ctrl+Shift+V")};
    }
    return {};
}

}  // namespace

QList<QKeySequence> ShortcutsFor(Command cmd) {
#ifdef Q_OS_MACOS
    return mac(cmd);
#else
    return other(cmd);
#endif
}
const char* CommandName(Command cmd) {
    switch (cmd) {
    case Command::New:              return "New";
    case Command::NewWindow:        return "NewWindow";
    case Command::Open:              return "Open";
    case Command::OpenDirectory:     return "OpenDirectory";
    case Command::CloseTab:          return "CloseTab";
    case Command::CloseWindow:      return "CloseWindow";
    case Command::Save:              return "Save";
    case Command::SaveAs:            return "SaveAs";
    case Command::Quit:              return "Quit";

    case Command::Undo:              return "Undo";
    case Command::Redo:              return "Redo";
    case Command::Cut:               return "Cut";
    case Command::Copy:              return "Copy";
    case Command::Paste:             return "Paste";
    case Command::SelectAll:          return "SelectAll";
    case Command::ToggleComment:      return "ToggleComment";
    case Command::Indent:             return "Indent";
    case Command::Outdent:            return "Outdent";
    case Command::FormatFile:         return "FormatFile";
    case Command::CompleteSymbol:     return "CompleteSymbol";
    case Command::ShowDocumentation:   return "ShowDocumentation";
    case Command::ShowSignatureHelp:   return "ShowSignatureHelp";
    case Command::RenameSymbol:       return "RenameSymbol";
    case Command::Settings:           return "Settings";
    case Command::TurmericSettings:    return "TurmericSettings";
    case Command::ExperimentFlags:   return "ExperimentFlags";

    case Command::Find:               return "Find";
    case Command::FindReplace:        return "FindReplace";
    case Command::FindNext:           return "FindNext";
    case Command::FindPrevious:       return "FindPrevious";
    case Command::UseSelectionForFind: return "UseSelectionForFind";
    case Command::SelectAllOccurrences: return "SelectAllOccurrences";

    case Command::ShowRepl:           return "ShowRepl";
    case Command::ToggleSplit:        return "ToggleSplit";
    case Command::WordWrap:           return "WordWrap";
    case Command::Minimap:            return "Minimap";
    case Command::ZoomIn:             return "ZoomIn";
    case Command::ZoomOut:            return "ZoomOut";
    case Command::ActualSize:         return "ActualSize";
    case Command::NextTab:            return "NextTab";
    case Command::PreviousTab:        return "PreviousTab";
    case Command::FullScreen:         return "FullScreen";

    case Command::Fold:               return "Fold";
    case Command::Unfold:             return "Unfold";
    case Command::ToggleFold:         return "ToggleFold";
    case Command::FoldAll:            return "FoldAll";
    case Command::UnfoldAll:          return "UnfoldAll";
    case Command::FoldTopLevel:       return "FoldTopLevel";

    case Command::GoToDefinition:     return "GoToDefinition";
    case Command::FindReferences:      return "FindReferences";
    case Command::ShowSymbols:         return "ShowSymbols";
    case Command::FindSymbolInProject:  return "FindSymbolInProject";
    case Command::GoToLine:            return "GoToLine";
    case Command::GoToDiagnosticSource: return "GoToDiagnosticSource";
    case Command::Back:                return "Back";
    case Command::Forward:             return "Forward";

    case Command::RunBuffer:           return "RunBuffer";
    case Command::RunSelection:       return "RunSelection";
    case Command::TraceBuffer:         return "TraceBuffer";
    case Command::DebugBuffer:         return "DebugBuffer";
    case Command::TimeTravelDebug:     return "TimeTravelDebug";
    case Command::RestartDebug:        return "RestartDebug";
    case Command::ToggleBreakpoint:    return "ToggleBreakpoint";
    case Command::RestartRepl:        return "RestartRepl";
    case Command::RestartReplIn:       return "RestartReplIn";
    case Command::ClearRepl:           return "ClearRepl";
    case Command::RestartLanguageServer: return "RestartLanguageServer";

    case Command::Minimize:            return "Minimize";
    case Command::Zoom:                return "Zoom";
    case Command::FocusEditor:         return "FocusEditor";
    case Command::FocusRepl:           return "FocusRepl";
    case Command::ToggleReplEditorFocus: return "ToggleReplEditorFocus";
    case Command::BringAllToFront:     return "BringAllToFront";

    case Command::TrowelHelp:          return "TrowelHelp";
    case Command::KeyboardShortcuts:   return "KeyboardShortcuts";
    case Command::TurmericDocumentation: return "TurmericDocumentation";
    case Command::ReportIssue:         return "ReportIssue";
    case Command::About:               return "About";

    case Command::TerminalCopy:        return "TerminalCopy";
    case Command::TerminalPaste:       return "TerminalPaste";
    }
    return "";
}

}  // namespace trowel
