#include "repl/repl_session.h"

#include "repl/pty_session.h"
#include "repl/terminal_view.h"
#include "repl/tur_invocation.h"

#include <QDir>
#include <QFileInfo>
#include <QUrl>

namespace trowel {

namespace {

// Replace a leading `home` with `~`, or return empty when it isn't a prefix.
QString abbreviateHome(const QString& path, const QString& home) {
    if (path.isEmpty() || home.isEmpty()) return {};
    if (path == home) return QStringLiteral("~");
    if (path.startsWith(home + QLatin1Char('/'))) {
        return QLatin1Char('~') + path.mid(home.size());
    }
    return {};
}

// Render a directory for a banner, abbreviating the user's home to `~` the way
// a shell prompt would.
QString displayPath(const QString& path) {
    if (path.isEmpty()) return QStringLiteral("(inherited)");

    QString shown = abbreviateHome(path, QDir::homePath());
    if (shown.isEmpty()) {
        // Try again canonically. A cwd reported by the REPL comes from
        // getcwd(), which resolves symlinks (/private/var/… on macOS), while
        // QDir::homePath() does not — so the literal prefix test above misses
        // and the home directory would be printed in full.
        shown = abbreviateHome(QFileInfo(path).canonicalFilePath(),
                               QFileInfo(QDir::homePath()).canonicalFilePath());
    }
    return shown.isEmpty() ? path : shown;
}

} // namespace

ReplSession::ReplSession(TerminalView* view, QObject* parent)
    : QObject(parent)
    , view_(view)
{}

bool ReplSession::isRunning() const {
    return pty_ && pty_->isRunning();
}

void ReplSession::start(const QString& workingDir, Dialect dialect) {
    if (isRunning()) return;

    lastWorkingDir_ = workingDir;

    if (pty_) {
        pty_->deleteLater();
        pty_ = nullptr;
    }
    pty_ = new PtySession(this);
    view_->attach(pty_);

    connect(pty_, &PtySession::finished, this, &ReplSession::onFinished);
    connect(pty_, &PtySession::startFailed, this, &ReplSession::onStartFailed);
    connect(pty_, &PtySession::dataReceived, this, &ReplSession::onPtyData);

    // Each REPL launch starts busy; the first prompt marker flips us idle.
    busy_ = true;
    scanTail_.clear();

    // `tur repl --lang <base>` starts the session in a dialect, which is
    // cheaper and more honest than starting in Turmeric and immediately
    // switching: the switch resets the environment, so the first thing the user
    // would see in a Scheme project is a session being thrown away.
    //
    // Turmeric is passed as NO FLAG rather than `--lang turmeric`, so a plain
    // start is byte-identical to what it always was.
    //
    // NOTE: this flag takes a BASE (`saffron/sweet`, `r7rs/sweet`) and rejects
    // reader spellings -- `--lang sweet-exp` and `--lang scheme` are both
    // "unknown --lang". `tur fmt --lang` is the other way round; see
    // DialectFmtLangFlag.
    QStringList subcommand{"repl"};
    if (dialect != Dialect::Turmeric) {
        subcommand << "--lang" << QString::fromLatin1(DialectBaseToken(dialect));
    }

    const TurInvocation inv = MakeTurInvocation(subcommand, workingDir);
    if (inv.binary.isEmpty()) {
        view_->showBanner(inv.error);
        return;
    }

    turBinary_ = inv.binary;
    if (!pty_->start(inv.binary, inv.args, workingDir, inv.envEntries())) {
        // startFailed will fire and report.
        return;
    }
    dialect_ = dialect;
    lastStartDialect_ = dialect;
    dirtied_ = false;
    emit dialectChanged(dialect_);
    onStarted();
}

void ReplSession::restartIn(const QString& workingDir, Dialect dialect) {
    stop();
    start(workingDir.isEmpty() ? lastWorkingDir_ : workingDir, dialect);
}

bool ReplSession::switchDialect(Dialect d) {
    if (!isRunning()) return false;
    if (d == dialect_) return true;
    // The REPL's own `#lang` handler does the work, including the environment
    // reset. dialect_ is NOT updated here: it moves when the acknowledgement
    // comes back (scanDialectReports), so a switch the REPL refused does not
    // leave us believing it happened.
    return sendCommand(QByteArray("#lang ") + DialectBaseToken(d));
}

void ReplSession::restart(const QString& workingDir) {
    stop();
    view_->showBanner("[trowel] restarting REPL…");
    start(workingDir.isEmpty() ? lastWorkingDir_ : workingDir, lastStartDialect_);
}

void ReplSession::stop() {
    if (!pty_) return;
    pty_->terminate();
}

bool ReplSession::sendCommand(const QByteArray& line) {
    if (!isRunning()) return false;
    // Anything sent to the prompt may define something, so the session counts
    // as dirtied -- except the three things that RESET it, after which there is
    // nothing left for a dialect switch to discard. `:run` installs a fresh
    // environment of its own (see run_buffer.cpp), and a `#lang` switch rewinds
    // to the pinned preload.
    const QByteArray trimmed = line.trimmed();
    const bool resets = trimmed == ":reset"
                        || trimmed.startsWith(":run")
                        || trimmed.startsWith("#lang ");
    dirtied_ = !resets;
    QByteArray payload = line;
    if (!payload.endsWith('\r') && !payload.endsWith('\n')) payload.append('\r');
    // Optimistically enter busy state. The next OSC 133;A from the REPL flips
    // back to idle. If the child doesn't emit markers we simply stay busy,
    // which just disables clearRepl's prompt-redraw and hurts nothing else.
    setBusy(true);
    pty_->write(payload);
    return true;
}

void ReplSession::redrawPrompt() {
    if (!isRunning() || busy_) return;
    // Ctrl+L = libedit's ed-clear-screen: emits terminal clear codes (which
    // TerminalView drops as unknown CSI) then redraws the prompt with any
    // in-progress input. Trowel has already wiped its own document; this just
    // brings the prompt line back.
    pty_->write(QByteArray("\x0c"));
}

void ReplSession::setBusy(bool busy) {
    if (busy_ == busy) return;
    busy_ = busy;
    emit busyChanged(busy_);
}

// Scan a byte stream for OSC 133;A (prompt-start) markers. On match, flip idle.
// Handles chunk boundaries by carrying an unterminated OSC tail into scanTail_.
// Format: ESC ] 1 3 3 ; A  {BEL | ESC \}
void ReplSession::onPtyData(const QByteArray& bytes) {
    static constexpr char kPromptA[] = "\x1b]133;A";
    static constexpr int kPromptALen = sizeof(kPromptA) - 1;

    QByteArray buf = scanTail_ + bytes;
    scanTail_.clear();

    int i = 0;
    while ((i = buf.indexOf(kPromptA, i)) >= 0) {
        int term = i + kPromptALen;
        if (term >= buf.size()) break; // partial — need more bytes
        char t = buf.at(term);
        if (t == '\x07') {
            setBusy(false);
            i = term + 1;
        } else if (t == '\x1b') {
            if (term + 1 >= buf.size()) break; // partial ST
            if (buf.at(term + 1) == '\\') {
                setBusy(false);
                i = term + 2;
            } else {
                i = term + 1;
            }
        } else {
            i = term;
        }
    }

    // Preserve any trailing partial ESC sequence for the next chunk. Cap the
    // carry so a stream full of stray ESCs can't grow unbounded.
    int lastEsc = buf.lastIndexOf('\x1b');
    if (lastEsc >= 0 && buf.size() - lastEsc <= 32) {
        scanTail_ = buf.mid(lastEsc);
    }

    scanCwdReports(bytes);
    scanDialectReports(bytes);
    scanCommandRequests(bytes);

    emit dataReceived(bytes);
}

// Scan for the REPL's `#lang` acknowledgements.
//
// Three wordings, all pinned upstream by tests/run-flags.sh:
//
//   ; language set to r7rs, reader r7rs (session reset)
//   ; reader set to turmeric/sweet (session reset)
//   ; reader already set to turmeric (saffron)
//
// Only the first names the LANGUAGE, which is the half that matters here -- a
// reader-only switch leaves the prelude alone. The line is matched rather than
// the send trusted, so a `#lang` the user typed into the pane directly is
// picked up too, and one the REPL refused is not.
//
// Matched on plain text, not an OSC sequence, because it is ordinary REPL
// output. That makes it best-effort by nature: it is scanned out of a byte
// stream that may split mid-line. Missing one costs a stale `dialect()` until
// the next switch, which degrades to an unnecessary `#lang` send -- harmless,
// since the REPL answers "already set to" and resets nothing.
void ReplSession::scanDialectReports(const QByteArray& bytes) {
    static constexpr char kPrefix[] = "; language set to ";
    static constexpr int kPrefixLen = sizeof(kPrefix) - 1;

    int i = 0;
    while ((i = bytes.indexOf(kPrefix, i)) >= 0) {
        const int nameStart = i + kPrefixLen;
        const int comma = bytes.indexOf(',', nameStart);
        if (comma < 0) break;  // split across reads; the next switch re-syncs
        const QByteArray language = bytes.mid(nameStart, comma - nameStart).trimmed();

        // `reader <name>` follows. The pair is what names a base, and
        // reader_type_name returns the fully-qualified Turmeric spelling
        // ("turmeric/sweet") while Scheme's readers answer "r7rs" and
        // "r7rs/sweet", so the base is reassembled rather than parsed.
        const int readerAt = bytes.indexOf("reader ", comma);
        QByteArray reader;
        if (readerAt >= 0) {
            int e = readerAt + 7;
            while (e < bytes.size() && bytes[e] != ' ' && bytes[e] != '\n'
                   && bytes[e] != '\r' && bytes[e] != '(') {
                ++e;
            }
            reader = bytes.mid(readerAt + 7, e - readerAt - 7).trimmed();
        }

        Dialect found = Dialect::Turmeric;
        bool ok = false;
        // A sweet reader under a dynamic language is spelled by the language
        // plus the reader's suffix; everything else is named by one of the two
        // halves on its own.
        if (reader.endsWith("/sweet") || reader == "sweet") {
            ok = DialectFromBaseToken(language + "/sweet", found);
        }
        if (!ok) ok = DialectFromBaseToken(language, found);
        if (!ok && !reader.isEmpty()) ok = DialectFromBaseToken(reader, found);

        if (ok && found != dialect_) {
            dialect_ = found;
            dirtied_ = false;  // a language switch resets the environment
            emit dialectChanged(dialect_);
        }
        i = comma;
    }
}

// Scan for OSC 7 cwd reports: ESC ] 7 ; file://<host>/<path> {BEL | ESC \}.
// `tur repl` emits one at startup and after `:cd`, which moves the live
// process — the one way the working directory changes without a restart.
void ReplSession::scanCwdReports(const QByteArray& bytes) {
    static constexpr char kCwdPrefix[] = "\x1b]7;";
    static constexpr int kCwdPrefixLen = sizeof(kCwdPrefix) - 1;
    // Generous next to the prompt-marker carry, because the payload is a whole
    // path — but still bounded, so a stream of stray ESCs can't grow it.
    static constexpr int kMaxCarry = 4096;

    QByteArray buf = cwdTail_ + bytes;
    cwdTail_.clear();

    int i = 0;
    while ((i = buf.indexOf(kCwdPrefix, i)) >= 0) {
        const int start = i + kCwdPrefixLen;
        int end = -1;
        int next = -1;
        for (int j = start; j < buf.size(); ++j) {
            const char c = buf.at(j);
            if (c == '\x07') { end = j; next = j + 1; break; }
            if (c == '\x1b') {
                if (j + 1 >= buf.size()) break;  // partial ST — need more bytes
                if (buf.at(j + 1) == '\\') { end = j; next = j + 2; }
                break;                           // any other ESC ends this OSC
            }
        }
        if (end < 0) {
            // Unterminated: hold it for the next chunk if it is a plausible size.
            if (buf.size() - i <= kMaxCarry) cwdTail_ = buf.mid(i);
            return;
        }
        applyCwdReport(buf.mid(start, end - start));
        i = next;
    }

    // Hold a trailing partial prefix (down to a bare ESC) for the next read.
    const int lastEsc = buf.lastIndexOf('\x1b');
    if (lastEsc >= 0 && buf.size() - lastEsc < kCwdPrefixLen) {
        cwdTail_ = buf.mid(lastEsc);
    }
}

void ReplSession::applyCwdReport(const QByteArray& uri) {
    const QUrl url(QString::fromUtf8(uri));
    if (!url.isLocalFile()) return;
    const QString dir = url.toLocalFile();
    if (dir.isEmpty()) return;

    // Compare canonically: the REPL reports getcwd(), which resolves symlinks,
    // while we started it with whatever path the editor had. On macOS that
    // alone differs (/tmp vs /private/tmp) and would otherwise announce a move
    // that never happened, on every launch.
    const QString incoming = QFileInfo(dir).canonicalFilePath();
    const QString current = QFileInfo(lastWorkingDir_).canonicalFilePath();
    if (!incoming.isEmpty() && incoming == current) {
        lastWorkingDir_ = dir;  // same place, just spelled differently
        return;
    }

    lastWorkingDir_ = dir;
    view_->showBanner(QString("[trowel] repl cwd is now %1").arg(displayPath(dir)));
    emit workingDirChanged(dir);
}

// P3: scan for OSC 517 command-request sequences.
// Format: ESC ] 5 1 7 ; <command-id> {BEL | ESC \}
// The REPL prints this to ask Trowel to run a command by id.  Trowel looks
// up the id in the CommandRegistry and runs it.
void ReplSession::scanCommandRequests(const QByteArray& bytes) {
    static constexpr char kCmdPrefix[] = "\x1b]517;";
    static constexpr int kCmdPrefixLen = sizeof(kCmdPrefix) - 1;
    static constexpr int kMaxCarry = 1024;

    QByteArray buf = cmdTail_ + bytes;
    cmdTail_.clear();

    int i = 0;
    while ((i = buf.indexOf(kCmdPrefix, i)) >= 0) {
        const int start = i + kCmdPrefixLen;
        int end = -1;
        int next = -1;
        for (int j = start; j < buf.size(); ++j) {
            const char c = buf.at(j);
            if (c == '\x07') { end = j; next = j + 1; break; }
            if (c == '\x1b') {
                if (j + 1 >= buf.size()) break;  // partial ST
                if (buf.at(j + 1) == '\\') { end = j; next = j + 2; }
                break;
            }
        }
        if (end < 0) {
            if (buf.size() - i <= kMaxCarry) cmdTail_ = buf.mid(i);
            return;
        }
        const QString id = QString::fromUtf8(buf.mid(start, end - start));
        if (!id.isEmpty()) emit commandRequested(id);
        i = next;
    }

    const int lastEsc = buf.lastIndexOf('\x1b');
    if (lastEsc >= 0 && buf.size() - lastEsc < kCmdPrefixLen) {
        cmdTail_ = buf.mid(lastEsc);
    }
}

void ReplSession::onStarted() {
    // Say where the REPL is rooted, not just that it started: with a REPL per
    // window, "which directory am I in?" is the question the banner should
    // answer, and the cwd is otherwise invisible until something breaks.
    view_->showBanner(QString("[trowel] %1 repl started in %2")
                          .arg(turBinary_, displayPath(lastWorkingDir_)));
    emit started();
}

void ReplSession::onFinished(int status) {
    view_->showBanner(QString("[trowel] repl exited with code %1").arg(status));
    view_->detach();
    emit stopped(status);
}

void ReplSession::onStartFailed(const QString& message) {
    view_->showBanner(QString("[trowel] failed to start: %1").arg(message));
    emit stopped(-1);
}

}
