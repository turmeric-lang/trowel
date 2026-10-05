#pragma once

#include "editor/dialect.h"

#include <QObject>
#include <QString>

namespace trowel {

class PtySession;
class TerminalView;

// Resolve the `tur` executable using the same order as ReplSession::start:
// QSettings "repl/turBinary" override, then bundled binary inside Trowel.app,
// then PATH lookup. Returns an absolute path, or an empty string when none
// of the candidates exist and are executable.
QString ResolveTurBinary();

// The stdlib directory belonging to `turBinary`, or an empty string when it has
// none beside it.
//
// Every `tur` invocation Trowel makes must pin TUR_STDLIB_DIR to this, or the
// ambient environment pairs the binary we chose with somebody else's stdlib.
// That is not hypothetical: this repo's own `mise.toml` pins turmeric 0.42.2,
// and with the pin missing a v0.60.1 `tur lsp` analysed the 0.42.2 stdlib and
// published twelve phantom errors against every clean buffer -- `#fx{Construct}`
// and `#fx{Bt}`, both of which that stdlib spells in the way v0.59.0 turned
// into a hard error.
//
// BOTH archive shapes are probed, which is the whole reason this is a function
// rather than one line repeated at each call site. `stdlib/` sits beside the
// binary in the flat layout and one level up under `share/turmeric/` in the
// prefix layout, and the released .tar.gz targets moved from the first to the
// second after v0.46.0. Five call sites each had their own copy of the flat-only
// probe; one had been fixed and four had not, so crossing that layout change
// silently dropped the pin in the LSP, the formatter, the project runner and the
// tracer while the REPL kept working.
QString TurStdlibDirFor(const QString& turBinary);

class ReplSession : public QObject {
    Q_OBJECT
public:
    explicit ReplSession(TerminalView* view, QObject* parent = nullptr);

    // `dialect` is the `#lang` base the session is started in, via
    // `tur repl --lang <base>`. Turmeric is the toolchain's own default and is
    // passed as no flag at all, so a plain start is unchanged.
    void start(const QString& workingDir = {}, Dialect dialect = Dialect::Turmeric);
    void restart(const QString& workingDir = {});
    void restartIn(const QString& workingDir, Dialect dialect);
    void stop();
    bool isRunning() const;

    // True when the REPL is executing a command (post-sendCommand, pre-prompt).
    // False when the REPL is at its prompt awaiting user input. Determined by
    // OSC 133;A prompt markers emitted by `tur repl`; stays true if the child
    // doesn't emit them (safe default — clearRepl won't redraw the prompt).
    bool isBusy() const { return busy_; }

    // Write `line` (a UTF-8 string) followed by \r to the REPL. Returns false
    // if the REPL isn't running.
    bool sendCommand(const QByteArray& line);

    // Ask the REPL to redraw its prompt (Ctrl+L → libedit ed-clear-screen).
    // No-op when busy — we don't want to interrupt a running command.
    void redrawPrompt();

    void setTurBinary(const QString& path) { turBinary_ = path; }
    QString turBinary() const { return turBinary_; }

    // Directory the REPL was last started in — where it is rooted right now.
    // Empty only before the first start(). A REPL cannot be moved once running
    // (you cannot chdir another process), so this changes only on restart.
    QString workingDir() const { return lastWorkingDir_; }

    PtySession* pty() const { return pty_; }

    // The `#lang` base the LIVE session is in.
    //
    // Tracked rather than assumed because the dialect is session state, not
    // file state: `#lang r7rs` at the prompt resets the environment and loads
    // another prelude, and until that has happened a Scheme buffer cannot run
    // here however it is spelled (see DialectNeedsSessionSwitch).
    //
    // Updated from the REPL's own acknowledgement, not from the send: the user
    // can type `#lang saffron` straight into the REPL pane, and a session we
    // only ever wrote to would then be wrong about itself.
    Dialect dialect() const { return dialect_; }

    // Switch the live session's language. Sends `#lang <base>` and returns
    // false if the REPL is not running. The REPL RESETS its environment on a
    // language switch, discarding accumulated definitions -- `dirtiedSinceReset`
    // is how a caller knows whether that costs the user anything.
    bool switchDialect(Dialect d);

    // Has anything been evaluated in this session since it was last reset?
    // False right after a start, a `:reset`, a `:run`, or a dialect switch.
    bool dirtiedSinceReset() const { return dirtied_; }

signals:
    // The live session's `#lang` base changed, as reported by the REPL.
    void dialectChanged(Dialect d);

signals:
    void started();
    void stopped(int exitCode);
    void dataReceived(const QByteArray& bytes);
    void busyChanged(bool busy);
    // The REPL reported a new working directory (OSC 7) — e.g. after `:cd`,
    // which moves the live process without a restart.
    void workingDirChanged(const QString& dir);
    // The REPL requested a command dispatch (OSC 517).  `id` is the command id
    // to look up in the CommandRegistry and run.
    void commandRequested(const QString& id);

private:
    void onStarted();
    void onFinished(int status);
    void onStartFailed(const QString& message);
    void onPtyData(const QByteArray& bytes);
    void setBusy(bool busy);
    // Scan for OSC 7 cwd reports. Kept separate from the OSC 133 prompt-marker
    // scan above rather than folded into one generic OSC parser: the two carry
    // very different payload sizes (a 7-byte marker vs. a full path), and the
    // prompt-marker path is load-bearing for busy/idle tracking.
    void scanCwdReports(const QByteArray& bytes);
    void applyCwdReport(const QByteArray& uri);
    // Scan for the REPL's own `; language set to <lang>, reader <reader>` /
    // `; reader set to <reader>` acknowledgements, which are the only honest
    // signal that a switch took effect. Pinned wording upstream
    // (tests/run-flags.sh reader-name-canonical, lang-same-base-no-reset).
    void scanDialectReports(const QByteArray& bytes);
    // P3: scan for OSC 517 command-request sequences: the REPL prints
    // ESC ] 5 1 7 ; <command-id> {BEL | ESC \} to ask Trowel to run a command.
    void scanCommandRequests(const QByteArray& bytes);

    TerminalView* view_;
    PtySession* pty_ = nullptr;
    QString turBinary_ = "tur";
    QString lastWorkingDir_;
    Dialect dialect_ = Dialect::Turmeric;
    Dialect lastStartDialect_ = Dialect::Turmeric;
    bool dirtied_ = false;
    // Start busy: idle flips true only after we see the first OSC 133;A. If the
    // child never emits it (older tur, non-tty), we conservatively stay busy so
    // clearRepl doesn't try to poke the process.
    bool busy_ = true;
    // Rolling tail of unmatched bytes (partial OSC sequence spanning chunks).
    QByteArray scanTail_;
    // Same idea for OSC 7, which carries a whole path and so needs a much
    // larger carry than the prompt-marker scan allows.
    QByteArray cwdTail_;
    // Same idea for OSC 517 command requests.
    QByteArray cmdTail_;
};

}
