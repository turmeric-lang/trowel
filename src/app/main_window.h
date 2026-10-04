#pragma once

#include "editor/dialect.h"
#include "lsp/lsp_location.h"
#include "lsp/lsp_symbol.h"
#include "trace/trace_runner.h"
#include "repl/run_buffer.h"

#include <QFont>
#include <QMainWindow>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

#include <memory>
#include <vector>

class QActionGroup;
class QAction;
class QDragEnterEvent;
class QDropEvent;
class QMenu;
class QBoxLayout;
class QScrollArea;
class QSplitter;
class QStackedWidget;

namespace trowel {

class BreakpointModel;
class DebugSession;
class DirectoryView;
class EditorView;
class ProjectRunner;
class ReplPane;
class TraceRunner;
class ReplSession;
class TabBar;
class TabContent;
class TerminalView;
class WindowManager;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    bool openPath(const QString& path);
    bool openDirectory(const QString& path);

    // Two-phase startup. The constructor builds UI only and leaves the window
    // with no buffers and no REPL; callers may then restoreSession() and/or
    // openPath() before startSession() finalizes things. Splitting it this way
    // is what lets a *new* window come up blank while the launch path still
    // restores, and it lets the REPL root at whatever the window actually ends
    // up holding rather than at whatever it held mid-construction.
    //
    // Every window must get a startSession(); WindowManager::newWindow() does
    // it for you.
    void startSession();

    // This window's slice of the persisted session: geometry, splitter, REPL
    // visibility, open buffers and which was active. Written and read by
    // WindowManager, which owns the array of them.
    QVariantMap sessionState() const;
    void applySessionState(const QVariantMap& state);

    // Handle a set of dropped paths. The first replaces the current tab (or
    // simply opens, when there is no tab to replace or the file is already
    // open here); any others open as additional tabs. Returns false if the
    // user cancelled an unsaved-changes prompt.
    bool openDropped(const QStringList& paths);

    EditorView* editorView() const;
    // One entry per open tab, in tab order; empty string for an Untitled or
    // otherwise pathless buffer. Exposed for the control socket so callers can
    // see the tab set, not just the active buffer.
    QStringList tabPaths() const;
    TerminalView* terminalView() const { return terminal_; }
    ReplSession* replSession() const { return repl_; }
    DebugSession* debugSession() const { return debug_; }
    BreakpointModel* breakpointModel() const { return breakpoints_; }
    ReplPane* replPane() const { return replPane_; }
    // Set before invoking debugBuffer() to control whether the next session
    // stops at the entry frame. Reset to false after each launch.
    void setDebugStopOnEntry(bool stop) { debugStopOnEntry_ = stop; }
    QSplitter* splitter() const { return splitter_; }

    // The registry this window belongs to, set by WindowManager::createWindow().
    // Windows need it to spawn siblings (File ▸ New Window); it is null only for
    // a window built outside the registry.
    void setWindowManager(WindowManager* windows);
    WindowManager* windowManager() const { return windows_; }

public slots:
    // Ask where the symbol under the caret is defined and jump there.
    //
    // Public because the control API drives it directly rather than through the
    // menu: it has to connect to definitionJumpFinished before the request goes
    // out, which menu.invoke gives it no chance to do.
    void goToDiagnosticSource();
    void goToDefinition();
    // Record an execution trace of the active buffer with `tur trace`.
    void traceBuffer();
    // Show the document outline. Public for the same reason as goToDefinition:
    // the control API awaits outlineReady before the request goes out.
    void showOutline();
    // List every use of the symbol at the caret, across the workspace.
    void rebuildDialectMenu();
    void showSignatureHelp();
    void findSymbolInProject();
    void findSymbolInProjectFor(const QString& query);
    void findReferences();
    // Ask whether the symbol at the caret can be renamed, and if so open the
    // inline input. The rename itself happens when that input is committed.
    void renameSymbol();

    // Apply a WorkspaceEdit to open buffers, opening tabs for documents that
    // have none. Returns the number of documents changed, or -1 on failure with
    // `error` set — in which case *nothing* was applied.
    //
    // Public so the control API can drive the mechanism without going through
    // the confirmation prompt, which is UI policy rather than part of applying.
    int applyWorkspaceEdit(const LspWorkspaceEdit& edit, QString* error);

signals:
    // A go-to-definition round trip finished. `jumped` is false when the server
    // had no answer or the target could not be opened.
    //
    // Exists so a caller can await the jump without sleeping: the reply is
    // asynchronous, and connecting before issuing the request is the only
    // race-free way to observe it. The smoke suite's nav.goto_definition is
    // built on this.
    void definitionJumpFinished(bool jumped);
    // The outline finished loading. `symbols` is what the server returned, in
    // document order; `reason` is empty on success and otherwise names the
    // state that was shown instead of a list.
    void outlineReady(const QVector<LspSymbol>& symbols, const QString& reason);
    // Workspace-symbol search finished. Empty spans plus a reason when the
    // search could not run or matched nothing — the same shape referencesReady
    // uses, so the control API can tell those apart from "still in flight".
    void workspaceSymbolsReady(const QVector<LspSpan>& spans, const QString& reason);
    // A references lookup finished. `reason` is empty on success.
    void referencesReady(const QVector<LspSpan>& spans, const QString& reason);
    // A rename round trip finished. `changedDocuments` is 0 when nothing was
    // applied; `message` carries the server's refusal verbatim when it refused.
    void renameFinished(int changedDocuments, const QString& message);
    // prepareRename came back affirmative and the inline input is now open.
    // The success counterpart to renameFinished's failure arms, so a caller can
    // await "the rename UI settled" without polling for a visible widget.
    void renameInputOpened();
    // A `tur trace` run finished. Carries the outcome rather than only the
    // step count, because "2 steps" without the reason is what makes a user
    // conclude the tracer is broken.
    void traceFinished(TraceOutcome outcome, const TraceSummary& summary,
                       const QString& explanation);

protected:
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private slots:
    void newFile();
    void newWindow();
    void quitApp();
    void openFile();
    void openDirectoryDialog();
    bool save();
    bool saveAs();
    Dialect replDialectForActiveBuffer() const;
    void restartRepl();
    void restartReplInDirectory();
    void clearRepl();
    void focusEditor();
    void focusRepl();
    void toggleReplEditorFocus();
    void runBuffer();
    void runProject();

    void runSelection();
    // Launch the active buffer under the interpreter debugger (`tur dap`).
    // A debug session is a separate sibling process from the REPL (there is
    // no `attach`), so anything loaded into the REPL is not visible to it.
    void debugBuffer();
    // What F5 does: Continue when a session is paused, Debug Buffer otherwise.
    // Kept separate from `debugBuffer` so the control socket's `debug.start`
    // still means "start", unconditionally.
    void debugOrContinue();
    // Record the active buffer's run and step through it in both directions
    // (`launch` with `"replay": true`). Reverse execution is only served from
    // a recording; `evaluate` is only served outside one.
    void replayBuffer();
    // Respawn the current session against the same file in the same mode.
    // `tur dap` has no `restart` request and runs one program per session
    // (constraint 3), so a restart is a new process, not a rewind.
    void restartDebug();
    // Toggle a breakpoint on the line holding the caret. The margin click is
    // the other way in; this one is discoverable from the menu and works
    // without hunting for a 14px gutter strip.
    void toggleBreakpointAtCaret();
    void formatFile();
    void requestCompletion();
    void showDocumentation();
    void navigateBack();
    void navigateForward();
    void restartLanguageServer();
    // Reflect the active buffer's diagnostics in the status bar: the message
    // under the caret if there is one, otherwise a count.
    void updateDiagnosticStatus();
    void pickFont();
    void openRecentFromAction();
    void toggleSplitOrientation();
    void toggleReplVisible(bool visible);
    void nextTab();
    void prevTab();
    void closeCurrentTab();
    void openPreferences();
    void applyRainbowBrackets(bool enabled);
    void applyBracketPairGuides(bool enabled);
    void rebuildWindowMenu();

public:
    // One visited caret position, for Back/Forward.
    //
    // A path and a byte offset, never an EditorView*: a tab can be dragged to
    // another window and closed there, so a pointer parked in a history stack
    // is a dangling pointer waiting to happen. Public because the control API
    // reads the stacks back for the smoke tests.
    struct NavEntry {
        QString path;
        int pos = 0;
    };

    const QVector<NavEntry>& navBackStack() const { return navBack_; }
    const QVector<NavEntry>& navForwardStack() const { return navForward_; }

    // True for a file inside the bundled stdlib. Such a buffer opens read-only
    // and stays out of both recent files and the persisted session.
    static bool isStdlibPath(const QString& path);

private:
    struct Buffer {
        TabContent* view = nullptr;
        QString displayName;
        int untitledIndex = 0;
    };

    void setupUi();
    void setupMenus();
    void setupToolBar();
    // Add one icon button (or a separator) to the vertical side bar. The button
    // takes its icon, tooltip, enabled and checked state from `action`.
    void addSideBarAction(QAction* action);
    void addSideBarSeparator();
    void updateWindowTitle();
    bool maybeSaveBuffer(int index);
    bool maybeSaveAll();
    // App-wide settings (recent files, editor font) as opposed to the
    // per-window state in sessionState().
    void persistGlobals();
    QString replWorkingDir() const;
    void openSettingsDirectory(const QString& relPath);

    void loadRecentFiles();
    void rebuildRecentMenu();
    void rememberRecentFile(const QString& path);

    int indexOfView(TabContent* v) const;
    // Index of the editor tab already showing `absPath`, or -1.
    int indexOfPath(const QString& absPath) const;
    // Open a path as a file or a directory, whichever it is.
    bool openAny(const QString& path);
    // Replace tab `index` with `path`, swapping the view kind if needed.
    bool replaceBufferWithPath(int index, const QString& path);
    bool replaceBufferWithFile(int index, const QString& path);
    bool replaceBufferWithDirectory(int index, const QString& path);
    void updateEditorActionsEnabled();

    // Send the caret to a resolved definition, activating or opening a tab as
    // the location requires. An invalid location is reported in the status bar
    // rather than silently doing nothing.
    void jumpToDefinition(const LspLocation& location);
    // Where the caret is right now, for pushing onto the back stack.
    NavEntry currentNavEntry() const;
    // Restore a visited position, reopening its file if it has since been
    // closed. False when the file no longer exists, which the caller treats as
    // "skip this entry" rather than as an error.
    bool goToNavEntry(const NavEntry& entry);
    void updateNavActionsEnabled();
    // Push the caret onto the back stack and drop the forward stack. Every
    // jump this window makes goes through here, so Back is uniform across
    // go-to-definition, the outline and the references list.
    void pushNavHistory(const NavEntry& origin);
    // Open `span`'s file if needed and select its range.
    void jumpToSpan(const LspSpan& span);
    // Issue the rename and apply what comes back. Split out of the commit
    // handler so the confirmation prompt stays in one place.
    void applyRename(EditorView* view, const QString& newName);
    // What the run/evaluate action means for the active tab. Disabled for a
    // non-Turmeric document, Project for a build.tur manifest.
    EvalMode currentEvalMode() const;
    int nextUntitledIndex() const;
    void refreshTabBar();
    // Repaint breakpoint gutter markers for the editor showing `path` (or all
    // editors when `path` is empty), driven by BreakpointModel::changed.
    void refreshBreakpointMarkers(const QString& path);
    // Rebuild the Debugger tab's breakpoints panel from the model, including
    // the basename-collision warning rows.
    void refreshBreakpointPanel();
    // Push the program file's breakpoints to the live debug session.
    void pushBreakpointsToSession();
    // Drop the execution-line and selected-frame markers from every open
    // editor. Called on resume, on exit, and before moving the marker to a
    // frame that may be in a different buffer.
    void clearExecutionLines();
    // Move the execution / selected-frame marker to `frameId`'s source line,
    // activating that buffer if it is open. Frame 0 gets the solid arrow
    // ("stopped here"); any other frame gets the hollow one ("looking here").
    void showSelectedFrame(int frameId);
    // The body both Debug Buffer and Time-Travel Debug run: gate, save, tear
    // down any live session, build a new one, wire it, launch.
    void startDebugSession(bool replay);
    QString computeDisplayName(const Buffer& buf) const;
    void updateBufferDisplayName(int index);
    Buffer* addBuffer(const QString& path, bool untitledIfEmpty);
    void activateBuffer(int index);
    void closeBuffer(int index);
    void ensureAtLeastOneBuffer();
    bool saveBuffer(int index);
    bool saveBufferAs(int index);
    void connectBufferSignals(int index);

    WindowManager* windows_ = nullptr;
    int activeIndex_ = -1;
    std::vector<std::unique_ptr<Buffer>> buffers_;

    QStackedWidget* editorStack_ = nullptr;
    TabBar* tabBar_ = nullptr;
    TerminalView* terminal_ = nullptr;
    ReplPane* replPane_ = nullptr;
    ReplSession* repl_ = nullptr;
    DebugSession* debug_ = nullptr;
    BreakpointModel* breakpoints_ = nullptr;
    // The depth ribbon describes the whole recording, so its sites are
    // fetched once per session rather than on every seek.
    bool timelineSitesLoaded_ = false;
    bool debugStopOnEntry_ = false;
    QSplitter* splitter_ = nullptr;
    QMenu* recentMenu_ = nullptr;
    QMenu* windowMenu_ = nullptr;
    ProjectRunner* projectRunner_ = nullptr;
    TraceRunner* traceRunner_ = nullptr;
    // Vertical icon bar pinned to the upper-left. The scroll area exists purely
    // so an overflowing button stack can still be wheeled through; its
    // scrollbars are always off, so no chrome is ever painted.
    QScrollArea* sideBar_ = nullptr;
    QBoxLayout* sideBarLayout_ = nullptr;
    QAction* runBufferAction_ = nullptr;
    QAction* runSelectionAction_ = nullptr;
    QAction* restartReplAction_ = nullptr;
    QAction* clearReplAction_ = nullptr;
    QAction* formatFileAction_ = nullptr;
    QAction* completeAction_ = nullptr;
    QAction* showDocAction_ = nullptr;
    QAction* gotoDefinitionAction_ = nullptr;
    QAction* diagnosticSourceAction_ = nullptr;
    QAction* outlineAction_ = nullptr;
    QAction* signatureHelpAction_ = nullptr;
    QMenu* dialectMenu_ = nullptr;
    QActionGroup* dialectGroup_ = nullptr;
    QAction* workspaceSymbolAction_ = nullptr;
    QAction* findReferencesAction_ = nullptr;
    QAction* renameAction_ = nullptr;
    QAction* traceAction_ = nullptr;
    QAction* debugAction_ = nullptr;
    QAction* replayAction_ = nullptr;
    QAction* restartDebugAction_ = nullptr;
    QAction* toggleBreakpointAction_ = nullptr;
    // Backing store for the references chooser: the rows shown are strings, so
    // the spans they stand for have to live somewhere the selection can reach.
    QVector<LspSpan> referenceSpans_;
    QAction* navBackAction_ = nullptr;
    QAction* navForwardAction_ = nullptr;
    QAction* restartLspAction_ = nullptr;
    QAction* toggleSplitAction_ = nullptr;
    QAction* toggleReplAction_ = nullptr;
    QAction* saveAction_ = nullptr;
    QAction* saveAsAction_ = nullptr;
    QAction* pickFontAction_ = nullptr;
    QStringList recentFiles_;
    QVector<NavEntry> navBack_;
    QVector<NavEntry> navForward_;
    QFont editorFont_;
};

}
