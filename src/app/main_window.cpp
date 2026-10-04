#include "app/main_window.h"

#include "app/directory_view.h"
#include "app/icon_font.h"
#include "app/preferences_view.h"
#include "app/tab_bar.h"
#include "app/tab_content.h"
#include "app/window_manager.h"
#include "app/repl_pane.h"
#include "debug/breakpoint_model.h"
#include "debug/debug_session.h"
#include "debug/debugger_view.h"
#include "debug/timeline_strip.h"
#include "editor/editor_view.h"
#include "editor/theme_loader.h"
#include "lsp/lsp_manager.h"
#include "repl/project_runner.h"
#include "trace/trace_runner.h"
#include "repl/repl_session.h"

#include <ScintillaEdit.h>
#include "repl/run_buffer.h"
#include "repl/terminal_view.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QFileDialog>
#include <QInputDialog>
#include <QActionGroup>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFontDialog>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QSettings>
#include <QDir>
#include <QFrame>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStringList>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

namespace trowel {

namespace {

// The shortcut for Go to Diagnostic Source, named once because the status-bar
// framing for a dependency error quotes it -- a hint that spells a different
// key than the menu does is worse than no hint.
//
// Shift+F12 rather than something near F12: it reads as "the other F12 jump",
// and F12 itself is Go to Definition.
constexpr const char* kGoToDiagnosticSourceShortcut = "Shift+F12";
// Depth of the Back stack. Deep enough that a normal exploration session never
// hits it, shallow enough that it stays a navigation aid rather than a log.
constexpr int kNavHistoryMax = 20;
// Above this many documents, a rename asks first and lists them. A cross-file
// rename is the most destructive thing this editor can do, and the number of
// files is the only advance warning the user gets about how far it reaches.
constexpr int kRenameConfirmThreshold = 20;
}  // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    const QString preferred = QFontDatabase::hasFamily("Iosevka") ? "Iosevka" : "Menlo";
    editorFont_ = QSettings().value("editorFont", QFont(preferred, 12)).value<QFont>();

    setupUi();
    setupMenus();
    setupToolBar();
    loadRecentFiles();
    updateEditorActionsEnabled();
    updateNavActionsEnabled();
    updateWindowTitle();
    // No buffers and no REPL yet — see startSession().
}

MainWindow::~MainWindow() {
    // QWidget's base-class teardown (not this destructor body) is what
    // deletes the child editor widgets, and it runs *after* buffers_ (a
    // member) is already gone. Destroying an EditorView fires
    // LspManager::closeDocument() -> diagnosticsUpdated, and that's still
    // connected to `this` — Qt only severs a QObject's connections in
    // ~QObject, later still. Without this, the connected lambda calls
    // editorView(), which indexes the already-destroyed buffers_. Cut that
    // connection now, before any of that teardown starts.
    disconnect(LspManager::instance(), nullptr, this, nullptr);
}

EditorView* MainWindow::editorView() const {
    if (activeIndex_ < 0 || activeIndex_ >= static_cast<int>(buffers_.size())) return nullptr;
    TabContent* v = buffers_[activeIndex_]->view;
    if (!v || v->kind() != TabContent::Kind::Editor) return nullptr;
    return static_cast<EditorView*>(v);
}

QStringList MainWindow::tabPaths() const {
    QStringList paths;
    for (const auto& b : buffers_) {
        paths << (b->view ? b->view->filePath() : QString());
    }
    return paths;
}

void MainWindow::setupUi() {
    splitter_ = new QSplitter(Qt::Horizontal, this);

    editorStack_ = new QStackedWidget(splitter_);
    // The terminal is created first so ReplPane can reparent it into its
    // stack; `terminal_` keeps pointing at the same widget, so every
    // `repl.*` handler and the control socket keep working untouched.
    terminal_ = new TerminalView(splitter_);
    replPane_ = new ReplPane(terminal_, splitter_);

    splitter_->addWidget(editorStack_);
    splitter_->addWidget(replPane_);
    splitter_->setChildrenCollapsible(false);
    splitter_->setSizes({700, 500});

    ApplyThemeToTerminal(terminal_, LoadBuiltinDarkTheme());

    // Central widget: the vertical icon bar on the left, then a column holding
    // the tab bar over the editor/REPL splitter. The bar lives in the central
    // widget rather than a QMainWindow toolbar area so it can start flush with
    // the top-left corner *and* be wrapped in a scroll area — a QToolBar in a
    // dock area answers overflow with an extension popup, not scrolling.
    auto* central = new QWidget(this);
    auto* hbox = new QHBoxLayout(central);
    hbox->setContentsMargins(0, 0, 0, 0);
    hbox->setSpacing(0);

    const Theme theme = LoadBuiltinDarkTheme();

    auto* sideBarBody = new QWidget;
    sideBarLayout_ = new QVBoxLayout(sideBarBody);
    sideBarLayout_->setContentsMargins(4, 4, 4, 4);
    sideBarLayout_->setSpacing(6);
    // Buttons pack from the top; the stretch keeps them there when the window
    // is taller than the stack.
    sideBarLayout_->addStretch(1);

    sideBar_ = new QScrollArea(central);
    sideBar_->setWidget(sideBarBody);
    sideBar_->setWidgetResizable(true);
    sideBar_->setFrameShape(QFrame::NoFrame);
    // Wheel/trackpad scrolling still works with the bars switched off — Qt
    // keeps the (hidden) scrollbar's range and routes wheel events to it.
    sideBar_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sideBar_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sideBar_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    // The buttons are flat by design: no frame in any state, and a background
    // only to show that a checkable action is currently on. `:checked` never
    // matches a non-checkable button, so plain actions stay bare throughout.
    sideBar_->setStyleSheet(QString(
        "QScrollArea, QScrollArea > QWidget > QWidget { background: %1; }"
        "QScrollArea { border: none; border-right: 1px solid %2; }"
        "QToolButton { border: none; background: transparent; }"
        "QToolButton:checked { background: rgba(%3, %4, %5, %6); }"
    ).arg(theme.editorBg.name(), theme.lineNumberFg.name())
     // rgba(), not name(): the selection color carries an alpha that name()
     // would drop, leaving the toggle a solid slab instead of a wash.
     .arg(theme.selectionBg.red()).arg(theme.selectionBg.green())
     .arg(theme.selectionBg.blue()).arg(theme.selectionBg.alpha()));

    auto* column = new QWidget(central);
    auto* vbox = new QVBoxLayout(column);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    tabBar_ = new TabBar(column);
    tabBar_->setColors(theme.editorBg, theme.editorFg, theme.lineNumberFg);
    tabBar_->setActiveFg(theme.matchedBraceFg);

    vbox->addWidget(tabBar_);
    vbox->addWidget(splitter_, 1);

    hbox->addWidget(sideBar_);
    hbox->addWidget(column, 1);

    setCentralWidget(central);
    resize(1200, 800);
    setAcceptDrops(true);

    // Hidden until something is actually being said, and hidden again the
    // moment it stops. 33 call sites do `statusBar()->show()` before a timed
    // `showMessage`, and nothing put it back — so the first transient message
    // of a session left an empty bar wedged across the bottom of the window
    // for the rest of it.
    //
    // `messageChanged` fires with an empty string both when a timed message
    // expires and when one is cleared, which is exactly the condition.
    statusBar()->setSizeGripEnabled(false);
    // No border, and the theme's colours rather than Qt's defaults.
    //
    // The default draws a rule along the bar's bottom edge — which is the
    // window's bottom edge, so it lands directly against the macOS window
    // bevel and reads as a rendering seam rather than as a divider. There is
    // nothing below it to divide it from.
    //
    // `QStatusBar::item` is set separately: it is the frame Qt puts around
    // each message widget, and it survives a border rule aimed at the bar.
    statusBar()->setStyleSheet(QString(
        "QStatusBar { background: %1; color: %2; border: none; }"
        "QStatusBar::item { border: none; }"
    ).arg(theme.editorBg.name(), theme.editorFg.name()));
    statusBar()->hide();
    connect(statusBar(), &QStatusBar::messageChanged, this,
            [this](const QString& text) {
        if (text.isEmpty()) statusBar()->hide();
    });

    connect(tabBar_, &TabBar::activateRequested, this, [this](int idx) {
        activateBuffer(idx);
    });
    connect(tabBar_, &TabBar::closeRequested, this, [this](int idx) {
        closeBuffer(idx);
    });
}

void MainWindow::setupMenus() {
    auto* fileMenu = menuBar()->addMenu("&File");

    auto* newAction = fileMenu->addAction("&New");
    newAction->setShortcut(QKeySequence::New);
    connect(newAction, &QAction::triggered, this, &MainWindow::newFile);

    auto* newWindowAction = fileMenu->addAction("New &Window");
    newWindowAction->setShortcut(QKeySequence("Ctrl+Shift+N"));
    connect(newWindowAction, &QAction::triggered, this, &MainWindow::newWindow);

    auto* openAction = fileMenu->addAction("&Open…");
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::openFile);

    auto* openDirAction = fileMenu->addAction("Open &Directory…");
    openDirAction->setShortcut(QKeySequence("Ctrl+Shift+O"));
    connect(openDirAction, &QAction::triggered, this, &MainWindow::openDirectoryDialog);

    recentMenu_ = fileMenu->addMenu("Open &Recent");
    rebuildRecentMenu();

    fileMenu->addSeparator();

    saveAction_ = fileMenu->addAction("&Save");
    saveAction_->setShortcut(QKeySequence::Save);
    connect(saveAction_, &QAction::triggered, this, [this]{ save(); });

    saveAsAction_ = fileMenu->addAction("Save &As…");
    saveAsAction_->setShortcut(QKeySequence::SaveAs);
    connect(saveAsAction_, &QAction::triggered, this, [this]{ saveAs(); });

    fileMenu->addSeparator();

    auto* closeTabAction = fileMenu->addAction("&Close Tab");
    closeTabAction->setShortcut(QKeySequence("Ctrl+W"));
    connect(closeTabAction, &QAction::triggered, this, &MainWindow::closeCurrentTab);

    auto* closeWindowAction = fileMenu->addAction("Close Win&dow");
    closeWindowAction->setShortcut(QKeySequence("Ctrl+Shift+W"));
    connect(closeWindowAction, &QAction::triggered, this, &QMainWindow::close);

    fileMenu->addSeparator();

    auto* quitAction = fileMenu->addAction("&Quit");
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &MainWindow::quitApp);

    menuBar()->addMenu("&Edit");

    auto* viewMenu = menuBar()->addMenu("&View");
    pickFontAction_ = viewMenu->addAction("&Font…");
    pickFontAction_->setShortcut(QKeySequence("Ctrl+,"));
    connect(pickFontAction_, &QAction::triggered, this, &MainWindow::pickFont);

    viewMenu->addSeparator();
    auto* nextTabAction = viewMenu->addAction("Ne&xt Tab");
    nextTabAction->setShortcut(QKeySequence("Ctrl+Tab"));
    connect(nextTabAction, &QAction::triggered, this, &MainWindow::nextTab);
    auto* prevTabAction = viewMenu->addAction("&Previous Tab");
    prevTabAction->setShortcut(QKeySequence("Ctrl+Shift+Tab"));
    connect(prevTabAction, &QAction::triggered, this, &MainWindow::prevTab);

    windowMenu_ = menuBar()->addMenu("&Window");
    // Populated from the registry; also refreshed just before it opens so
    // window titles are current even if nothing opened or closed.
    connect(windowMenu_, &QMenu::aboutToShow, this, &MainWindow::rebuildWindowMenu);
    rebuildWindowMenu();

    auto* runMenu = menuBar()->addMenu("&Run");

    runBufferAction_ = new QAction("&Run Buffer", this);
    runBufferAction_->setShortcut(QKeySequence("Ctrl+R"));
    runBufferAction_->setToolTip("Evaluate File");
    connect(runBufferAction_, &QAction::triggered, this, &MainWindow::runBuffer);
    runMenu->addAction(runBufferAction_);

    runSelectionAction_ = new QAction("Run &Selection", this);
    runSelectionAction_->setShortcut(QKeySequence("Ctrl+Shift+E"));
    runSelectionAction_->setToolTip("Evaluate Selection");
    connect(runSelectionAction_, &QAction::triggered, this, &MainWindow::runSelection);
    runMenu->addAction(runSelectionAction_);

    traceAction_ = new QAction("&Trace Buffer", this);
    traceAction_->setShortcut(QKeySequence("Ctrl+Shift+T"));
    traceAction_->setToolTip("Record an execution trace with `tur trace`");
    connect(traceAction_, &QAction::triggered, this, &MainWindow::traceBuffer);
    runMenu->addAction(traceAction_);

    debugAction_ = new QAction("&Debug Buffer", this);
    debugAction_->setShortcut(QKeySequence("F5"));
    debugAction_->setToolTip(
        "Run the current file under the interpreter debugger (`tur dap`)");
    connect(debugAction_, &QAction::triggered, this, &MainWindow::debugOrContinue);
    runMenu->addAction(debugAction_);

    replayAction_ = new QAction("Time-Travel Debu&g", this);
    replayAction_->setShortcut(QKeySequence("Ctrl+F5"));
    replayAction_->setToolTip(
        "Record the run, then step through it in both directions "
        "(`tur dap` with a recording)");
    connect(replayAction_, &QAction::triggered, this, &MainWindow::replayBuffer);
    runMenu->addAction(replayAction_);

    restartDebugAction_ = new QAction("Restart Debug S&ession", this);
    restartDebugAction_->setShortcut(QKeySequence("Ctrl+Shift+F5"));
    restartDebugAction_->setToolTip(
        "Respawn the debug session — `tur dap` runs one program per session, "
        "so a restart is a fresh process");
    connect(restartDebugAction_, &QAction::triggered, this, &MainWindow::restartDebug);
    runMenu->addAction(restartDebugAction_);

    toggleBreakpointAction_ = new QAction("Toggle &Breakpoint", this);
    toggleBreakpointAction_->setShortcut(QKeySequence("F9"));
    toggleBreakpointAction_->setToolTip(
        "Set or clear a breakpoint on the caret's line (or click the gutter)");
    connect(toggleBreakpointAction_, &QAction::triggered,
            this, &MainWindow::toggleBreakpointAtCaret);
    runMenu->addAction(toggleBreakpointAction_);

    runMenu->addSeparator();

    restartReplAction_ = new QAction("Res&tart REPL", this);
    restartReplAction_->setShortcut(QKeySequence("Ctrl+Shift+R"));
    restartReplAction_->setToolTip("Restart the REPL in the current file's directory");
    connect(restartReplAction_, &QAction::triggered, this, &MainWindow::restartRepl);
    runMenu->addAction(restartReplAction_);

    auto* restartReplInAction = runMenu->addAction("Restart REPL &In…");
    restartReplInAction->setToolTip("Pick a directory and restart the REPL there");
    connect(restartReplInAction, &QAction::triggered,
            this, &MainWindow::restartReplInDirectory);

    clearReplAction_ = new QAction("&Clear REPL", this);
    clearReplAction_->setShortcut(QKeySequence("Ctrl+Shift+K"));
    clearReplAction_->setToolTip("Clear REPL Output");
    connect(clearReplAction_, &QAction::triggered, this, &MainWindow::clearRepl);
    runMenu->addAction(clearReplAction_);

    formatFileAction_ = new QAction("&Format File", this);
    formatFileAction_->setShortcut(QKeySequence("Ctrl+Shift+F"));
    formatFileAction_->setToolTip("Format file with `tur fmt`");
    connect(formatFileAction_, &QAction::triggered, this, &MainWindow::formatFile);
    runMenu->addAction(formatFileAction_);

    runMenu->addSeparator();

    completeAction_ = new QAction("Complete S&ymbol", this);
    completeAction_->setShortcut(QKeySequence("Ctrl+Space"));
    completeAction_->setToolTip("Suggest completions at the caret");
    connect(completeAction_, &QAction::triggered, this, &MainWindow::requestCompletion);
    runMenu->addAction(completeAction_);

    showDocAction_ = new QAction("Show &Documentation", this);
    showDocAction_->setShortcut(QKeySequence("Ctrl+Shift+D"));
    showDocAction_->setToolTip("Show the type and docstring of the symbol at the caret");
    connect(showDocAction_, &QAction::triggered, this, &MainWindow::showDocumentation);
    runMenu->addAction(showDocAction_);

    // Signature help, beside Show Documentation: the two answer the same
    // question at different moments, and share the call-tip surface.
    signatureHelpAction_ = new QAction("Show Signature &Help", this);
    signatureHelpAction_->setShortcut(QKeySequence("Ctrl+Shift+P"));
    signatureHelpAction_->setToolTip(
        "Show the parameter list of the call the caret is inside");
    connect(signatureHelpAction_, &QAction::triggered, this,
            &MainWindow::showSignatureHelp);
    runMenu->addAction(signatureHelpAction_);

    // §6.1 of the navigation plan proposes the Edit menu; Run is where it
    // actually goes, because that is where Complete Symbol and Show
    // Documentation already live and Edit is empty. Ctrl+Shift+O is taken by
    // Open Directory (see :195) and is deliberately not reused.
    outlineAction_ = new QAction("Show Sy&mbols", this);
    outlineAction_->setShortcut(QKeySequence("Ctrl+Shift+M"));
    outlineAction_->setToolTip("List the definitions in this file");
    connect(outlineAction_, &QAction::triggered, this, &MainWindow::showOutline);
    runMenu->addAction(outlineAction_);

    // The workspace counterpart of Show Symbols: that one lists this file,
    // this one searches every file the server has indexed.
    workspaceSymbolAction_ = new QAction("Find Symbol in &Project…", this);
    workspaceSymbolAction_->setShortcut(QKeySequence("Ctrl+Shift+T"));
    workspaceSymbolAction_->setToolTip("Search for a definition across the project");
    connect(workspaceSymbolAction_, &QAction::triggered, this,
            &MainWindow::findSymbolInProject);
    runMenu->addAction(workspaceSymbolAction_);

    diagnosticSourceAction_ = new QAction("Go to Diagnostic &Source", this);
    diagnosticSourceAction_->setShortcut(QKeySequence(kGoToDiagnosticSourceShortcut));
    diagnosticSourceAction_->setToolTip(
        "Open the code a dependency error came from");
    connect(diagnosticSourceAction_, &QAction::triggered, this,
            &MainWindow::goToDiagnosticSource);
    runMenu->addAction(diagnosticSourceAction_);

    gotoDefinitionAction_ = new QAction("&Go to Definition", this);
    gotoDefinitionAction_->setShortcut(QKeySequence("F12"));
    gotoDefinitionAction_->setToolTip("Jump to where the symbol at the caret is defined");
    connect(gotoDefinitionAction_, &QAction::triggered, this, &MainWindow::goToDefinition);
    runMenu->addAction(gotoDefinitionAction_);

    findReferencesAction_ = new QAction("Find &References", this);
    findReferencesAction_->setShortcut(QKeySequence("Shift+F12"));
    findReferencesAction_->setToolTip("List every use of the symbol at the caret");
    connect(findReferencesAction_, &QAction::triggered, this, &MainWindow::findReferences);
    runMenu->addAction(findReferencesAction_);

    renameAction_ = new QAction("Re&name Symbol", this);
    renameAction_->setShortcut(QKeySequence("F2"));
    renameAction_->setToolTip("Rename the symbol at the caret across the workspace");
    connect(renameAction_, &QAction::triggered, this, &MainWindow::renameSymbol);
    runMenu->addAction(renameAction_);

    // Ctrl+Alt+Left/Right is a workspace switcher under several Linux desktops,
    // so these use VS Code's alternate pair, which nothing here or there claims.
    navBackAction_ = new QAction("Go &Back", this);
    navBackAction_->setShortcut(QKeySequence("Ctrl+Alt+-"));
    navBackAction_->setToolTip("Return to the position before the last jump");
    connect(navBackAction_, &QAction::triggered, this, &MainWindow::navigateBack);
    runMenu->addAction(navBackAction_);

    navForwardAction_ = new QAction("Go For&ward", this);
    navForwardAction_->setShortcut(QKeySequence("Ctrl+Alt+Shift+-"));
    navForwardAction_->setToolTip("Redo the jump that Back undid");
    connect(navForwardAction_, &QAction::triggered, this, &MainWindow::navigateForward);
    runMenu->addAction(navForwardAction_);

    restartLspAction_ = new QAction("Restart &Language Server", this);
    restartLspAction_->setToolTip("Restart `tur lsp`");
    connect(restartLspAction_, &QAction::triggered, this, &MainWindow::restartLanguageServer);
    runMenu->addAction(restartLspAction_);

    runMenu->addSeparator();

    // --- Dialect picker ---------------------------------------------------
    //
    // Grouped by LANGUAGE with a heading each, which is how the Try Turmeric
    // picker settled after two revisions: a `#lang` base names a (language,
    // reader) pair, so the list reads as readers under a language rather than
    // ten flat rows.
    //
    // Curly-infix and neoteric are deliberately NOT offered: `{a + b}` is
    // enabled in every dialect and neoteric is one of sweet-exp's three tools,
    // so presenting them as dialects of their own misrepresents what they are.
    // Both stay spellable, and a buffer that names one gets its row back
    // (rebuildDialectMenu adds it when the current dialect is a hidden one).
    runMenu->addSeparator();
    dialectMenu_ = runMenu->addMenu("&Dialect");
    dialectGroup_ = new QActionGroup(this);
    dialectGroup_->setExclusive(true);
    connect(dialectGroup_, &QActionGroup::triggered, this, [this](QAction* a) {
        EditorView* v = editorView();
        if (!v || !a) return;
        const auto d = static_cast<Dialect>(a->data().toInt());
        if (!v->setLangDirective(d)) {
            // Already in that dialect and nothing to write -- say so rather
            // than leaving the click looking like it did nothing.
            statusBar()->show();
            statusBar()->showMessage(
                QString("Already %1").arg(DialectBaseToken(d)), 2000);
        }
        rebuildDialectMenu();
    });
    // Rebuilt when the menu opens rather than synced from every signal that
    // could change the answer (tab switch, Save As to a new extension, the user
    // typing a `#lang` line). Those are many and the menu is cheap, so "ask at
    // the moment it is read" cannot go stale the way a subscription can.
    connect(dialectMenu_, &QMenu::aboutToShow, this, &MainWindow::rebuildDialectMenu);
    rebuildDialectMenu();

    auto* focusEditorAction = runMenu->addAction("Focus &Editor");
    focusEditorAction->setShortcut(QKeySequence("Ctrl+E"));
    connect(focusEditorAction, &QAction::triggered, this, &MainWindow::focusEditor);

    auto* focusReplAction = runMenu->addAction("Focus RE&PL");
    focusReplAction->setShortcut(QKeySequence("Ctrl+T"));
    connect(focusReplAction, &QAction::triggered, this, &MainWindow::focusRepl);

    auto* toggleFocusAction = runMenu->addAction("Toggle REPL/Editor &Focus");
    toggleFocusAction->setShortcut(QKeySequence("Ctrl+`"));
    connect(toggleFocusAction, &QAction::triggered, this, &MainWindow::toggleReplEditorFocus);
}

namespace {

// Icon size is fixed at the value the horizontal toolbar used; the button box
// and the bar's width are derived from it so the bar stays exactly one button
// wide however the padding is tuned.
constexpr int kSideBarGlyphSize = 18;
constexpr int kSideBarButtonSize = 30;
// Width of the rule the side bar's stylesheet paints along its right edge.
constexpr int kSideBarDividerWidth = 1;

}  // namespace

void MainWindow::addSideBarAction(QAction* action) {
    if (!action || !sideBarLayout_) return;
    auto* button = new QToolButton(sideBar_->widget());
    // setDefaultAction wires icon, tooltip, checkable/checked *and* the
    // enabled state, so the eval gate greys the button out for free.
    button->setDefaultAction(action);
    button->setIconSize(QSize(kSideBarGlyphSize, kSideBarGlyphSize));
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setAutoRaise(true);
    button->setFixedSize(kSideBarButtonSize, kSideBarButtonSize);
    // Insert before the trailing stretch.
    sideBarLayout_->insertWidget(sideBarLayout_->count() - 1, button);
}

void MainWindow::addSideBarSeparator() {
    if (!sideBarLayout_) return;
    auto* line = new QFrame(sideBar_->widget());
    line->setFrameShape(QFrame::HLine);
    line->setFixedHeight(1);
    line->setStyleSheet(QString("background: %1; border: none;")
                            .arg(LoadBuiltinDarkTheme().lineNumberFg.name()));
    sideBarLayout_->insertWidget(sideBarLayout_->count() - 1, line);
}

void MainWindow::setupToolBar() {
    if (!sideBar_ || !sideBarLayout_) return;

    const Theme theme = LoadBuiltinDarkTheme();
    const QColor iconColor = theme.editorFg;
    const int glyphSize = kSideBarGlyphSize;

    if (runBufferAction_) {
        runBufferAction_->setIcon(NerdIcon(NF::Play, glyphSize, iconColor));
        addSideBarAction(runBufferAction_);
    }
    if (runSelectionAction_) {
        runSelectionAction_->setIcon(NerdIcon(NF::PlaylistPlay, glyphSize, iconColor));
        addSideBarAction(runSelectionAction_);
    }
    if (debugAction_) {
        debugAction_->setIcon(NerdIcon(NF::Play, glyphSize, iconColor));
        addSideBarAction(debugAction_);
    }
    if (restartReplAction_) {
        restartReplAction_->setIcon(NerdIcon(NF::Restart, glyphSize, iconColor));
        addSideBarAction(restartReplAction_);
    }
    if (clearReplAction_) {
        clearReplAction_->setIcon(NerdIcon(NF::Broom, glyphSize, iconColor));
        addSideBarAction(clearReplAction_);
    }
    if (formatFileAction_) {
        formatFileAction_->setIcon(NerdIcon(NF::AutoFix, glyphSize, iconColor));
        addSideBarAction(formatFileAction_);
    }

    if (outlineAction_) {
        outlineAction_->setIcon(NerdIcon(NF::FormatListBulleted, glyphSize, iconColor));
        addSideBarAction(outlineAction_);
    }

    addSideBarSeparator();

    toggleSplitAction_ = new QAction("Toggle Split Orientation", this);
    toggleSplitAction_->setToolTip("Toggle REPL position (right / below)");
    toggleSplitAction_->setIcon(NerdIcon(NF::ViewSplitHorizontal, glyphSize, iconColor));
    connect(toggleSplitAction_, &QAction::triggered, this, &MainWindow::toggleSplitOrientation);
    addSideBarAction(toggleSplitAction_);

    toggleReplAction_ = new QAction("Show/Hide REPL", this);
    toggleReplAction_->setCheckable(true);
    toggleReplAction_->setChecked(true);
    toggleReplAction_->setToolTip("Show/Hide REPL");
    toggleReplAction_->setIcon(NerdIcon(NF::Console, glyphSize, iconColor));
    connect(toggleReplAction_, &QAction::toggled, this, &MainWindow::toggleReplVisible);
    addSideBarAction(toggleReplAction_);

    addSideBarSeparator();

    auto* settingsMenu = new QMenu(this);
    auto* trowelSettingsAction = settingsMenu->addAction("Trowel Settings");
    connect(trowelSettingsAction, &QAction::triggered, this,
            &MainWindow::openPreferences);
    auto* turmericSettingsAction = settingsMenu->addAction("Turmeric Settings");
    connect(turmericSettingsAction, &QAction::triggered, this,
            [this]{ openSettingsDirectory(".config/turmeric"); });

    auto* settingsButton = new QToolButton(sideBar_->widget());
    settingsButton->setToolTip("Settings");
    settingsButton->setIcon(NerdIcon(NF::Cog, glyphSize, iconColor));
    settingsButton->setIconSize(QSize(glyphSize, glyphSize));
    settingsButton->setAutoRaise(true);
    settingsButton->setFixedSize(kSideBarButtonSize, kSideBarButtonSize);
    settingsButton->setMenu(settingsMenu);
    settingsButton->setPopupMode(QToolButton::InstantPopup);
    // No arrow: the indicator would eat into the 18px glyph inside a
    // button this narrow.
    settingsButton->setStyleSheet("QToolButton::menu-indicator { image: none; }");
    sideBarLayout_->insertWidget(sideBarLayout_->count() - 1, settingsButton);

    // Exactly as wide as the stack needs, plus the 1px divider the stylesheet
    // draws on the right — that border comes out of the viewport, so without it
    // the bar would have a permanent one-pixel horizontal scroll range.
    sideBar_->setFixedWidth(sideBarLayout_->minimumSize().width() + kSideBarDividerWidth);
}

void MainWindow::toggleReplVisible(bool visible) {
    // Toggle the whole pane (terminal + debugger + tab bar), not just the
    // terminal — the pane is now the splitter widget.
    if (replPane_) replPane_->setVisible(visible);
}

void MainWindow::toggleSplitOrientation() {
    if (!splitter_) return;
    const bool wasHorizontal = splitter_->orientation() == Qt::Horizontal;
    splitter_->setOrientation(wasHorizontal ? Qt::Vertical : Qt::Horizontal);
    if (toggleSplitAction_) {
        const QColor iconColor = LoadBuiltinDarkTheme().editorFg;
        const char32_t glyph = wasHorizontal ? NF::ViewSplitVertical : NF::ViewSplitHorizontal;
        toggleSplitAction_->setIcon(NerdIcon(glyph, 18, iconColor));
    }
    const int total = wasHorizontal ? splitter_->height() : splitter_->width();
    if (total > 0) {
        const int first = static_cast<int>(total * 0.58);
        splitter_->setSizes({first, total - first});
    }
}

int MainWindow::nextUntitledIndex() const {
    int max = 0;
    for (const auto& b : buffers_) {
        if (b->untitledIndex > max) max = b->untitledIndex;
    }
    return max + 1;
}

int MainWindow::indexOfPath(const QString& absPath) const {
    for (int i = 0; i < static_cast<int>(buffers_.size()); ++i) {
        TabContent* v = buffers_[i]->view;
        if (!v || v->kind() != TabContent::Kind::Editor) continue;
        if (v->filePath().isEmpty()) continue;
        if (QFileInfo(v->filePath()).absoluteFilePath() == absPath) return i;
    }
    return -1;
}

bool MainWindow::openAny(const QString& path) {
    return QFileInfo(path).isDir() ? openDirectory(path) : openPath(path);
}

bool MainWindow::replaceBufferWithPath(int index, const QString& path) {
    return QFileInfo(path).isDir() ? replaceBufferWithDirectory(index, path)
                                   : replaceBufferWithFile(index, path);
}

int MainWindow::indexOfView(TabContent* v) const {
    for (int i = 0; i < static_cast<int>(buffers_.size()); ++i) {
        if (buffers_[i]->view == v) return i;
    }
    return -1;
}

QString MainWindow::computeDisplayName(const Buffer& buf) const {
    if (buf.view && buf.view->kind() == TabContent::Kind::Directory) {
        const QString name = buf.view->displayName();
        return name.isEmpty() ? QStringLiteral("Directory") : name;
    }
    if (buf.view && buf.view->kind() == TabContent::Kind::Preferences) {
        return buf.view->displayName();
    }
    if (buf.view && !buf.view->filePath().isEmpty()) {
        return QFileInfo(buf.view->filePath()).fileName();
    }
    if (buf.untitledIndex <= 1) return "Untitled";
    return QString("Untitled %1").arg(buf.untitledIndex);
}

void MainWindow::updateBufferDisplayName(int index) {
    if (index < 0 || index >= static_cast<int>(buffers_.size())) return;
    buffers_[index]->displayName = computeDisplayName(*buffers_[index]);
    refreshTabBar();
}

void MainWindow::refreshTabBar() {
    if (!tabBar_) return;
    QStringList names;
    for (const auto& b : buffers_) names.append(b->displayName);
    tabBar_->setTabs(names, activeIndex_);
    for (int i = 0; i < static_cast<int>(buffers_.size()); ++i) {
        TabContent* v = buffers_[i]->view;
        tabBar_->setModified(i, v && v->isModified());
        const bool readOnly = v && v->kind() == TabContent::Kind::Editor &&
                              static_cast<EditorView*>(v)->isReadOnly();
        tabBar_->setReadOnly(i, readOnly);
        QString tip = v ? v->filePath() : QString();
        // Say *why* it is locked. "(ro)" alone invites a bug report; naming the
        // bundle explains that the file is Trowel's, not the user's.
        if (readOnly) tip += QStringLiteral("\nRead-only: bundled Turmeric stdlib");
        tabBar_->setTooltip(i, tip);
    }
}

void MainWindow::refreshBreakpointMarkers(const QString& path) {
    if (!breakpoints_) return;
    // Repaint markers for the editor showing `path`, or every editor when
    // `path` is empty (a clear-all). A pending breakpoint is one not yet
    // verified by the adapter — rendered hollow so the send-timing delay is
    // visible (constraint 6).
    const bool live = debug_ && debug_->isRunning();
    for (const auto& b : buffers_) {
        if (!b->view || b->view->kind() != TabContent::Kind::Editor) continue;
        auto* ed = static_cast<EditorView*>(b->view);
        if (!path.isEmpty() && ed->filePath() != path) continue;
        QVector<EditorView::BreakpointMark> marks;
        for (const auto& bp : breakpoints_->forFile(ed->filePath())) {
            EditorView::BreakpointMark m;
            m.line = bp.line;
            m.enabled = bp.enabled;
            // Pending while a session is live but has not yet acknowledged
            // this breakpoint (it will after the next stop). Simplified: mark
            // all pending while running, verified-looking while idle.
            m.pending = live;
            marks.append(m);
        }
        ed->setBreakpointMarkers(marks);
    }
    refreshBreakpointPanel();
}

void MainWindow::refreshBreakpointPanel() {
    if (!breakpoints_ || !replPane_ || !replPane_->debugger()) return;

    // `tur dap` reduces a breakpoint's path to its basename before binding it
    // (constraint 5), so two open files with the same name share one
    // breakpoint set inside the interpreter. Detected here and surfaced as a
    // warning row: silently wrong is much worse than loudly limited.
    QStringList openPaths;
    for (const auto& b : buffers_) {
        if (b->view && b->view->kind() == TabContent::Kind::Editor &&
            !b->view->filePath().isEmpty()) {
            openPaths << b->view->filePath();
        }
    }
    QStringList colliding;
    for (const auto& bp : breakpoints_->breakpoints()) {
        if (!BreakpointModel::HasBasenameCollision(bp.path, openPaths)) continue;
        const QString name = QFileInfo(bp.path).fileName();
        if (!colliding.contains(name)) colliding << name;
    }
    replPane_->debugger()->setBreakpoints(breakpoints_->breakpoints(), colliding);
}

void MainWindow::clearExecutionLines() {
    for (const auto& b : buffers_) {
        if (b->view && b->view->kind() == TabContent::Kind::Editor) {
            static_cast<EditorView*>(b->view)->clearExecutionLine();
        }
    }
}

void MainWindow::showSelectedFrame(int frameId) {
    if (!debug_) return;
    const QVector<DebugSession::Frame>& frames = debug_->frames();
    const auto it = std::find_if(frames.cbegin(), frames.cend(),
                                 [frameId](const DebugSession::Frame& f) {
                                     return f.id == frameId;
                                 });
    if (it == frames.cend() || it->filePath.isEmpty()) return;

    // Clear everywhere first: the previous frame may well have been in a
    // different buffer, and a marker left behind in one says the program is
    // stopped in two places at once.
    clearExecutionLines();

    int idx = indexOfPath(it->filePath);
    if (idx < 0) {
        // Open it. A stack that names a file you cannot see is a stack you
        // cannot follow, and the frames the debugger hands back are all real
        // paths on disk (`dap.c` emits `source.path` in full).
        if (!QFileInfo::exists(it->filePath)) return;
        if (!openPath(it->filePath)) return;
        idx = indexOfPath(it->filePath);
        if (idx < 0) return;
    }
    if (idx != activeIndex_) activateBuffer(idx);
    EditorView* ed = editorView();
    if (!ed) return;
    // Frame 0 is where the program actually is; anything else is a frame the
    // user is *looking at*, and gets the hollow arrow instead.
    const bool isTop = !frames.isEmpty() && frames.first().id == frameId;
    ed->setExecutionLine(it->line, isTop);
}

void MainWindow::pushBreakpointsToSession() {
    if (!debug_ || !breakpoints_) return;
    // The *debugged program's* file, not whatever buffer happens to be in
    // front. Switching tabs while paused must not silently re-point the
    // session's breakpoint set at a different file.
    const QString program = debug_->program();
    if (program.isEmpty()) return;
    QVector<DebugSession::BreakpointSpec> bps;
    for (const auto& bp : breakpoints_->forFile(program)) {
        DebugSession::BreakpointSpec s;
        s.line = bp.line;
        s.enabled = bp.enabled;
        s.condition = bp.condition;
        bps.append(s);
    }
    debug_->setBreakpoints(program, bps);
}

void MainWindow::connectBufferSignals(int index) {
    TabContent* view = buffers_[index]->view;
    connect(view, &TabContent::modifiedChanged, this, [this, view](bool modified) {
        const int i = indexOfView(view);
        if (i < 0) return;
        if (tabBar_) tabBar_->setModified(i, modified);
        if (i == activeIndex_) updateWindowTitle();
    });
    connect(view, &TabContent::filePathChanged, this, [this, view](const QString&) {
        const int i = indexOfView(view);
        if (i < 0) return;
        updateBufferDisplayName(i);
        if (i != activeIndex_) return;
        updateWindowTitle();
        // A Save As can turn a plain script into a build.tur (or a .tur into a
        // .txt), so the eval gate has to be re-evaluated on rename too.
        updateEditorActionsEnabled();
    });
    connect(view, &TabContent::displayNameChanged, this, [this, view]() {
        const int i = indexOfView(view);
        if (i < 0) return;
        updateBufferDisplayName(i);
        if (i == activeIndex_) updateWindowTitle();
    });
    if (view->kind() == TabContent::Kind::Directory) {
        auto* dv = static_cast<DirectoryView*>(view);
        connect(dv, &DirectoryView::fileActivated, this, [this, view](const QString& path) {
            const int i = indexOfView(view);
            if (i < 0) return;
            replaceBufferWithFile(i, path);
        });
        return;
    }

    auto* editor = static_cast<EditorView*>(view);
    // Caret movement re-reads the diagnostic under the cursor. updateUi also
    // fires for selection and scroll changes, which is harmless — the readout
    // is cheap and idempotent.
    connect(editor->sciWidget(), &ScintillaEditBase::updateUi, this,
            [this, editor](Scintilla::Update) {
        if (editor == editorView()) updateDiagnosticStatus();
    });
    // Click the breakpoint margin to toggle a breakpoint in the model. The
    // model's `changed` signal flows back to refreshBreakpointMarkers().
    connect(editor, &EditorView::breakpointToggleRequested, this,
            [this, editor](int line) {
        if (!breakpoints_) return;
        const QString path = editor->filePath();
        if (path.isEmpty()) return;  // untitled buffers can't bind breakpoints
        breakpoints_->toggle(path, line);
        // A toggle made while paused can be delivered now; mid-run it cannot
        // be (constraint 6) and waits for the next stop.
        if (debug_ && debug_->state() == DebugSession::State::Paused) {
            pushBreakpointsToSession();
        }
    });
    // Scintilla carries markers across edits; the model is told here so a
    // breakpoint set on a line that has since slid down still means the same
    // statement. Without this the model keeps a bare line number and the
    // marker and the breakpoint quietly disagree.
    connect(editor, &EditorView::breakpointLinesMoved, this,
            [this, editor](const QVector<QPair<int, int>>& moves) {
        if (!breakpoints_ || editor->filePath().isEmpty()) return;
        breakpoints_->applyLineMoves(editor->filePath(), moves);
    });
    // EditorView connects to this signal in its own constructor, so by the time
    // this runs the view has already repainted its indicators.
    connect(LspManager::instance(), &LspManager::diagnosticsUpdated, this,
            [this, editor](const QString&) {
        if (editor == editorView()) updateDiagnosticStatus();
    });
    // The reply may arrive after the user switched tabs. Acting on it then
    // would yank them somewhere they did not ask to go, so it is dropped —
    // the manager's staleness guard covers edits, this covers focus.
    connect(editor, &EditorView::definitionResolved, this,
            [this, editor](const LspLocation& location) {
        if (editor != editorView()) return;
        jumpToDefinition(location);
    });
    // An outline pick is a jump like any other, so it goes on the same history
    // stack — Back after using the outline returns where you were.
    connect(editor, &EditorView::outlineSymbolChosen, this,
            [this, editor](int line, int character) {
        if (editor != editorView()) return;
        const NavEntry origin = currentNavEntry();
        editor->setCursorPos(editor->posFromLineCol(line, character));
        editor->sciWidget()->scrollCaret();
        pushNavHistory(origin);
    });
    // The references chooser reports a row index; the span it stands for lives
    // in referenceSpans_, which was filled when the list was shown.
    connect(editor, &EditorView::listRowChosen, this, [this, editor](int row) {
        if (editor != editorView()) return;
        if (row < 0 || row >= referenceSpans_.size()) return;
        jumpToSpan(referenceSpans_.at(row));
    });
    connect(editor, &EditorView::renameCommitted, this,
            [this, editor](const QString& newName) {
        if (editor != editorView()) return;
        applyRename(editor, newName);
    });
    connect(editor, &EditorView::renameCancelled, this, [this, editor] {
        if (editor != editorView()) return;
        emit renameFinished(0, QStringLiteral("rename cancelled"));
    });
}

MainWindow::Buffer* MainWindow::addBuffer(const QString& path, bool untitledIfEmpty) {
    auto buf = std::make_unique<Buffer>();
    auto* editor = new EditorView(editorStack_);
    editor->setFont(editorFont_);
    if (!path.isEmpty()) {
        if (!editor->loadFile(path)) {
            delete editor;
            return nullptr;
        }
    } else if (untitledIfEmpty) {
        buf->untitledIndex = nextUntitledIndex();
    }
    buf->view = editor;
    editorStack_->addWidget(buf->view);
    buf->displayName = computeDisplayName(*buf);
    buffers_.push_back(std::move(buf));
    const int newIndex = static_cast<int>(buffers_.size()) - 1;
    connectBufferSignals(newIndex);
    return buffers_.back().get();
}

void MainWindow::activateBuffer(int index) {
    if (index < 0 || index >= static_cast<int>(buffers_.size())) return;
    activeIndex_ = index;
    editorStack_->setCurrentWidget(buffers_[index]->view);
    if (tabBar_) tabBar_->setActive(index);
    updateWindowTitle();
    updateEditorActionsEnabled();
    updateDiagnosticStatus();
}

bool MainWindow::maybeSaveBuffer(int index) {
    if (index < 0 || index >= static_cast<int>(buffers_.size())) return true;
    TabContent* v = buffers_[index]->view;
    if (!v || v->kind() != TabContent::Kind::Editor) return true;
    if (!v->isModified()) return true;
    activateBuffer(index);
    const auto choice = QMessageBox::question(
        this, "Trowel",
        QString("Save changes to %1?").arg(buffers_[index]->displayName),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    switch (choice) {
        case QMessageBox::Save: return saveBuffer(index);
        case QMessageBox::Discard: return true;
        default: return false;
    }
}

bool MainWindow::maybeSaveAll() {
    for (int i = 0; i < static_cast<int>(buffers_.size()); ++i) {
        if (!maybeSaveBuffer(i)) return false;
    }
    return true;
}

void MainWindow::closeBuffer(int index) {
    if (index < 0 || index >= static_cast<int>(buffers_.size())) return;
    if (!maybeSaveBuffer(index)) return;

    TabContent* view = buffers_[index]->view;
    editorStack_->removeWidget(view);
    if (view) view->deleteLater();
    buffers_.erase(buffers_.begin() + index);

    if (buffers_.empty()) {
        addBuffer(QString(), /*untitledIfEmpty=*/true);
        activeIndex_ = 0;
    } else if (activeIndex_ >= static_cast<int>(buffers_.size())) {
        activeIndex_ = static_cast<int>(buffers_.size()) - 1;
    } else if (activeIndex_ > index) {
        --activeIndex_;
    } else if (activeIndex_ == index) {
        // Keep same index (now points to what used to be index+1).
        if (activeIndex_ >= static_cast<int>(buffers_.size())) {
            activeIndex_ = static_cast<int>(buffers_.size()) - 1;
        }
    }
    editorStack_->setCurrentWidget(buffers_[activeIndex_]->view);
    refreshTabBar();
    if (tabBar_) tabBar_->setActive(activeIndex_);
    updateWindowTitle();
    // Closing a tab makes a different document active, which may well have a
    // different eval mode.
    updateEditorActionsEnabled();
}

void MainWindow::ensureAtLeastOneBuffer() {
    if (!buffers_.empty()) return;
    addBuffer(QString(), /*untitledIfEmpty=*/true);
    activeIndex_ = 0;
    editorStack_->setCurrentWidget(buffers_[0]->view);
    refreshTabBar();
}

bool MainWindow::openPath(const QString& path) {
    // Already open in this window? Focus that tab instead of making a second
    // one. Deliberately scoped to this window — a file open in *another*
    // window is left alone rather than yanking the user across windows.
    const QString abs = QFileInfo(path).absoluteFilePath();
    if (const int existing = indexOfPath(abs); existing >= 0) {
        activateBuffer(existing);
        rememberRecentFile(abs);
        return true;
    }

    // Reuse current buffer if it's a fresh, empty, unmodified Untitled editor.
    if (activeIndex_ >= 0 && activeIndex_ < static_cast<int>(buffers_.size())) {
        Buffer* cur = buffers_[activeIndex_].get();
        if (cur->view && cur->view->kind() == TabContent::Kind::Editor) {
            auto* ev = static_cast<EditorView*>(cur->view);
            if (ev->filePath().isEmpty() && ev->isEmpty() && !ev->isModified()) {
                if (!ev->loadFile(path)) {
                    QMessageBox::warning(this, "Trowel", QString("Could not open %1").arg(path));
                    return false;
                }
                cur->untitledIndex = 0;
                updateBufferDisplayName(activeIndex_);
                rememberRecentFile(path);
                updateWindowTitle();
                return true;
            }
        }
    }

    Buffer* b = addBuffer(path, /*untitledIfEmpty=*/false);
    if (!b) {
        QMessageBox::warning(this, "Trowel", QString("Could not open %1").arg(path));
        return false;
    }
    activateBuffer(static_cast<int>(buffers_.size()) - 1);
    refreshTabBar();
    rememberRecentFile(path);
    return true;
}

bool MainWindow::openDirectory(const QString& path) {
    QFileInfo info(path);
    if (!info.exists() || !info.isDir()) {
        QMessageBox::warning(this, "Trowel", QString("Not a directory: %1").arg(path));
        return false;
    }
    const QString abs = info.absoluteFilePath();

    // Reuse current buffer if it's a fresh, empty, unmodified Untitled editor.
    if (activeIndex_ >= 0 && activeIndex_ < static_cast<int>(buffers_.size())) {
        Buffer* cur = buffers_[activeIndex_].get();
        if (cur->view && cur->view->kind() == TabContent::Kind::Editor) {
            auto* ev = static_cast<EditorView*>(cur->view);
            if (ev->filePath().isEmpty() && ev->isEmpty() && !ev->isModified()) {
                return replaceBufferWithDirectory(activeIndex_, abs);
            }
        }
    }

    // Otherwise create a new buffer with a directory view.
    auto buf = std::make_unique<Buffer>();
    auto* dv = new DirectoryView(editorStack_);
    buf->view = dv;
    editorStack_->addWidget(dv);
    dv->setRoot(abs);
    buf->displayName = computeDisplayName(*buf);
    buffers_.push_back(std::move(buf));
    const int newIndex = static_cast<int>(buffers_.size()) - 1;
    connectBufferSignals(newIndex);
    activateBuffer(newIndex);
    refreshTabBar();
    return true;
}

bool MainWindow::replaceBufferWithDirectory(int index, const QString& path) {
    if (index < 0 || index >= static_cast<int>(buffers_.size())) return false;
    QFileInfo info(path);
    if (!info.exists() || !info.isDir()) {
        QMessageBox::warning(this, "Trowel", QString("Not a directory: %1").arg(path));
        return false;
    }
    Buffer* buf = buffers_[index].get();
    TabContent* old = buf->view;

    auto* dv = new DirectoryView(editorStack_);
    editorStack_->addWidget(dv);
    editorStack_->removeWidget(old);
    if (old) old->deleteLater();

    buf->view = dv;
    buf->untitledIndex = 0;
    connectBufferSignals(index);
    dv->setRoot(info.absoluteFilePath());

    if (index == activeIndex_) {
        editorStack_->setCurrentWidget(dv);
    }
    updateBufferDisplayName(index);
    updateWindowTitle();
    updateEditorActionsEnabled();
    return true;
}

bool MainWindow::replaceBufferWithFile(int index, const QString& path) {
    if (index < 0 || index >= static_cast<int>(buffers_.size())) return false;
    Buffer* buf = buffers_[index].get();
    TabContent* old = buf->view;

    auto* editor = new EditorView(editorStack_);
    editor->setFont(editorFont_);
    if (!editor->loadFile(path)) {
        delete editor;
        QMessageBox::warning(this, "Trowel", QString("Could not open %1").arg(path));
        return false;
    }
    editorStack_->addWidget(editor);
    editorStack_->removeWidget(old);
    if (old) old->deleteLater();

    buf->view = editor;
    buf->untitledIndex = 0;
    connectBufferSignals(index);

    if (index == activeIndex_) {
        editorStack_->setCurrentWidget(editor);
    }
    updateBufferDisplayName(index);
    rememberRecentFile(path);
    updateWindowTitle();
    updateEditorActionsEnabled();
    return true;
}

EvalMode MainWindow::currentEvalMode() const {
    EditorView* v = editorView();
    if (!v) return EvalMode::Disabled;
    return EvalModeForPath(v->filePath());
}

void MainWindow::updateEditorActionsEnabled() {
    EditorView* active = editorView();
    const bool hasEditor = active != nullptr;
    // Save and Format both write. Offering them on a locked buffer and failing
    // afterwards is the save-error UI §5.3 rejects in favour of the lock.
    // Save As still works — copying a stdlib file somewhere writable is a
    // perfectly good thing to want.
    const bool writable = hasEditor && !active->isReadOnly();
    if (saveAction_) saveAction_->setEnabled(writable);
    if (saveAsAction_) saveAsAction_->setEnabled(hasEditor);
    if (formatFileAction_) formatFileAction_->setEnabled(writable);
    if (pickFontAction_) pickFontAction_->setEnabled(hasEditor);

    // Evaluation only makes sense for Turmeric documents. A `build.tur` is a
    // Turmeric file but not a script, so the same action turns into "build the
    // project that owns this manifest" — and running a *selection* of a
    // manifest means nothing, so that one stays off.
    const EvalMode mode = currentEvalMode();
    if (runBufferAction_) {
        runBufferAction_->setEnabled(mode != EvalMode::Disabled);
        if (mode == EvalMode::Project) {
            runBufferAction_->setText("&Build Project");
            runBufferAction_->setToolTip("Build the project this build.tur describes");
        } else {
            runBufferAction_->setText("&Run Buffer");
            runBufferAction_->setToolTip(
                mode == EvalMode::Buffer
                    ? QStringLiteral("Evaluate File")
                    : QStringLiteral("Evaluation is only available for Turmeric files"));
        }
    }
    if (traceAction_) {
        // Same gate as Run Buffer: a manifest is traceable in the sense that
        // it is Turmeric, but there is nothing to enter.
        traceAction_->setEnabled(mode == EvalMode::Buffer);
        traceAction_->setToolTip(
            mode == EvalMode::Buffer
                ? QStringLiteral("Record an execution trace with `tur trace`")
                : QStringLiteral("Tracing is only available for Turmeric files"));
    }
    if (runSelectionAction_) {
        runSelectionAction_->setEnabled(mode == EvalMode::Buffer);
        runSelectionAction_->setToolTip(
            mode == EvalMode::Buffer
                ? QStringLiteral("Evaluate Selection")
                : QStringLiteral("Evaluation is only available for Turmeric files"));
    }
    if (debugAction_) {
        // Same gate as Run Buffer: a manifest is not a script, and the
        // interpreter debugger runs a program, not a project description.
        debugAction_->setEnabled(mode == EvalMode::Buffer);
        debugAction_->setToolTip(
            mode == EvalMode::Buffer
                ? QStringLiteral("Run the current file under the interpreter debugger (`tur dap`)")
                : QStringLiteral("Debugging is only available for Turmeric files"));
    }
    if (replayAction_) {
        replayAction_->setEnabled(mode == EvalMode::Buffer);
        replayAction_->setToolTip(
            mode == EvalMode::Buffer
                ? QStringLiteral("Record the run, then step through it in both "
                                 "directions (`tur dap` with a recording)")
                : QStringLiteral("Time-travel debugging is only available for "
                                 "Turmeric files"));
    }
    if (toggleBreakpointAction_) {
        // Gated on the same thing as Debug Buffer — a breakpoint in a file the
        // debugger will never load is a decoration, not a breakpoint.
        toggleBreakpointAction_->setEnabled(mode == EvalMode::Buffer);
        toggleBreakpointAction_->setToolTip(
            mode == EvalMode::Buffer
                ? QStringLiteral("Set or clear a breakpoint on the caret's line (or click the gutter)")
                : QStringLiteral("Breakpoints are only available for Turmeric files"));
    }

    // Trowel highlights nine languages and has a language server for one. F12
    // that silently does nothing in a .py file is a bug report; greying it out
    // says which files it works on.
    const bool servedByLsp = hasEditor && mode != EvalMode::Disabled;
    if (gotoDefinitionAction_) {
        gotoDefinitionAction_->setEnabled(servedByLsp);
        gotoDefinitionAction_->setToolTip(
            servedByLsp
                ? QStringLiteral("Jump to where the symbol at the caret is defined")
                : QStringLiteral("Go to Definition is only available for Turmeric files"));
    }
    if (outlineAction_) {
        outlineAction_->setEnabled(servedByLsp);
        outlineAction_->setToolTip(
            servedByLsp
                ? QStringLiteral("List the definitions in this file")
                : QStringLiteral("Symbols is only available for Turmeric files"));
    }
    if (findReferencesAction_) {
        findReferencesAction_->setEnabled(servedByLsp);
        findReferencesAction_->setToolTip(
            servedByLsp
                ? QStringLiteral("List every use of the symbol at the caret")
                : QStringLiteral("Find References is only available for Turmeric files"));
    }
    if (renameAction_) {
        // Also off for a read-only stdlib buffer: the server refuses to rename
        // stdlib symbols anyway, and greying it out says so before the round
        // trip rather than after it.
        const bool renameable = servedByLsp && writable;
        renameAction_->setEnabled(renameable);
        renameAction_->setToolTip(
            renameable
                ? QStringLiteral("Rename the symbol at the caret across the workspace")
                : QStringLiteral("Rename is only available for writable Turmeric files"));
    }
}

void MainWindow::rememberRecentFile(const QString& path) {
    if (path.isEmpty()) return;
    // "Open Recent ▸ list.tur" landing in an unwritable file inside the app
    // bundle is a puzzle, not a feature.
    if (isStdlibPath(path)) return;
    const QString abs = QFileInfo(path).absoluteFilePath();
    recentFiles_.removeAll(abs);
    recentFiles_.prepend(abs);
    while (recentFiles_.size() > 8) recentFiles_.removeLast();
    rebuildRecentMenu();
}

void MainWindow::rebuildRecentMenu() {
    if (!recentMenu_) return;
    recentMenu_->clear();
    if (recentFiles_.isEmpty()) {
        auto* empty = recentMenu_->addAction("(no recent files)");
        empty->setEnabled(false);
        return;
    }
    for (const QString& path : recentFiles_) {
        auto* a = recentMenu_->addAction(QFileInfo(path).fileName());
        a->setToolTip(path);
        a->setData(path);
        connect(a, &QAction::triggered, this, &MainWindow::openRecentFromAction);
    }
    recentMenu_->addSeparator();
    auto* clear = recentMenu_->addAction("Clear Menu");
    connect(clear, &QAction::triggered, this, [this]{
        recentFiles_.clear();
        rebuildRecentMenu();
    });
}

void MainWindow::openRecentFromAction() {
    auto* a = qobject_cast<QAction*>(sender());
    if (!a) return;
    const QString path = a->data().toString();
    if (path.isEmpty()) return;
    if (!QFileInfo::exists(path)) {
        recentFiles_.removeAll(path);
        rebuildRecentMenu();
        statusBar()->showMessage(QString("File no longer exists: %1").arg(path), 4000);
        return;
    }
    openPath(path);
}

void MainWindow::loadRecentFiles() {
    QSettings settings;
    recentFiles_ = settings.value("recentFiles").toStringList();
    while (recentFiles_.size() > 8) recentFiles_.removeLast();
    rebuildRecentMenu();
}

void MainWindow::pickFont() {
    EditorView* v = editorView();
    if (!v) return;
    bool ok = false;
    const QFont chosen = QFontDialog::getFont(&ok, v->currentFont(), this, "Editor Font");
    if (!ok) return;
    editorFont_ = chosen;
    for (auto& b : buffers_) {
        if (b->view && b->view->kind() == TabContent::Kind::Editor) {
            static_cast<EditorView*>(b->view)->setFont(chosen);
        }
    }
    QSettings().setValue("editorFont", chosen);
}

void MainWindow::setWindowManager(WindowManager* windows) {
    windows_ = windows;
    if (windows_) {
        connect(windows_, &WindowManager::windowsChanged,
                this, &MainWindow::rebuildWindowMenu);
    }
    rebuildWindowMenu();
}

void MainWindow::rebuildWindowMenu() {
    if (!windowMenu_) return;
    windowMenu_->clear();
    if (!windows_) return;

    for (MainWindow* w : windows_->windows()) {
        QAction* action = windowMenu_->addAction(w->windowTitle());
        action->setCheckable(true);
        action->setChecked(w == this);
        // `w` as the context object: if that window goes away, so does the
        // connection, and the menu is rebuilt anyway.
        connect(action, &QAction::triggered, w, [w]() {
            w->show();
            w->raise();
            w->activateWindow();
        });
    }
}

void MainWindow::newWindow() {
    if (!windows_) return;
    windows_->newWindow();
}

void MainWindow::quitApp() {
    // Save the whole window set *before* closing anything: quitting should
    // restore every window that was open, whereas closing a window one at a
    // time deliberately drops it from the session.
    if (windows_) {
        windows_->persistAll();
        windows_->setQuitting(true);
    }

    // Quit means the whole app, not just this window. closeAllWindows() runs
    // every window's closeEvent, so an unsaved-changes prompt can still cancel
    // — windows that already closed stay closed, and any that refused keep the
    // app alive.
    QApplication::closeAllWindows();

    if (!windows_ || windows_->count() == 0) {
        // On macOS quitOnLastWindowClosed is disabled (the app is meant to
        // outlive its windows), so closing them all is not enough to end the
        // process — ask explicitly.
        QApplication::quit();
        return;
    }

    // Somebody refused to close, so the quit is off. Resync the saved session
    // to what is actually still open.
    windows_->setQuitting(false);
    windows_->persistAll();
}

void MainWindow::newFile() {
    Buffer* b = addBuffer(QString(), /*untitledIfEmpty=*/true);
    if (!b) return;
    activateBuffer(static_cast<int>(buffers_.size()) - 1);
    refreshTabBar();
}

void MainWindow::openFile() {
    QSettings settings;
    const QString last = settings.value("lastOpenDir", QDir::homePath()).toString();
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, "Open", last,
        "All files (*);;Turmeric (*.tur *.tur.sweet);;Scheme (*.scm);;"
        "Markdown (*.md *.markdown);;"
        "JSON (*.json);;C (*.c *.h);;Justfile (Justfile justfile *.just)");
    if (paths.isEmpty()) return;
    for (const QString& path : paths) {
        if (QFileInfo(path).isDir()) {
            openDirectory(path);
        } else {
            openPath(path);
        }
    }
    settings.setValue("lastOpenDir", QFileInfo(paths.last()).absolutePath());
}

void MainWindow::openDirectoryDialog() {
    QSettings settings;
    const QString last = settings.value("lastOpenDir", QDir::homePath()).toString();
    const QString path = QFileDialog::getExistingDirectory(this, "Open Directory", last);
    if (path.isEmpty()) return;
    settings.setValue("lastOpenDir", path);
    openDirectory(path);
}

bool MainWindow::saveBuffer(int index) {
    if (index < 0 || index >= static_cast<int>(buffers_.size())) return false;
    TabContent* tc = buffers_[index]->view;
    if (!tc || tc->kind() != TabContent::Kind::Editor) return false;
    auto* v = static_cast<EditorView*>(tc);
    if (v->filePath().isEmpty()) return saveBufferAs(index);
    if (!v->saveCurrent()) {
        QMessageBox::warning(this, "Trowel", "Could not save file.");
        return false;
    }
    return true;
}

bool MainWindow::saveBufferAs(int index) {
    if (index < 0 || index >= static_cast<int>(buffers_.size())) return false;
    TabContent* tc = buffers_[index]->view;
    if (!tc || tc->kind() != TabContent::Kind::Editor) return false;
    auto* v = static_cast<EditorView*>(tc);
    QSettings settings;
    const QString last = settings.value("lastOpenDir", QDir::homePath()).toString();
    const QString path = QFileDialog::getSaveFileName(
        this, "Save As", last,
        "Turmeric (*.tur *.tur.sweet);;Scheme (*.scm);;"
        "Markdown (*.md *.markdown);;JSON (*.json);;"
        "C (*.c *.h);;Justfile (Justfile justfile *.just);;All files (*)");
    if (path.isEmpty()) return false;
    if (!v->saveFile(path)) {
        QMessageBox::warning(this, "Trowel", QString("Could not save %1").arg(path));
        return false;
    }
    settings.setValue("lastOpenDir", QFileInfo(path).absolutePath());
    rememberRecentFile(path);
    buffers_[index]->untitledIndex = 0;
    updateBufferDisplayName(index);
    return true;
}

bool MainWindow::save() { return saveBuffer(activeIndex_); }
bool MainWindow::saveAs() { return saveBufferAs(activeIndex_); }

void MainWindow::clearRepl() {
    if (terminal_) terminal_->clearScreen();
    // If the REPL is idle at its prompt, ask it to redraw so the user sees the
    // prompt after the wipe. When busy, leave the screen blank — a running
    // command's output will fill it back in.
    if (repl_) repl_->redrawPrompt();
}

// The dialect a fresh REPL should start in: the active buffer's.
//
// The REPL pane serves the editor, so restarting it while a Scheme file is open
// should give a Scheme session -- otherwise the first Run Buffer immediately
// switches and resets the session the user just started, which is a worse
// first impression than the REPL simply coming up in the right language. The
// banner names the dialect, so this is never silent.
Dialect MainWindow::replDialectForActiveBuffer() const {
    const EditorView* v = editorView();
    return v ? v->dialect() : Dialect::Turmeric;
}

// Rebuild the Dialect submenu for the active buffer.
//
// Rebuilt rather than merely re-checked, because which ROWS exist depends on
// the buffer: a file that names `turmeric/neoteric` gets a row for it, and no
// other file does.
void MainWindow::rebuildDialectMenu() {
    if (!dialectMenu_ || !dialectGroup_) return;

    for (QAction* a : dialectGroup_->actions()) {
        dialectGroup_->removeAction(a);
        a->deleteLater();
    }
    dialectMenu_->clear();

    EditorView* v = editorView();
    const Dialect current = v ? v->dialect() : Dialect::Turmeric;

    // The readers worth offering, per language. `scheme` is Scheme's bare
    // reader; its name in the menu is the language's own word for "no sweet".
    const QVector<Dialect> offered{
        Dialect::Turmeric, Dialect::TurmericSweet,
        Dialect::Saffron,  Dialect::SaffronSweet,
        Dialect::R7rs,     Dialect::R7rsSweet,
    };

    DialectLanguage heading = DialectLanguage::Turmeric;
    bool first = true;
    for (const Dialect d : offered) {
        if (first || LanguageOf(d) != heading) {
            heading = LanguageOf(d);
            first = false;
            // A disabled action as a heading: QMenu has no section widget that
            // themes consistently across platforms, and a separator alone
            // would not say which language follows.
            QAction* label = dialectMenu_->addAction(
                QString("— %1 —").arg(DialectLanguageName(d)));
            label->setEnabled(false);
        }
        QAction* a = dialectMenu_->addAction(
            QString("%1    (#lang %2)")
                .arg(DialectReaderName(d), DialectBaseToken(d)));
        a->setCheckable(true);
        a->setData(static_cast<int>(d));
        a->setChecked(d == current);
        dialectGroup_->addAction(a);
    }

    // The buffer names a reader the list does not offer -- give it its row, so
    // the menu never disagrees with the source.
    const bool shown = offered.contains(current);
    if (!shown) {
        dialectMenu_->addSeparator();
        QAction* a = dialectMenu_->addAction(
            QString("%1    (#lang %2)")
                .arg(DialectReaderName(current), DialectBaseToken(current)));
        a->setCheckable(true);
        a->setChecked(true);
        a->setData(static_cast<int>(current));
        dialectGroup_->addAction(a);
    }

    dialectMenu_->setEnabled(v != nullptr);
}

void MainWindow::restartRepl() {
    repl_->restartIn(replWorkingDir(), replDialectForActiveBuffer());
}

void MainWindow::restartReplInDirectory() {
    if (!repl_) return;
    // Offer where the REPL is now, so the dialog opens somewhere meaningful
    // rather than at the filesystem root.
    QString start = repl_->workingDir();
    if (start.isEmpty()) start = replWorkingDir();

    const QString dir = QFileDialog::getExistingDirectory(
        this, "Restart REPL In", start);
    if (dir.isEmpty()) return;  // cancelled

    // A running process cannot be moved, so "set the directory" is necessarily
    // a restart — which is why the menu entry says so.
    repl_->restartIn(dir, replDialectForActiveBuffer());
}

void MainWindow::runBuffer() {
    EditorView* v = editorView();
    if (!v) return;
    // The action is greyed out in this case, but the control socket and any
    // stale shortcut route here too — so the gate lives here, not only in the
    // enabled state.
    const EvalMode mode = EvalModeForPath(v->filePath());
    if (mode == EvalMode::Disabled) {
        statusBar()->show();
        statusBar()->showMessage(
            "Evaluation is only available for Turmeric files.", 4000);
        return;
    }
    if (mode == EvalMode::Project) {
        runProject();
        return;
    }
    const RunResult r = RunBuffer(v, repl_);
    if (!r.ok) statusBar()->showMessage(r.message, 4000);
}

void MainWindow::runProject() {
    EditorView* v = editorView();
    if (!v) return;
    const QString dir = ProjectDirForPath(v->filePath());
    if (dir.isEmpty()) return;
    // The manifest on disk is what `tur build` reads, so an unsaved edit would
    // silently build the previous version.
    if (v->isModified() && !save()) return;
    if (!projectRunner_) projectRunner_ = new ProjectRunner(terminal_, this);
    const RunResult r = projectRunner_->run(dir);
    statusBar()->show();
    statusBar()->showMessage(r.message, 4000);
}

void MainWindow::traceBuffer() {
    EditorView* v = editorView();
    if (!v) return;
    if (EvalModeForPath(v->filePath()) == EvalMode::Disabled) {
        statusBar()->show();
        statusBar()->showMessage("Tracing is only available for Turmeric files.", 4000);
        emit traceFinished(TraceOutcome::Failed, TraceSummary{},
                           QStringLiteral("Tracing is only available for Turmeric files."));
        return;
    }
    // `tur trace` reads the file from disk, so an unsaved edit would trace the
    // previous version — the same trap runProject() avoids.
    if (v->filePath().isEmpty() || (v->isModified() && !save())) {
        statusBar()->show();
        statusBar()->showMessage("Save the file before tracing it.", 4000);
        emit traceFinished(TraceOutcome::Failed, TraceSummary{},
                           QStringLiteral("Save the file before tracing it."));
        return;
    }
    if (!traceRunner_) {
        traceRunner_ = new TraceRunner(terminal_, this);
        connect(traceRunner_, &TraceRunner::finished, this,
                [this](TraceOutcome outcome, const TraceSummary& summary,
                       const QString& explanation) {
            statusBar()->show();
            // The explanation, not the step count: "2 steps" on its own is the
            // reading that makes a user conclude the tracer is broken.
            statusBar()->showMessage(explanation, 10000);
            emit traceFinished(outcome, summary, explanation);
        });
    }
    const RunResult r = traceRunner_->run(v->filePath());
    statusBar()->show();
    statusBar()->showMessage(r.message, 4000);
    if (!r.ok) emit traceFinished(TraceOutcome::Failed, TraceSummary{}, r.message);
}

void MainWindow::runSelection() {
    EditorView* v = editorView();
    if (!v) return;
    if (EvalModeForPath(v->filePath()) != EvalMode::Buffer) {
        statusBar()->show();
        statusBar()->showMessage(
            "Evaluation is only available for Turmeric files.", 4000);
        return;
    }
    const auto [start, end] = v->selectionRange();
    const RunResult r = RunRange(v, repl_, start, end);
    if (!r.ok) statusBar()->showMessage(r.message, 4000);
}

void MainWindow::toggleBreakpointAtCaret() {
    EditorView* v = editorView();
    if (!v || !breakpoints_) return;
    const QString path = v->filePath();
    // A breakpoint is keyed by path, and an untitled buffer has none. Say so
    // rather than silently dropping the toggle.
    if (path.isEmpty()) {
        statusBar()->show();
        statusBar()->showMessage("Save the file before setting a breakpoint.", 4000);
        return;
    }
    const auto [line0, col] = v->lineColFromPos(v->cursorPos());
    Q_UNUSED(col);
    const int line = line0 + 1;  // the model and DAP are both 1-based

    // Read the before-state rather than the after: `toggle` reports whether the
    // set changed, not which way it went, and the message has to name the
    // direction.
    const QVector<BreakpointModel::Breakpoint> before = breakpoints_->forFile(path);
    const bool wasSet = std::any_of(
        before.cbegin(), before.cend(),
        [line](const BreakpointModel::Breakpoint& b) { return b.line == line; });
    breakpoints_->toggle(path, line);

    // A live session only reads stdin while paused (constraint 6), so a toggle
    // made mid-run cannot be delivered until the next stop — but one made while
    // already paused can, and should be, or stepping past the new line would
    // sail right through it.
    if (debug_ && debug_->state() == DebugSession::State::Paused) {
        pushBreakpointsToSession();
    }

    statusBar()->show();
    statusBar()->showMessage(
        QStringLiteral("%1 breakpoint at %2:%3")
            .arg(wasSet ? QStringLiteral("Cleared") : QStringLiteral("Set"),
                 QFileInfo(path).fileName())
            .arg(line),
        3000);
}

void MainWindow::debugOrContinue() {
    // F5 means Continue while a session is paused and Start otherwise — the
    // convention every debugger uses, and the reason the Debugger tab's
    // Continue button carries no shortcut of its own.
    if (debug_ && debug_->state() == DebugSession::State::Paused) {
        debug_->resume();
        return;
    }
    startDebugSession(/*replay=*/false);
}

void MainWindow::debugBuffer() { startDebugSession(/*replay=*/false); }

void MainWindow::replayBuffer() {
    // Always stop on entry. A replay launch without it seeks straight to the
    // first breakpoint and, finding none, ends the session — the adapter
    // reports `exited` for a recording it never showed anyone, which reads as
    // a broken feature rather than as "you set no breakpoints". Landing at
    // step 0 is also what the action is for: scrubbing from the start.
    debugStopOnEntry_ = true;
    startDebugSession(/*replay=*/true);
}

void MainWindow::restartDebug() {
    // `tur dap` runs one program per session (constraint 3) and has no
    // `restart` request, so this is a respawn, not a rewind. The buffer and
    // the mode are taken from the session being replaced so a restart means
    // "the same thing again" — including replay, where re-recording is the
    // only way to get back to step 0.
    if (!debug_) {
        statusBar()->show();
        statusBar()->showMessage("No debug session to restart.", 4000);
        return;
    }
    // Read before the session is torn down inside startDebugSession.
    const bool wasReplay = debug_->isReplay();
    const bool wasStopOnEntry = debug_->stopsOnEntry();
    const QString program = debug_->program();
    if (!program.isEmpty()) {
        const int idx = indexOfPath(program);
        if (idx >= 0 && idx != activeIndex_) activateBuffer(idx);
    }
    if (wasReplay) {
        replayBuffer();
        return;
    }
    // `debugStopOnEntry_` is a one-shot the launch consumes, so it has to be
    // set again here — otherwise a restart of a paused session silently runs
    // to completion, which is the opposite of what was asked for.
    debugStopOnEntry_ = wasStopOnEntry;
    startDebugSession(/*replay=*/false);
}

void MainWindow::startDebugSession(bool replay) {
    EditorView* v = editorView();
    if (!v) return;
    // The action is greyed out otherwise, but the control socket and any stale
    // shortcut route here too — so the gate lives here as well.
    if (EvalModeForPath(v->filePath()) != EvalMode::Buffer) {
        statusBar()->show();
        statusBar()->showMessage(
            "Debugging is only available for Turmeric files.", 4000);
        return;
    }
    // Breakpoints bind by basename (constraint 5) and stack frames carry the
    // path, so a scratch file with a mangled name would silently fail to bind
    // breakpoints. Require a saved file, exactly like traceBuffer does.
    if (v->filePath().isEmpty() || (v->isModified() && !save())) {
        statusBar()->show();
        statusBar()->showMessage("Save the file before debugging it.", 4000);
        return;
    }

    // One session per window. A live session is stopped first — "restart"
    // means respawning `tur dap` (one program per session, constraint 3).
    if (debug_) {
        debug_->stop();
        debug_->deleteLater();
        debug_ = nullptr;
    }

    timelineSitesLoaded_ = false;
    debug_ = new DebugSession(this);
    connect(debug_, &DebugSession::outputReceived, this, [this](const QString& text) {
        // Stream debuggee output into the Debugger tab's console.
        if (replPane_ && replPane_->debugger()) replPane_->debugger()->appendOutput(text);
    });
    // Toolbar → session. The toolbar lives in the DebuggerView; the session
    // is owned by the window, so the connection crosses that boundary here.
    if (replPane_ && replPane_->debugger()) {
        auto* dv = replPane_->debugger();
        connect(dv, &DebuggerView::continueRequested, debug_, &DebugSession::resume);
        connect(dv, &DebuggerView::stepOverRequested, debug_, &DebugSession::stepOver);
        connect(dv, &DebuggerView::stepInRequested, debug_, &DebugSession::stepIn);
        connect(dv, &DebuggerView::stepOutRequested, debug_, &DebugSession::stepOut);
        connect(dv, &DebuggerView::stopRequested, debug_, &DebugSession::stop);
        connect(dv, &DebuggerView::stepBackRequested, debug_, &DebugSession::stepBack);
        connect(dv, &DebuggerView::reverseStepOverRequested,
                debug_, &DebugSession::reverseStepOver);
        connect(dv, &DebuggerView::reverseContinueRequested,
                debug_, &DebugSession::reverseContinue);
        // The two capabilities are exclusive and both are the adapter's rules,
        // not ours: reverse execution needs a recording, and `evaluate` needs a
        // live frame. Saying so once, up front, beats an error per keystroke.
        dv->setReverseAvailable(replay);
        // The scrubber appears only once the adapter has confirmed it serves
        // the timeline — which it does on `initialize`, after this runs. Hidden
        // until then.
        dv->setTimelineVisible(false);
        connect(dv->timeline(), &TimelineStrip::seekRequested,
                debug_, &DebugSession::seek);
        dv->setEvaluateEnabled(
            !replay,
            QStringLiteral("Evaluate is unavailable in a recording — "
                           "run Debug Buffer for a live session"));
        // Picking a frame moves the *selected-frame* highlight, which is a
        // different marker from the current-execution line — "looking at"
        // versus "stopped at". Frame 0 is both, so it gets the execution one.
        connect(dv, &DebuggerView::frameSelected, this, [this](int frameId) {
            if (!debug_) return;
            debug_->selectFrame(frameId);
            showSelectedFrame(frameId);
        });
        connect(dv, &DebuggerView::evaluateRequested, this, [this](const QString& expr) {
            if (!debug_) return;
            auto* view = replPane_ ? replPane_->debugger() : nullptr;
            debug_->evaluate(expr, [view, expr](bool ok, const QString& text) {
                if (view) view->appendEvaluation(expr, text, ok);
            });
        });
    }
    connect(debug_, &DebugSession::framesUpdated, this, [this] {
        if (!debug_ || !replPane_ || !replPane_->debugger()) return;
        replPane_->debugger()->setFrames(debug_->frames(), debug_->selectedFrameId());
    });
    connect(debug_, &DebugSession::variablesUpdated, this, [this] {
        if (!debug_ || !replPane_ || !replPane_->debugger()) return;
        replPane_->debugger()->setVariables(debug_->variables());
    });
    connect(debug_, &DebugSession::timelineUpdated, this, [this] {
        if (!debug_ || !replPane_ || !replPane_->debugger()) return;
        auto* dv = replPane_->debugger();
        const bool live = debug_->isReplay() && debug_->hasTimeline();
        dv->setTimelineVisible(live);
        if (!live) return;
        dv->timeline()->setTimeline(debug_->timeline());
        // The ribbon is the whole recording's shape, so it only has to be
        // fetched once per session — not once per seek.
        if (!timelineSitesLoaded_) {
            timelineSitesLoaded_ = true;
            auto* view = dv;
            debug_->requestSites(96, [view](const QVector<DebugSession::Site>& s) {
                view->timeline()->setSites(s);
            });
        }
    });
    // The site readout is driven from `framesUpdated`, NOT from
    // `timelineUpdated`. `refreshTimeline` is issued before `refreshFrames` in
    // `onStopped`, so reading `frames()` there gets the *previous* stop's top
    // frame — measured: the strip read `main tl.tur:4` while the stack showed
    // `work:2`. Same class of mistake as the one that made `stopped` fire
    // early, and the same fix: read the data where it is known to be current.
    connect(debug_, &DebugSession::framesUpdated, this, [this] {
        if (!debug_ || !replPane_ || !replPane_->debugger()) return;
        auto* dv = replPane_->debugger();
        if (!debug_->isReplay() || !debug_->hasTimeline()) return;
        const auto& frames = debug_->frames();
        if (frames.isEmpty()) { dv->timeline()->setSite({}, {}, 0); return; }
        const auto& f = frames.first();
        dv->timeline()->setSite(f.name, QFileInfo(f.filePath).fileName(), f.line);
    });
    // A backwards seek shortens the transcript; the console swaps rather than
    // grows. This is T5 — the console rewinds with the cursor.
    connect(debug_, &DebugSession::outputReplaced, this, [this](const QString& text) {
        if (replPane_ && replPane_->debugger()) replPane_->debugger()->setOutput(text);
    });
    connect(debug_, &DebugSession::pushBreakpointsRequested, this,
            &MainWindow::pushBreakpointsToSession);
    connect(debug_, &DebugSession::stateChanged, this, [this](DebugSession::State s) {
        if (!replPane_ || !replPane_->debugger()) return;
        // Paused: everything except Stop enabled. Running: only Stop.
        // Idle/Terminated: everything off.
        if (s == DebugSession::State::Paused) replPane_->debugger()->setPaused(true);
        else if (s == DebugSession::State::Running) replPane_->debugger()->setRunning(true);
        else replPane_->debugger()->setRunning(false);
    });
    connect(debug_, &DebugSession::stopped, this, [this](const QString&) {
        // A new stop always lands on the innermost frame, so the execution
        // line and the selection are the same thing here. `frames()` is
        // populated by the time this fires — `stopped` is deferred until
        // stackTrace and variables have both answered.
        if (!debug_) return;
        const auto& frames = debug_->frames();
        if (frames.isEmpty()) return;
        showSelectedFrame(frames.first().id);
    });
    connect(debug_, &DebugSession::resumed, this, [this]() {
        clearExecutionLines();
    });
    connect(debug_, &DebugSession::programExited, this, [this](int code) {
        clearExecutionLines();
        statusBar()->show();
        if (code < 0) {
            statusBar()->showMessage("Debug session stopped.", 4000);
        } else {
            statusBar()->showMessage(
                QStringLiteral("Debug session exited (%1).").arg(code), 4000);
        }
    });
    connect(debug_, &DebugSession::sessionFailed, this, [this](const QString& msg) {
        statusBar()->show();
        statusBar()->showMessage(msg.isEmpty()
            ? QStringLiteral("Debug session failed.") : msg, 6000);
    });

    // Auto-switch to the Debugger tab when a session starts. Do not switch
    // away when it ends — yanking the pane out from under someone reading
    // output is worse than a stale tab.
    if (replPane_) replPane_->showDebugger();

    // Pin TUR_STDLIB_DIR to the sibling of the resolved binary, exactly as
    // ReplSession::start does — the debuggee is a fresh process with no
    // inherited REPL environment.
    QStringList extraEnv;
    const QString tur = ResolveTurBinary();
    if (!tur.isEmpty()) {
        const QString siblingStdlib = TurStdlibDirFor(tur);
        if (!siblingStdlib.isEmpty()) {
            extraEnv << QStringLiteral("TUR_STDLIB_DIR=") + siblingStdlib;
        }
    }

    // A warning used to stand here: that a file without `(defn main [] …)`
    // would run straight through -- no entry stop, no breakpoint hits -- because
    // `tur dap` instrumented only what `(main)` evaluated, and that a breakpoint
    // in such a file would sit verified in the gutter and never bind.
    //
    // That stopped being true in Turmeric v0.44.0: "`tur dap` and `tur trace`
    // now instrument top-level programs, not only `(main)` ... the launch path
    // now pre-scans for a top-level `main` and arms the debugger around the file
    // load itself when there isn't one."  Trowel bundles v0.46.0, so breakpoints
    // in a top-level file bind and the debugger stops.
    //
    // Removed rather than reworded: it told the user to restructure their
    // program to work around a limitation that no longer exists, which is worse
    // than saying nothing.  Measured against the bundled binary -- `tur trace`
    // on a top-level file now records 3 steps where it recorded 0.

    debug_->start(v->filePath(), replWorkingDir(), extraEnv, debugStopOnEntry_, replay);
    debugStopOnEntry_ = false;  // one-shot: the menu action always runs to completion
    statusBar()->show();
    // Two different waits, said differently. A replay launch runs the whole
    // program with the recorder attached before it answers anything — roughly
    // 4x an untraced interpreter run — and a progress message that said
    // "Debugging…" would look hung.
    statusBar()->showMessage(
        replay ? QStringLiteral("Recording %1 for time-travel…")
                     .arg(QFileInfo(v->filePath()).fileName())
               : QStringLiteral("Debugging %1…")
                     .arg(QFileInfo(v->filePath()).fileName()),
        replay ? 6000 : 2000);
}

void MainWindow::formatFile() {
    EditorView* v = editorView();
    if (!v) return;
    const QString binary = ResolveTurBinary();
    if (binary.isEmpty()) {
        statusBar()->showMessage("Could not locate `tur` executable.", 4000);
        return;
    }

    const Dialect dialect = v->dialect();

    // `tur fmt --stdin`, not `tur format`. Without a dialect a Scheme buffer
    // formats to different bytes: `(define (f x)\n(* x 2))` comes back collapsed
    // onto one line with a blank inserted, where `--lang r7rs` re-indents it.
    //
    // Every dialect goes through, the three sweet readers included: v0.61.0
    // parse-checks a sweet buffer and keeps it as written, where v0.60.1
    // reprinted the two Turmeric ones as s-expressions and had to be declined.
    const QStringList args{"fmt", "--stdin", "--lang", DialectFmtLangFlag(dialect)};
    QProcess proc;
    proc.start(binary, args);
    if (!proc.waitForStarted(3000)) {
        statusBar()->showMessage("Failed to start `tur fmt`.", 4000);
        return;
    }
    proc.write(v->text());
    proc.closeWriteChannel();
    if (!proc.waitForFinished(10000)) {
        proc.kill();
        statusBar()->showMessage("`tur fmt` timed out.", 4000);
        return;
    }
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        const QString err = QString::fromUtf8(proc.readAllStandardError()).trimmed();
        statusBar()->showMessage(err.isEmpty() ? QString("`tur fmt` failed.")
                                               : QString("Format error: %1").arg(err),
                                 6000);
        return;
    }
    const QByteArray formatted = proc.readAllStandardOutput();
    if (formatted == v->text()) {
        statusBar()->showMessage("Already formatted.", 2000);
        return;
    }
    const int caret = v->cursorPos();
    const int anchor = v->anchorPos();
    v->setText(formatted);
    const int len = formatted.size();
    v->setSelection(qMin(anchor, len), qMin(caret, len));
    statusBar()->showMessage("Formatted.", 2000);
}

void MainWindow::requestCompletion() {
    EditorView* v = editorView();
    if (!v) return;
    if (v->filePath().isEmpty()) {
        statusBar()->show();
        statusBar()->showMessage(LspManager::kSkipUnsavedReason, 4000);
        return;
    }
    emit v->completionRequested(v->cursorPos());
}

void MainWindow::showDocumentation() {
    EditorView* v = editorView();
    if (!v) return;
    if (v->filePath().isEmpty()) {
        statusBar()->show();
        statusBar()->showMessage(LspManager::kSkipUnsavedReason, 4000);
        return;
    }
    emit v->hoverRequested(v->cursorPos());
}

// Jump to the code a dependency error actually came from.
//
// Its own action rather than a case inside Go to Definition: F12 meaning
// "jump to the definition of this symbol" and sometimes "jump to an error in
// another file" would be a surprise, and the two have different preconditions
// (a symbol under the caret vs. a diagnostic under it).
void MainWindow::goToDiagnosticSource() {
    EditorView* v = editorView();
    if (!v) return;

    const LspDiagnostic* d = v->diagnosticAt(v->cursorPos());
    if (!d || d->related.isEmpty()) {
        statusBar()->show();
        statusBar()->showMessage(
            d ? QStringLiteral("This diagnostic is reported where it happened")
              : QStringLiteral("No diagnostic at the caret"),
            4000);
        return;
    }

    // The first related location. The server sends one for a `load` error; if a
    // future diagnostic carries several, the first is the innermost site, which
    // is the one worth landing on.
    jumpToSpan(d->related.first());
    if (!d->relatedMessages.isEmpty()) {
        statusBar()->show();
        statusBar()->showMessage(d->relatedMessages.first(), 6000);
    }
}

void MainWindow::goToDefinition() {
    EditorView* v = editorView();
    if (!v) { emit definitionJumpFinished(false); return; }
    if (v->filePath().isEmpty()) {
        statusBar()->show();
        statusBar()->showMessage(LspManager::kSkipUnsavedReason, 4000);
        emit definitionJumpFinished(false);
        return;
    }
    emit v->definitionRequested(v->cursorPos());
}

void MainWindow::showOutline() {
    EditorView* v = editorView();
    if (!v) { emit outlineReady({}, QStringLiteral("no editor")); return; }

    // An unsaved buffer has no URI, so the server has never seen it. Reported
    // the way completion and documentation already report it.
    if (v->filePath().isEmpty()) {
        statusBar()->show();
        statusBar()->showMessage(LspManager::kSkipUnsavedReason, 4000);
        v->showOutlineMessage(QString::fromUtf8(LspManager::kSkipUnsavedReason));
        emit outlineReady({}, QString::fromUtf8(LspManager::kSkipUnsavedReason));
        return;
    }

    LspManager* lsp = LspManager::instance();
    if (lsp->state() != LspManager::State::Ready) {
        // An empty outline would imply an empty file. A Trowel user may not
        // know a language server exists, so name it rather than showing
        // nothing.
        const QString reason = QStringLiteral("Language server is not ready");
        statusBar()->show();
        statusBar()->showMessage(reason, 4000);
        v->showOutlineMessage(reason);
        emit outlineReady({}, reason);
        return;
    }

    lsp->requestDocumentSymbols(v, [this, v](const QVector<LspSymbol>& symbols) {
        // The user may have switched tabs while the request was in flight;
        // popping a list over a different buffer would be worse than nothing.
        if (v != editorView()) return;

        if (!symbols.isEmpty()) {
            v->showSymbolList(symbols);
            emit outlineReady(symbols, QString());
            return;
        }

        // Empty has two very different causes and §4.2.1 measured that the
        // second is the common one: any analysis error empties documentSymbol
        // for the whole file. Blaming the file for defining nothing when it
        // actually failed to compile is the same lie the unavailable-server
        // case avoids.
        LspManager* mgr = LspManager::instance();
        const QString uri = LspManager::UriForPath(v->filePath());
        const bool brokenFile =
            mgr->hasPublishedFor(uri) && !mgr->diagnosticsFor(uri).isEmpty();
        const QString reason = brokenFile
                                   ? QStringLiteral("Not analyzed — fix errors first")
                                   : QStringLiteral("Nothing defined yet");
        v->showOutlineMessage(reason);
        emit outlineReady({}, reason);
    });
}

void MainWindow::findReferences() {
    EditorView* v = editorView();
    if (!v) { emit referencesReady({}, QStringLiteral("no editor")); return; }
    if (v->filePath().isEmpty()) {
        statusBar()->show();
        statusBar()->showMessage(LspManager::kSkipUnsavedReason, 4000);
        emit referencesReady({}, QString::fromUtf8(LspManager::kSkipUnsavedReason));
        return;
    }
    LspManager* lsp = LspManager::instance();
    if (lsp->state() != LspManager::State::Ready) {
        const QString reason = QStringLiteral("Language server is not ready");
        statusBar()->show();
        statusBar()->showMessage(reason, 4000);
        emit referencesReady({}, reason);
        return;
    }

    // includeDeclaration: the declaration is usually the thing being looked for.
    lsp->requestReferences(v, v->cursorPos(), /*includeDeclaration=*/true,
                           [this, v](const QVector<LspSpan>& spans) {
        if (v != editorView()) return;
        referenceSpans_ = spans;
        if (spans.isEmpty()) {
            const QString reason = QStringLiteral("No references found");
            v->showOutlineMessage(reason);
            statusBar()->show();
            statusBar()->showMessage(reason, 4000);
            emit referencesReady({}, reason);
            return;
        }

        QStringList rows;
        rows.reserve(spans.size());
        for (const LspSpan& span : spans) {
            const QString path = LspManager::PathForUri(span.uri);
            const QString name = path.isEmpty() ? span.uri : QFileInfo(path).fileName();
            QString text;
            // Prefer an open buffer: it may be dirty, and the reference then
            // describes what is on screen rather than what is on disk.
            if (const int idx = indexOfPath(QFileInfo(path).absoluteFilePath()); idx >= 0) {
                auto* other = qobject_cast<EditorView*>(buffers_[idx]->view);
                if (other) {
                    text = QString::fromUtf8(other->text())
                               .section('\n', span.range.startLine, span.range.startLine);
                }
            } else if (QFile file(path); file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                text = QString::fromUtf8(file.readAll())
                           .section('\n', span.range.startLine, span.range.startLine);
            }
            rows << QStringLiteral("%1:%2  %3")
                        .arg(name)
                        .arg(span.range.startLine + 1)
                        .arg(text.trimmed());
        }
        v->showChooserList(rows);
        // "References", never "All references": an oversized workspace returns
        // a shorter list rather than an error, so completeness cannot be
        // claimed (§3.1 of the plan).
        statusBar()->show();
        statusBar()->showMessage(QStringLiteral("References: %1").arg(spans.size()), 4000);
        emit referencesReady(spans, QString());
    });
}

// Signature help for the call the cursor is inside, on demand.
//
// It also fires by itself on `(` (EditorView's charAdded hook). This is the
// explicit entry: the tip is dismissed by any keystroke Scintilla thinks ends
// it, and after that the only way back was to delete the paren and retype it.
void MainWindow::showSignatureHelp() {
    EditorView* v = editorView();
    if (!v) return;
    if (v->filePath().isEmpty()) {
        statusBar()->show();
        statusBar()->showMessage(LspManager::kSkipUnsavedReason, 4000);
        return;
    }
    emit v->signatureHelpRequested(v->cursorPos());
}

// Workspace-wide symbol search: `workspace/symbol`, which the server has
// advertised all along and nothing asked for.
//
// Results go through the SAME chooser and the same `referenceSpans_` as Find
// References, so picking a row reuses jumpToSpan and the nav history with it.
// The query seeds from the word at the caret, which is what you want nine
// times out of ten and is still editable.
void MainWindow::findSymbolInProject() {
    EditorView* v = editorView();
    LspManager* lsp = LspManager::instance();
    if (lsp->state() != LspManager::State::Ready) {
        const QString reason = QStringLiteral("Language server is not ready");
        statusBar()->show();
        statusBar()->showMessage(reason, 4000);
        emit workspaceSymbolsReady({}, reason);
        return;
    }

    // Seed from the word at the caret, which is what you want nine times out of
    // ten and is still editable.
    QString seed;
    if (v && v->sciWidget()) {
        ScintillaEdit* sci = v->sciWidget();
        const int pos = v->cursorPos();
        const int from = static_cast<int>(sci->wordStartPosition(pos, true));
        const int to = static_cast<int>(sci->wordEndPosition(pos, true));
        if (to > from) seed = QString::fromUtf8(v->textInRange(from, to));
    }

    bool accepted = false;
    const QString query = QInputDialog::getText(
        this, QStringLiteral("Find Symbol in Project"),
        QStringLiteral("Symbol name:"), QLineEdit::Normal, seed, &accepted);
    if (!accepted) return;  // cancelled
    findSymbolInProjectFor(query);
}

// The half without the dialog, so the control API (and therefore the smoke
// suite) can drive the search without a modal in the way.
void MainWindow::findSymbolInProjectFor(const QString& query) {
    LspManager* lsp = LspManager::instance();
    if (lsp->state() != LspManager::State::Ready) {
        const QString reason = QStringLiteral("Language server is not ready");
        statusBar()->show();
        statusBar()->showMessage(reason, 4000);
        emit workspaceSymbolsReady({}, reason);
        return;
    }

    lsp->requestWorkspaceSymbols(query,
        [this](const QVector<LspSpan>& spans, const QStringList& names) {
        EditorView* cur = editorView();
        if (!cur) return;
        if (spans.isEmpty()) {
            const QString reason = QStringLiteral("No matching symbols");
            cur->showOutlineMessage(reason);
            statusBar()->show();
            statusBar()->showMessage(reason, 4000);
            emit workspaceSymbolsReady({}, reason);
            return;
        }

        referenceSpans_ = spans;
        QStringList rows;
        rows.reserve(spans.size());
        for (int i = 0; i < spans.size(); ++i) {
            const QString path = LspManager::PathForUri(spans.at(i).uri);
            const QString file = path.isEmpty() ? spans.at(i).uri
                                                : QFileInfo(path).fileName();
            rows << QStringLiteral("%1  —  %2:%3")
                        .arg(names.value(i))
                        .arg(file)
                        .arg(spans.at(i).range.startLine + 1);
        }
        cur->showChooserList(rows);
        // Not "all matches": an oversized workspace answers with a shorter
        // list rather than an error, the same caveat Find References carries.
        statusBar()->show();
        statusBar()->showMessage(QStringLiteral("Symbols: %1").arg(spans.size()), 4000);
        emit workspaceSymbolsReady(spans, QString());
    });
}

void MainWindow::renameSymbol() {
    EditorView* v = editorView();
    if (!v) { emit renameFinished(0, QStringLiteral("no editor")); return; }
    if (v->filePath().isEmpty()) {
        statusBar()->show();
        statusBar()->showMessage(LspManager::kSkipUnsavedReason, 4000);
        emit renameFinished(0, QString::fromUtf8(LspManager::kSkipUnsavedReason));
        return;
    }
    LspManager* lsp = LspManager::instance();
    if (lsp->state() != LspManager::State::Ready) {
        const QString reason = QStringLiteral("Language server is not ready");
        statusBar()->show();
        statusBar()->showMessage(reason, 4000);
        emit renameFinished(0, reason);
        return;
    }

    statusBar()->show();
    statusBar()->showMessage(QStringLiteral("Checking whether this can be renamed…"), 2000);

    // prepareRename first, always, and before any input is shown. The server's
    // refusals are written to be read; putting one in front of the user before
    // they type a new name is the whole reason prepareProvider is advertised.
    lsp->requestPrepareRename(v, v->cursorPos(),
                              [this, v](const LspManager::PrepareRename& prep) {
        if (v != editorView()) return;
        if (!prep.refusal.isEmpty()) {
            statusBar()->showMessage(prep.refusal, 8000);
            emit renameFinished(0, prep.refusal);
            return;
        }
        if (!prep.renameable) {
            const QString reason = QStringLiteral("No symbol to rename here");
            statusBar()->showMessage(reason, 4000);
            emit renameFinished(0, reason);
            return;
        }
        statusBar()->clearMessage();
        v->showRenameInput(prep.range, prep.placeholder);
        emit renameInputOpened();
    });
}

void MainWindow::applyRename(EditorView* view, const QString& newName) {
    statusBar()->show();
    // Rename compiles every importing file, so it is the one request that
    // routinely takes seconds — and it blocks diagnostics, completion and hover
    // behind it. Say so rather than looking hung.
    statusBar()->showMessage(QStringLiteral("Renaming to “%1”…").arg(newName));

    LspManager::instance()->requestRename(
        view, view->cursorPos(), newName,
        [this](const LspManager::WorkspaceEdit& edit, const QString& error) {
            if (!error.isEmpty()) {
                statusBar()->showMessage(error, 8000);
                emit renameFinished(0, error);
                return;
            }
            if (edit.isEmpty()) {
                const QString reason = QStringLiteral("Nothing to rename");
                statusBar()->showMessage(reason, 4000);
                emit renameFinished(0, reason);
                return;
            }

            // Blast-radius cap. Confirmation lives here rather than inside
            // applyWorkspaceEdit so the control API can drive the mechanism
            // without a modal dialog blocking the socket's event loop.
            if (edit.size() > kRenameConfirmThreshold) {
                QStringList names;
                for (auto it = edit.constBegin(); it != edit.constEnd(); ++it) {
                    names << QFileInfo(LspManager::PathForUri(it.key())).fileName();
                }
                names.sort();
                const auto choice = QMessageBox::question(
                    this, "Trowel",
                    QString("Rename across %1 files?\n\n%2")
                        .arg(edit.size())
                        .arg(names.join(QStringLiteral(", "))),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                if (choice != QMessageBox::Yes) {
                    const QString reason = QStringLiteral("rename cancelled");
                    statusBar()->showMessage(reason, 4000);
                    emit renameFinished(0, reason);
                    return;
                }
            }

            QString applyError;
            const int changed = applyWorkspaceEdit(edit, &applyError);
            if (changed < 0) {
                statusBar()->showMessage(applyError, 8000);
                emit renameFinished(0, applyError);
                return;
            }
            const QString ok = changed == 1
                                   ? QStringLiteral("Renamed in 1 file.")
                                   : QStringLiteral("Renamed in %1 files.").arg(changed);
            statusBar()->showMessage(ok, 4000);
            emit renameFinished(changed, QString());
        });
}

int MainWindow::applyWorkspaceEdit(const LspWorkspaceEdit& edit, QString* error) {
    // Resolve everything first. A half-applied cross-file rename is worse than
    // a refused one, and the only way to guarantee all-or-nothing is to prove
    // every document is reachable before touching any of them.
    struct Target {
        QString path;
        QVector<LspTextEdit> edits;
    };
    QVector<Target> targets;
    for (auto it = edit.constBegin(); it != edit.constEnd(); ++it) {
        const QString path = LspManager::PathForUri(it.key());
        if (path.isEmpty() || !QFileInfo::exists(path)) {
            if (error) {
                *error = QString("Rename touches a file Trowel cannot open (%1); "
                                 "nothing was changed.").arg(it.key());
            }
            return -1;
        }
        if (isStdlibPath(path)) {
            if (error) {
                *error = QString("Rename would edit the bundled stdlib (%1); "
                                 "nothing was changed.")
                             .arg(QFileInfo(path).fileName());
            }
            return -1;
        }
        targets.append(Target{QFileInfo(path).absoluteFilePath(), it.value()});
    }

    // Return the user where they were: applying an edit opens tabs, and a
    // rename that leaves you in a file you never asked to see is disorienting.
    const int originalIndex = activeIndex_;

    int changed = 0;
    for (Target& target : targets) {
        int index = indexOfPath(target.path);
        if (index < 0) {
            // No tab: open one and leave it dirty. Writing to disk behind the
            // user's back is not undoable by Ctrl+Z, and Trowel has no VCS
            // integration to fall back on.
            if (!openPath(target.path)) {
                if (error) {
                    *error = QString("Could not open %1; some files may already "
                                     "have been changed.").arg(target.path);
                }
                return -1;
            }
            index = indexOfPath(target.path);
        }
        if (index < 0) continue;
        auto* view = qobject_cast<EditorView*>(buffers_[index]->view);
        if (!view) continue;

        // Descending by position, so an edit that changes length cannot
        // invalidate the ranges of the edits that follow it.
        std::sort(target.edits.begin(), target.edits.end(),
                  [](const LspTextEdit& a, const LspTextEdit& b) {
                      if (a.range.startLine != b.range.startLine) {
                          return a.range.startLine > b.range.startLine;
                      }
                      return a.range.startCharacter > b.range.startCharacter;
                  });

        // One undo action per document, so Ctrl+Z reverses the whole file's
        // share of the rename rather than one occurrence at a time.
        view->beginEditGroup();
        for (const LspTextEdit& e : target.edits) view->replaceRange(e.range, e.newText);
        view->endEditGroup();
        changed++;
    }

    if (originalIndex >= 0 && originalIndex < static_cast<int>(buffers_.size())) {
        activateBuffer(originalIndex);
    }
    return changed;
}

bool MainWindow::isStdlibPath(const QString& path) {
    // Delegated rather than re-derived: two independent derivations of the
    // stdlib directory drift the moment TROWEL_TURMERIC_VERSION moves.
    return LspManager::instance()->isStdlibPath(path);
}

MainWindow::NavEntry MainWindow::currentNavEntry() const {
    EditorView* v = editorView();
    if (!v) return {};
    return NavEntry{v->filePath(), v->cursorPos()};
}

bool MainWindow::goToNavEntry(const NavEntry& entry) {
    if (entry.path.isEmpty()) return false;
    if (!QFileInfo::exists(entry.path)) return false;
    const QString abs = QFileInfo(entry.path).absoluteFilePath();
    if (const int existing = indexOfPath(abs); existing >= 0) {
        activateBuffer(existing);
    } else if (!openPath(abs)) {
        return false;
    }
    EditorView* v = editorView();
    if (!v) return false;
    v->setCursorPos(entry.pos);
    v->sciWidget()->scrollCaret();
    return true;
}

void MainWindow::jumpToDefinition(const LspLocation& location) {
    statusBar()->show();
    if (!location.isValid()) {
        // §4.2.1: the server also answers null for every name in a file that
        // failed to analyze, so point at that rather than only at the symbol.
        statusBar()->showMessage("No definition found (is the file free of errors?)", 4000);
        emit definitionJumpFinished(false);
        return;
    }

    const QString target = LspManager::PathForUri(location.uri);
    if (target.isEmpty()) {
        statusBar()->showMessage("Definition is not in a local file", 4000);
        emit definitionJumpFinished(false);
        return;
    }
    if (!QFileInfo::exists(target)) {
        statusBar()->showMessage(QString("Definition file is missing: %1").arg(target), 5000);
        emit definitionJumpFinished(false);
        return;
    }

    // Push before moving, so Back returns to where the user actually was.
    const NavEntry origin = currentNavEntry();

    const QString abs = QFileInfo(target).absoluteFilePath();
    if (const int existing = indexOfPath(abs); existing >= 0) {
        // Cases 1 and 2 in one: the same document is just the tab that is
        // already active, so activating it is a no-op and no tab churns.
        activateBuffer(existing);
    } else if (!openPath(abs)) {
        statusBar()->showMessage(QString("Could not open %1").arg(abs), 5000);
        emit definitionJumpFinished(false);
        return;
    }

    EditorView* v = editorView();
    if (!v) { emit definitionJumpFinished(false); return; }
    v->setCursorPos(v->posFromLineCol(location.line, location.character));
    v->sciWidget()->scrollCaret();

    pushNavHistory(origin);
    statusBar()->clearMessage();
    // Tab set and caret are both final by here, so an awaiting caller sees a
    // settled window rather than one mid-jump.
    emit definitionJumpFinished(true);
}

void MainWindow::pushNavHistory(const NavEntry& origin) {
    if (origin.path.isEmpty()) return;
    navBack_.append(origin);
    while (navBack_.size() > kNavHistoryMax) navBack_.removeFirst();
    // A fresh jump invalidates whatever Forward was pointing at.
    navForward_.clear();
    updateNavActionsEnabled();
}

void MainWindow::jumpToSpan(const LspSpan& span) {
    const QString target = LspManager::PathForUri(span.uri);
    if (target.isEmpty() || !QFileInfo::exists(target)) {
        statusBar()->show();
        statusBar()->showMessage(QString("Cannot open %1").arg(span.uri), 5000);
        return;
    }
    const NavEntry origin = currentNavEntry();
    const QString abs = QFileInfo(target).absoluteFilePath();
    if (const int existing = indexOfPath(abs); existing >= 0) {
        activateBuffer(existing);
    } else if (!openPath(abs)) {
        return;
    }
    EditorView* v = editorView();
    if (!v) return;
    // Select the whole span rather than just placing the caret: for a reference
    // the extent is the point, and it survives the occurrence highlight that
    // paints over the same range.
    const int start = v->posFromLineCol(span.range.startLine, span.range.startCharacter);
    const int end = v->posFromLineCol(span.range.endLine, span.range.endCharacter);
    v->setSelection(start, end);
    v->sciWidget()->scrollCaret();
    pushNavHistory(origin);
}

void MainWindow::navigateBack() {
    // Skip entries whose file has since been deleted rather than erroring: the
    // stack is a convenience, and one dead entry should not block the rest.
    while (!navBack_.isEmpty()) {
        const NavEntry entry = navBack_.takeLast();
        const NavEntry here = currentNavEntry();
        if (goToNavEntry(entry)) {
            if (!here.path.isEmpty()) navForward_.append(here);
            break;
        }
    }
    updateNavActionsEnabled();
}

void MainWindow::navigateForward() {
    while (!navForward_.isEmpty()) {
        const NavEntry entry = navForward_.takeLast();
        const NavEntry here = currentNavEntry();
        if (goToNavEntry(entry)) {
            if (!here.path.isEmpty()) navBack_.append(here);
            break;
        }
    }
    updateNavActionsEnabled();
}

void MainWindow::updateNavActionsEnabled() {
    if (navBackAction_) navBackAction_->setEnabled(!navBack_.isEmpty());
    if (navForwardAction_) navForwardAction_->setEnabled(!navForward_.isEmpty());
}

void MainWindow::restartLanguageServer() {
    LspManager::instance()->restart();
    statusBar()->show();
    statusBar()->showMessage("Restarting language server…", 2000);
}

void MainWindow::updateDiagnosticStatus() {
    EditorView* v = editorView();
    if (!v) return;

    const auto& diagnostics = v->diagnostics();
    if (diagnostics.isEmpty()) {
        statusBar()->clearMessage();
        statusBar()->hide();
        return;
    }

    // Prefer the diagnostic under the caret; fall back to a count so the user
    // knows something is wrong even when the caret is elsewhere.
    QString message;
    if (const LspDiagnostic* d = v->diagnosticAt(v->cursorPos())) {
        message = d->message;
        // A dependency error is drawn on the `(load "...")` form, not at the
        // code that is actually wrong, so say so -- otherwise the squiggle
        // reads as "this load is malformed". The message already names the
        // real file and line; what the prefix adds is that the fault is not
        // here, and that there is somewhere to go.
        if (d->isFromDependency()) {
            message = QString("Error in a dependency — %1  (%2 to open it)")
                          .arg(message, kGoToDiagnosticSourceShortcut);
        }
    }
    if (message.isEmpty()) {
        int errors = 0;
        for (const LspDiagnostic& d : diagnostics) {
            if (d.severity <= LspDiagnostic::Error) errors++;
        }
        const int warnings = int(diagnostics.size()) - errors;
        QStringList parts;
        if (errors > 0) parts << QString("%1 error%2").arg(errors).arg(errors == 1 ? "" : "s");
        if (warnings > 0) parts << QString("%1 warning%2").arg(warnings).arg(warnings == 1 ? "" : "s");
        message = parts.join(", ");
    }

    statusBar()->show();
    statusBar()->showMessage(message);
}

void MainWindow::focusEditor() {
    if (EditorView* v = editorView()) v->setFocus();
}

void MainWindow::focusRepl() {
    if (terminal_) terminal_->setFocus();
}

void MainWindow::toggleReplEditorFocus() {
    if (terminal_ && terminal_->hasFocus()) {
        focusEditor();
    } else {
        focusRepl();
    }
}

void MainWindow::nextTab() {
    if (buffers_.empty()) return;
    activateBuffer((activeIndex_ + 1) % static_cast<int>(buffers_.size()));
}

void MainWindow::prevTab() {
    if (buffers_.empty()) return;
    const int n = static_cast<int>(buffers_.size());
    activateBuffer((activeIndex_ - 1 + n) % n);
}

void MainWindow::closeCurrentTab() {
    closeBuffer(activeIndex_);
}

void MainWindow::openSettingsDirectory(const QString& relPath) {
    const QString path = QDir(QDir::homePath()).filePath(relPath);
    QDir().mkpath(path);
    const QString abs = QFileInfo(path).absoluteFilePath();
    for (int i = 0; i < static_cast<int>(buffers_.size()); ++i) {
        TabContent* v = buffers_[i]->view;
        if (!v || v->kind() != TabContent::Kind::Directory) continue;
        if (QFileInfo(v->filePath()).absoluteFilePath() == abs) {
            activateBuffer(i);
            return;
        }
    }
    openDirectory(abs);
}

void MainWindow::openPreferences() {
    for (int i = 0; i < static_cast<int>(buffers_.size()); ++i) {
        TabContent* v = buffers_[i]->view;
        if (v && v->kind() == TabContent::Kind::Preferences) {
            activateBuffer(i);
            return;
        }
    }

    auto* prefs = new PreferencesView(editorStack_);
    connect(prefs, &PreferencesView::rainbowBracketsChanged,
            this, &MainWindow::applyRainbowBrackets);
    connect(prefs, &PreferencesView::bracketPairGuidesChanged,
            this, &MainWindow::applyBracketPairGuides);

    // Reuse a fresh, empty, unmodified Untitled editor if available.
    if (activeIndex_ >= 0 && activeIndex_ < static_cast<int>(buffers_.size())) {
        Buffer* cur = buffers_[activeIndex_].get();
        if (cur->view && cur->view->kind() == TabContent::Kind::Editor) {
            auto* ev = static_cast<EditorView*>(cur->view);
            if (ev->filePath().isEmpty() && ev->isEmpty() && !ev->isModified()) {
                editorStack_->addWidget(prefs);
                editorStack_->removeWidget(ev);
                ev->deleteLater();
                cur->view = prefs;
                cur->untitledIndex = 0;
                connectBufferSignals(activeIndex_);
                editorStack_->setCurrentWidget(prefs);
                updateBufferDisplayName(activeIndex_);
                updateWindowTitle();
                updateEditorActionsEnabled();
                return;
            }
        }
    }

    auto buf = std::make_unique<Buffer>();
    buf->view = prefs;
    editorStack_->addWidget(prefs);
    buf->displayName = computeDisplayName(*buf);
    buffers_.push_back(std::move(buf));
    const int newIndex = static_cast<int>(buffers_.size()) - 1;
    connectBufferSignals(newIndex);
    activateBuffer(newIndex);
    refreshTabBar();
}

void MainWindow::applyRainbowBrackets(bool enabled) {
    for (auto& b : buffers_) {
        if (b->view && b->view->kind() == TabContent::Kind::Editor) {
            auto* editor = static_cast<EditorView*>(b->view);
            editor->setRainbowBrackets(enabled);
            // The guide takes its colour from the pair's depth style, which
            // just changed out from under it in both directions.
            editor->updateBracketGuide();
        }
    }
}

void MainWindow::applyBracketPairGuides(bool enabled) {
    for (auto& b : buffers_) {
        if (b->view && b->view->kind() == TabContent::Kind::Editor) {
            static_cast<EditorView*>(b->view)->setBracketPairGuides(enabled);
        }
    }
}

QString MainWindow::replWorkingDir() const {
    if (EditorView* v = editorView()) {
        const QString path = v->filePath();
        if (!path.isEmpty()) return QFileInfo(path).absolutePath();
    }
    return QDir::homePath();
}

namespace {

// Local-file paths carried by a drag, in order. Non-file URLs (http://, and
// so on) are ignored rather than opened.
QStringList DroppedPaths(const QMimeData* mime) {
    QStringList paths;
    if (!mime || !mime->hasUrls()) return paths;
    for (const QUrl& url : mime->urls()) {
        if (!url.isLocalFile()) continue;
        const QString local = url.toLocalFile();
        if (!local.isEmpty()) paths << local;
    }
    return paths;
}

}  // namespace

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    // Only claim the drag if it actually carries local files; leaving it
    // unaccepted otherwise lets the drop fall through to whatever else wants it.
    if (DroppedPaths(event->mimeData()).isEmpty()) return;
    event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event) {
    const QStringList paths = DroppedPaths(event->mimeData());
    if (paths.isEmpty()) return;
    event->acceptProposedAction();
    openDropped(paths);
    raise();
    activateWindow();
}

bool MainWindow::openDropped(const QStringList& paths) {
    if (paths.isEmpty()) return false;

    const QString first = QFileInfo(paths.first()).absoluteFilePath();
    bool ok = true;

    // A drop replaces the current tab — but not when there is no tab to
    // replace, and not when the file is already open in this window, where the
    // focus-existing-tab rule wins (replacing would leave two tabs on one file).
    if (buffers_.empty() || activeIndex_ < 0 || indexOfPath(first) >= 0) {
        ok = openAny(first);
    } else {
        // Replacing discards whatever the tab held, so offer to save first.
        if (!maybeSaveBuffer(activeIndex_)) return false;
        ok = replaceBufferWithPath(activeIndex_, first);
    }

    // Anything else in the same drop opens as an additional tab.
    for (int i = 1; i < paths.size(); ++i) {
        ok = openAny(paths[i]) && ok;
    }
    return ok;
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (!maybeSaveAll()) {
        event->ignore();
        return;
    }
    if (repl_) repl_->stop();
    if (debug_) debug_->stop();
    persistGlobals();
    // Leave the registry before the deferred delete: WA_DeleteOnClose runs on
    // the next event-loop pass, so anything checking count() in between (a
    // quit, or the "was that the last window?" decision) must not still see us.
    if (windows_) {
        windows_->forget(this);
        // Closing a window drops it from the saved session, so rewrite what
        // survives. During a quit we skip this: quitApp() already snapshotted
        // the full set, and rewriting per close would erase it window by
        // window until nothing was left to restore.
        //
        // Except when this was the *last* window: `forget` has already removed
        // it, so "what survives" is nothing and persistAll would write an empty
        // session — losing geometry, splitter, tabs and breakpoints. Closing
        // the last window is how a session ordinarily ends, so its state *is*
        // the session.
        if (!windows_->isQuitting()) {
            if (windows_->count() == 0) windows_->persistClosing(this);
            else windows_->persistAll();
        }
    }
    event->accept();
}

void MainWindow::updateWindowTitle() {
    const bool haveBuffer =
        activeIndex_ >= 0 && activeIndex_ < static_cast<int>(buffers_.size());
    TabContent* v = haveBuffer ? buffers_[activeIndex_]->view : nullptr;

    QString title = QStringLiteral("Trowel");
    if (v) {
        const QString name = buffers_[activeIndex_]->displayName;
        const QString marker = v->isModified() ? " •" : "";
        title = QString("%1%2 — Trowel").arg(name, marker);
        setWindowFilePath(v->kind() == TabContent::Kind::Editor ? v->filePath() : QString());
        setWindowModified(v->isModified());
    }
    if (windowTitle() == title) return;

    setWindowTitle(title);
    // Every window's Window menu lists these titles, so a change has to reach
    // the other windows' menus too — otherwise they keep showing the "Trowel"
    // a window was called before it had loaded anything.
    if (windows_) windows_->notifyWindowsChanged();
}

void MainWindow::startSession() {
    // Guard: a window only ever gets one session. Cheap insurance against a
    // caller that both restores and calls newWindow()-style setup.
    if (repl_) return;

    ensureAtLeastOneBuffer();
    activateBuffer(activeIndex_ < 0 ? 0 : activeIndex_);

    // The breakpoint model is per-window (matching the session scope) and
    // survives across debug sessions — "restart" respawns `tur dap`, but the
    // breakpoints persist.
    if (!breakpoints_) {
        breakpoints_ = new BreakpointModel(this);
        connect(breakpoints_, &BreakpointModel::changed, this,
                [this](const QString& path) { refreshBreakpointMarkers(path); });

        // The breakpoints panel is wired once, here, rather than per session:
        // breakpoints outlive every session (one program per session,
        // constraint 3 — "restart" respawns `tur dap`), so the panel has to
        // work with no session at all.
        if (replPane_ && replPane_->debugger()) {
            auto* dv = replPane_->debugger();
            connect(dv, &DebuggerView::breakpointEnableToggled, this,
                    [this](const QString& path, int line, bool enabled) {
                breakpoints_->setEnabled(path, line, enabled);
                if (debug_ && debug_->state() == DebugSession::State::Paused) {
                    pushBreakpointsToSession();
                }
            });
            connect(dv, &DebuggerView::breakpointConditionEdited, this,
                    [this](const QString& path, int line, const QString& cond) {
                breakpoints_->setCondition(path, line, cond);
                if (debug_ && debug_->state() == DebugSession::State::Paused) {
                    pushBreakpointsToSession();
                }
            });
            connect(dv, &DebuggerView::breakpointRemoved, this,
                    [this](const QString& path, int line) {
                breakpoints_->remove(path, line);
                if (debug_ && debug_->state() == DebugSession::State::Paused) {
                    pushBreakpointsToSession();
                }
            });
            connect(dv, &DebuggerView::breakpointActivated, this,
                    [this](const QString& path, int line) {
                // Jump to it, and put the jump on the nav stack so Back
                // returns — the same treatment a definition jump gets.
                const NavEntry origin = currentNavEntry();
                if (!openPath(path)) return;
                if (EditorView* ed = editorView()) {
                    ed->setCursorPos(ed->posFromLineCol(line - 1, 0));
                    ed->sciWidget()->scrollCaret();
                }
                pushNavHistory(origin);
            });
        }
    }

    // Started last, so replWorkingDir() sees whatever this window actually
    // ended up holding — a restored session, a file opened from the CLI, or
    // nothing at all (a blank window roots the REPL at $HOME).
    repl_ = new ReplSession(terminal_, this);
    repl_->start(replWorkingDir(), replDialectForActiveBuffer());
}

void MainWindow::applySessionState(const QVariantMap& state) {
    if (state.contains("geometry")) {
        restoreGeometry(state.value("geometry").toByteArray());
    }
    if (state.contains("splitterState")) {
        splitter_->restoreState(state.value("splitterState").toByteArray());
    }
    const bool replVisible = state.value("replVisible", true).toBool();
    if (terminal_) terminal_->setVisible(replVisible);
    if (toggleReplAction_) toggleReplAction_->setChecked(replVisible);

    const QStringList openPaths = state.value("openBuffers").toStringList();
    int desiredActive = state.value("activeBuffer", 0).toInt();

    static const QString kDirPrefix = QStringLiteral("dir://");
    for (const QString& entry : openPaths) {
        const bool isDir = entry.startsWith(kDirPrefix);
        const QString path = isDir ? entry.mid(kDirPrefix.size()) : entry;
        if (!QFileInfo::exists(path)) {
            statusBar()->showMessage(QString("Path no longer exists: %1").arg(path), 4000);
            continue;
        }
        if (isDir) {
            openDirectory(path);
        } else {
            addBuffer(path, /*untitledIfEmpty=*/false);
        }
    }

    if (!buffers_.empty()) {
        if (desiredActive < 0 || desiredActive >= static_cast<int>(buffers_.size())) {
            desiredActive = 0;
        }
        activeIndex_ = desiredActive;
        editorStack_->setCurrentWidget(buffers_[activeIndex_]->view);
    }
    refreshTabBar();
    updateEditorActionsEnabled();

    // Restore the REPL-pane tab (REPL=0, Debugger=1). Default REPL so a
    // restored session doesn't open on the (likely empty) Debugger tab.
    if (replPane_) {
        const int paneTab = state.value("replPaneTab", 0).toInt();
        replPane_->setActiveTab(paneTab);
    }

    // Breakpoints survive a restart. They are a deliberate annotation on the
    // source, not session scratch — losing them on quit is the thing that
    // makes people stop using a debugger's gutter.
    if (breakpoints_) {
        for (const QVariant& v : state.value("breakpoints").toList()) {
            const QVariantMap m = v.toMap();
            const QString path = m.value("path").toString();
            const int line = m.value("line").toInt();
            if (path.isEmpty() || line < 1) continue;
            // A file that has since been deleted or moved would otherwise
            // accumulate breakpoints nothing can ever reach or clear.
            if (!QFileInfo::exists(path)) continue;
            breakpoints_->set(path, line, m.value("enabled", true).toBool(),
                              m.value("condition").toString());
        }
    }
}

QVariantMap MainWindow::sessionState() const {
    QVariantMap state;
    state["geometry"] = saveGeometry();
    state["splitterState"] = splitter_->saveState();
    state["replVisible"] = toggleReplAction_ ? toggleReplAction_->isChecked() : true;

    QStringList openPaths;
    int activeInList = -1;
    for (int i = 0; i < static_cast<int>(buffers_.size()); ++i) {
        TabContent* v = buffers_[i]->view;
        if (!v) continue;
        const QString path = v->filePath();
        if (path.isEmpty()) continue;  // Untitled buffers have nothing to point at
        // A stdlib path is stable across upgrades, so restoring one would
        // silently present *last version's* stdlib as current after a
        // TROWEL_TURMERIC_VERSION bump. Stale and indistinguishable from fresh
        // is worse than absent.
        //
        // Skipped before activeInList is set, so dropping the active tab cannot
        // leave the index pointing at whichever buffer happened to follow it.
        if (isStdlibPath(path)) continue;
        if (i == activeIndex_) activeInList = openPaths.size();
        if (v->kind() == TabContent::Kind::Directory) {
            openPaths << (QStringLiteral("dir://") + path);
        } else {
            openPaths << path;
        }
    }
    state["openBuffers"] = openPaths;
    state["activeBuffer"] = activeInList < 0 ? 0 : activeInList;
    // Which REPL-pane tab was visible (REPL=0, Debugger=1). Default REPL.
    state["replPaneTab"] = replPane_ ? replPane_->activeTab() : 0;

    // Breakpoints, with their enabled flag and condition. Saved even for files
    // not currently open — a breakpoint is an annotation on a path, and the
    // buffer being closed does not retract it.
    if (breakpoints_) {
        QVariantList bps;
        for (const auto& bp : breakpoints_->breakpoints()) {
            bps << QVariantMap{{"path", bp.path},
                               {"line", bp.line},
                               {"enabled", bp.enabled},
                               {"condition", bp.condition}};
        }
        state["breakpoints"] = bps;
    }
    return state;
}

void MainWindow::persistGlobals() {
    QSettings settings;
    if (EditorView* v = editorView()) {
        settings.setValue("editorFont", v->currentFont());
    }
    // Merge rather than overwrite. Each window carries its own recent-files
    // list, so a plain write would let whichever window closed last throw away
    // everything the others had opened. This window's entries stay in front.
    QStringList merged = recentFiles_;
    for (const QString& path : settings.value("recentFiles").toStringList()) {
        if (!merged.contains(path)) merged << path;
    }
    while (merged.size() > 8) merged.removeLast();
    settings.setValue("recentFiles", merged);
}

}
