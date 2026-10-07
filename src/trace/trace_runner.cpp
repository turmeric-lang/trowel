#include "trace/trace_runner.h"

#include "repl/tur_invocation.h"
#include "repl/terminal_view.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

namespace trowel {

namespace {

// The terminal renders a pty stream, so bare \n from a pipe would stair-step.
QByteArray toTerminalNewlines(const QByteArray& bytes) {
    QByteArray out;
    out.reserve(bytes.size() + 8);
    for (int i = 0; i < bytes.size(); ++i) {
        const char c = bytes.at(i);
        if (c == '\n' && (i == 0 || bytes.at(i - 1) != '\r')) out.append('\r');
        out.append(c);
    }
    return out;
}

// A recording this short means the program was compiled rather than
// interpreted, not that it did nothing. One `ENTER` plus a couple of steps is
// what an annotated `defn` produces: measured at 2 steps for a `main` calling
// an annotated `fib`.
constexpr int kShortRecordingSteps = 16;

}  // namespace

bool DefinesMainEntry(const QByteArray& source) {
    // `(defn main` at the start of a line, allowing leading whitespace and the
    // `defn-` / `defn*` spellings the reader also accepts. Anchored to a line
    // start because a nested `(defn main ...)` inside another form is not the
    // entry point, and unanchored matching would find `main` in a comment
    // about main.
    static const QRegularExpression re(
        R"(^[ \t]*\((?:defn|defn-|defn\*)[ \t]+main[ \t\[\n])",
        QRegularExpression::MultilineOption);
    return re.match(QString::fromUtf8(source)).hasMatch();
}

TraceRunner::TraceRunner(TerminalView* terminal, QObject* parent)
    : QObject(parent)
    , terminal_(terminal)
{}

bool TraceRunner::isRunning() const {
    return proc_ && proc_->state() != QProcess::NotRunning;
}

TraceSummary TraceRunner::ParseSummary(const QString& text) {
    // Two formats, and both must parse:
    //
    //   v0.42.0 and earlier
    //     trace: 6005 steps, 1 enters, … truncated no
    //   after Turmeric e7140c97c
    //     trace: 6005 steps (per expression), 1 enters, … truncated no
    //
    // The `(…)` group is optional rather than two regexes. Trowel pins a
    // binary, so it will meet the old format until the pin moves and the new
    // one after — and a parser that only knew one would report the other as
    // "could not run `tur trace`", which is the least informative possible
    // failure for a version skew.
    static const QRegularExpression re(
        R"(trace:\s+(\d+) steps(?:\s+\((per line|per expression)\))?,\s+)"
        R"((\d+) enters,\s+(\d+) pops,\s+(\d+) changes,\s+)"
        R"(peak depth (\d+),\s+(\d+) bytes,\s+(\d+) bytes of output,\s+truncated (yes|no))");
    const QRegularExpressionMatch m = re.match(text);
    TraceSummary s;
    if (!m.hasMatch()) return s;
    s.parsed = true;
    s.steps = m.captured(1).toInt();
    s.granularity = m.captured(2);  // empty on a pre-e7140c97c binary
    s.enters = m.captured(3).toInt();
    s.pops = m.captured(4).toInt();
    s.changes = m.captured(5).toInt();
    s.peakDepth = m.captured(6).toInt();
    s.bytes = m.captured(7).toInt();
    s.outputBytes = m.captured(8).toInt();
    s.truncated = m.captured(9) == QLatin1String("yes");
    return s;
}

QString TraceRunner::ExplanationFor(TraceOutcome outcome, const TraceSummary& summary) {
    switch (outcome) {
        case TraceOutcome::Failed:
            return QStringLiteral("Could not run `tur trace`.");
        case TraceOutcome::CompileError:
            // The tracer prints a summary even here, so without this the user
            // would see "0 steps" and no reason for it.
            return QStringLiteral(
                "This file does not compile, so nothing was recorded. "
                "Fix the errors above and trace again.");
        case TraceOutcome::NoMain:
            // The advice here used to be "wrap it in `(defn main [] …)`",
            // because `tur trace` recorded only what `(main)` evaluated and a
            // top-level program recorded nothing.  Turmeric v0.44.0 instruments
            // top-level programs too, so that is no longer the reason for a
            // zero recording and no longer sound advice.
            //
            // What still reaches this branch is a file with nothing to execute
            // at all -- measured against the bundled v0.44.2: top-level work
            // records 3 steps, a lone `defn` records 1, and only a comment-only
            // file records 0.
            return QStringLiteral(
                "Recorded 0 steps: there is nothing in this file to run. "
                "Add an expression or a `(defn main [] …)` and trace again.");
        case TraceOutcome::ShortRecording:
            // Two genuinely different causes, and telling the user the wrong
            // one sends them somewhere useless. Under line granularity a short
            // recording usually says nothing about the program at all — it is
            // an artifact of how the source is punctuated.
            if (summary.perLine()) {
                return QStringLiteral(
                    "Recorded only %1 steps. This `tur` records one step per "
                    "source *line*, so a form written on a single line collapses "
                    "to one step however much it evaluates — a one-line loop "
                    "records its counter jumping straight to its final value. "
                    "Reformatting across lines, or a newer `tur` that records "
                    "per expression, gives a fuller recording.")
                    .arg(summary.steps);
            }
            return QStringLiteral(
                "Recorded %1 steps. The program evaluated very little — this is "
                "the whole run, not a truncated one.")
                .arg(summary.steps);
        case TraceOutcome::Recorded:
            return QStringLiteral("Recorded %1 steps, peak depth %2%3.")
                .arg(summary.steps)
                .arg(summary.peakDepth)
                .arg(summary.truncated
                         ? QStringLiteral(" (truncated at the step cap)")
                         : QString());
    }
    return QString();
}

RunResult TraceRunner::run(const QString& filePath) {
    RunResult r;
    if (filePath.isEmpty() || !QFileInfo::exists(filePath)) {
        r.message = QStringLiteral("Save the file before tracing it.");
        return r;
    }
    if (isRunning()) {
        r.message = QStringLiteral("A trace is already running.");
        return r;
    }

    const QString tmpDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    recordingPath_ = QDir(tmpDir).filePath(
        QFileInfo(filePath).completeBaseName() + QStringLiteral(".turtrace"));

    const TurInvocation inv =
        MakeTurInvocation({"trace", filePath, "-o", recordingPath_},
                          QFileInfo(filePath).absolutePath());
    if (inv.binary.isEmpty()) {
        r.message = QStringLiteral("Could not locate `tur` executable.");
        return r;
    }

    if (proc_) {
        proc_->deleteLater();
        proc_ = nullptr;
    }
    transcript_.clear();
    proc_ = new QProcess(this);
    proc_->setProcessChannelMode(QProcess::MergedChannels);
    proc_->setWorkingDirectory(QFileInfo(filePath).absolutePath());
    proc_->setProcessEnvironment(inv.env);

    connect(proc_, &QProcess::readyReadStandardOutput, this, &TraceRunner::onReadyRead);
    connect(proc_, &QProcess::finished, this,
            [this](int, QProcess::ExitStatus) { onFinished(); });

    if (terminal_) {
        terminal_->showBanner(
            QString("[trowel] tur trace %1").arg(QFileInfo(filePath).fileName()));
    }

    proc_->start(inv.binary, inv.args);
    if (!proc_->waitForStarted(3000)) {
        r.message = QStringLiteral("Failed to start `tur trace`.");
        return r;
    }
    r.ok = true;
    r.scratchPath = recordingPath_;
    r.message = QString("Tracing %1…").arg(QFileInfo(filePath).fileName());
    return r;
}

void TraceRunner::onReadyRead() {
    if (!proc_) return;
    const QByteArray chunk = proc_->readAllStandardOutput();
    if (chunk.isEmpty()) return;
    transcript_ += QString::fromUtf8(chunk);
    if (terminal_) terminal_->appendOutput(toTerminalNewlines(chunk));
}

void TraceRunner::onFinished() {
    onReadyRead();  // drain whatever arrived with the exit

    const TraceSummary summary = ParseSummary(transcript_);

    // Classified from the text, never the exit code — see the header.
    TraceOutcome outcome = TraceOutcome::Failed;
    // `error:` on the stream means the compile failed. Checked before the step
    // count, because a broken file still gets a `trace: 0 steps` summary and
    // would otherwise be misreported as "no main".
    static const QRegularExpression errorLine(R"(^.*\berror\b.*$)",
                                              QRegularExpression::MultilineOption);
    if (errorLine.match(transcript_).hasMatch()) {
        outcome = TraceOutcome::CompileError;
    } else if (!summary.parsed) {
        outcome = TraceOutcome::Failed;
    } else if (summary.steps == 0 && summary.enters == 0) {
        outcome = TraceOutcome::NoMain;
    } else if (summary.steps < kShortRecordingSteps) {
        outcome = TraceOutcome::ShortRecording;
    } else {
        outcome = TraceOutcome::Recorded;
    }

    const QString explanation = ExplanationFor(outcome, summary);
    if (terminal_) terminal_->showBanner(QStringLiteral("[trowel] ") + explanation);
    emit finished(outcome, summary, explanation);
}

}
