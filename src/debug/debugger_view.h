#pragma once

#include "debug/breakpoint_model.h"
#include "debug/debug_session.h"

#include <QStringList>
#include <QVector>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSplitter;
class QToolBar;
class QTreeWidget;

namespace trowel {

struct Theme;

// A QTreeWidget that paints centred, dimmed text when it has no rows, instead
// of being an empty box — three blank rectangles is what this pane looked like
// before any session had run, and a blank rectangle is indistinguishable from
// a broken one. Qt has no placeholder for item views.
//
// Defined in the .cpp, like `BracketGuideOverlay`: it declares no signals or
// slots, so it needs no moc. Held by concrete type rather than as QTreeWidget*
// precisely because it has no Q_OBJECT and therefore cannot be qobject_cast.
class PlaceholderTree;
class TimelineStrip;

// The Debugger tab's contents: a stepping toolbar, the call stack, variables
// and breakpoints panes, and a read-only output console with an evaluate line.
// Lives in the REPL pane (not the editor stack), so it is a plain QWidget,
// not a TabContent.
class DebuggerView : public QWidget {
    Q_OBJECT
public:
    explicit DebuggerView(QWidget* parent = nullptr);

    // Restyle from the editor theme. Everything here is *derived* rather than
    // read from new theme keys — headers from the line-number colour, accents
    // from the matched-brace colour, rules from the editor background — so a
    // user's own theme file styles the debugger without having to know the
    // debugger exists.
    void applyTheme(const Theme& theme);

    // The time-travel scrubber. Hidden unless a replay session is running
    // against a `tur` that advertises the timeline extension — a scrubber for
    // a recording that does not exist is not a disabled control, it is a lie
    // about what this session can do.
    TimelineStrip* timeline() const { return timeline_; }
    void setTimelineVisible(bool visible);

    // Replace the console outright, for a backwards seek. `appendOutput` grows
    // it; this swaps it, which is what makes the console rewind with the
    // cursor (T5).
    void setOutput(const QString& text);

    // Append a chunk of debuggee output (from a DAP `output` event).
    void appendOutput(const QString& text);
    // Append one evaluate round trip, marked so a result is not mistaken for
    // program output.
    void appendEvaluation(const QString& expression, const QString& result, bool ok);
    // Clear the output console.
    void clearOutput();

    // Replace the call stack. `selectedId` is the frame to highlight; the row
    // index and the frame id are the same number (constraint 11), but they are
    // passed separately so this does not silently depend on that.
    void setFrames(const QVector<DebugSession::Frame>& frames, int selectedId);
    // Replace the variables pane.
    void setVariables(const QVector<DebugSession::Variable>& vars);

    // Replace the breakpoints panel — a view over the model, never the model
    // itself. `collidingBasenames` are basenames that appear more than once
    // among the open buffers: `tur dap` matches breakpoints by basename
    // (constraint 5), so those breakpoints silently apply to both files, and a
    // warning row says so. Silently wrong is much worse than loudly limited.
    void setBreakpoints(const QVector<BreakpointModel::Breakpoint>& bps,
                        const QStringList& collidingBasenames);

    // Enable/disable the toolbar buttons from the session state machine:
    // everything except Stop is disabled unless Paused.
    void setPaused(bool paused);
    void setRunning(bool running);

    // Show or hide the reverse-execution buttons. They mean nothing in a live
    // session — the adapter only serves them from a recording — so they are
    // hidden rather than disabled, which would imply they might light up.
    void setReverseAvailable(bool available);

    // Turn the evaluate line on or off. `whyNot` becomes its placeholder when
    // disabled, so the reason is where the box is rather than in a per-keystroke
    // error (the replay case: "there is no live frame").
    void setEvaluateEnabled(bool enabled, const QString& whyNot = {});

signals:
    // Toolbar actions. MainWindow connects these to the live DebugSession.
    void continueRequested();
    void stepOverRequested();
    void stepInRequested();
    void stepOutRequested();
    void stopRequested();
    // Reverse execution, served from a recording (T3).
    void stepBackRequested();
    void reverseStepOverRequested();
    void reverseContinueRequested();

    // A row in the call stack was picked. Carries the frame id.
    void frameSelected(int frameId);
    // The user submitted an expression on the evaluate line.
    void evaluateRequested(const QString& expression);

    // Breakpoints panel. The panel reports intent; the owner edits the model,
    // and the model's `changed` brings the new state back here. The panel
    // never writes to the model directly — that is what keeps it a view.
    void breakpointEnableToggled(const QString& path, int line, bool enabled);
    void breakpointConditionEdited(const QString& path, int line, const QString& condition);
    void breakpointActivated(const QString& path, int line);
    void breakpointRemoved(const QString& path, int line);

private:
    // Re-derive the toolbar icons in the current accent colour. Icons are
    // rasterized per colour, so this runs on every theme change.
    void restyleIcons();

    QToolBar* toolbar_;
    TimelineStrip* timeline_;
    QLabel* warning_;
    QSplitter* panes_;
    QSplitter* vsplit_;
    PlaceholderTree* stack_;
    PlaceholderTree* variables_;
    PlaceholderTree* breakpoints_;
    QPlainTextEdit* console_;
    // The evaluate line: a `turi>` prompt and the field, in one row so the
    // rule above them spans both and they read as a single REPL line.
    QWidget* evalRow_;
    QLabel* evalPrompt_;
    QLineEdit* evalInput_;
    // Enable/disable the prompt and the field together — the prompt dims with
    // the line it belongs to, rather than staying lit above a dead field.
    void setEvalLineEnabled(bool enabled);
    // Actions kept by hand rather than read back off the toolbar by index:
    // the index arithmetic that `setPaused` used to do broke the moment the
    // reverse buttons made the toolbar's length conditional.
    QVector<QAction*> stepActions_;
    QVector<QAction*> reverseActions_;
    QAction* stopAction_ = nullptr;
    // Whether this session can evaluate at all, independent of whether it is
    // paused right now. False in a recording: there is no live frame.
    bool evaluateAllowed_ = true;
    QColor accent_;
    QColor dim_;
};

}
